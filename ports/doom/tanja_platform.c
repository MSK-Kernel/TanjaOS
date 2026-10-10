/*
 * tanja_platform.c - doomgeneric platform layer for TanjaOS (x86, BIOS/VGA).
 *
 *  video : programs VGA mode 13h (320x200x256) directly through the VGA
 *          registers - the kernel runs in protected mode, there is no BIOS
 *          to call.  The complete text-mode state (registers, DAC palette,
 *          font plane) is saved first and restored on exit, so the shell
 *          comes back exactly as it was.
 *  input : polls the PS/2 controller (port 0x60/0x64).  The kernel keyboard
 *          driver is polled too (IRQ1 is masked), so Doom can read raw set-1
 *          scancodes and get real key-up events, which get_key() hides.
 *  timing: kernel millisecond tick (get_uptime_ms / timer_delay_ms).
 *  entry : main(args) runs Doom on a private 256 KiB stack; exit() unwinds
 *          back to the shell with __builtin_longjmp.
 */
#include <stdint.h>
#include <stddef.h>

#include "doomkeys.h"
#include "doomtype.h"
#include "doomgeneric.h"
#include "i_video.h"
#include "d_event.h"

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern uint32_t get_uptime_ms(void);
extern void timer_delay_ms(uint32_t ms);
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern void print(const char* s);

extern boolean palette_changed;
extern byte *I_VideoBuffer;     /* i_video.c (CMAP256 build) */
extern float mouse_acceleration;    /* i_video.c: 2.0, as in vanilla/Chocolate Doom */
extern int   mouse_threshold;       /* i_video.c: 10 */

/* ================================================================== */
/* VGA register access                                                 */
/* ================================================================== */
#define VGA_MEM ((volatile uint8_t*)0xA0000)

static void seq_w(int i, uint8_t v) { outb(0x3C4, (uint8_t)i); outb(0x3C5, v); }
static uint8_t seq_r(int i)         { outb(0x3C4, (uint8_t)i); return inb(0x3C5); }
static void crt_w(int i, uint8_t v) { outb(0x3D4, (uint8_t)i); outb(0x3D5, v); }
static uint8_t crt_r(int i)         { outb(0x3D4, (uint8_t)i); return inb(0x3D5); }
static void gc_w(int i, uint8_t v)  { outb(0x3CE, (uint8_t)i); outb(0x3CF, v); }
static uint8_t gc_r(int i)          { outb(0x3CE, (uint8_t)i); return inb(0x3CF); }

typedef struct {
    uint8_t misc, seq[5], crtc[25], gc[9], ac[21];
} vga_regs_t;

/* Standard BIOS mode 13h register set. */
static const vga_regs_t mode13 = {
    0x63,
    { 0x03, 0x01, 0x0F, 0x00, 0x0E },
    { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00 }
};

static vga_regs_t saved_regs;
static uint8_t saved_dac[768];
static uint8_t saved_font[8192];
static uint16_t saved_text[80 * 25 * 2];   /* text screen as the CPU sees it */
static int gfx_on;

static void vga_read_regs(vga_regs_t* r)
{
    int i;
    r->misc = inb(0x3CC);
    for (i = 0; i < 5;  i++) r->seq[i]  = seq_r(i);
    for (i = 0; i < 25; i++) r->crtc[i] = crt_r(i);
    for (i = 0; i < 9;  i++) r->gc[i]   = gc_r(i);
    for (i = 0; i < 21; i++) {
        (void)inb(0x3DA);           /* reset the attribute flip-flop */
        outb(0x3C0, (uint8_t)i);
        r->ac[i] = inb(0x3C1);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);              /* leave video enabled */
}

static void vga_write_regs(const vga_regs_t* r)
{
    int i;

    (void)inb(0x3DA);
    outb(0x3C0, 0x00);              /* blank the display while reprogramming */

    outb(0x3C2, r->misc);

    seq_w(0, 0x01);                 /* synchronous reset */
    for (i = 1; i < 5; i++) seq_w(i, r->seq[i]);
    seq_w(0, 0x03);                 /* run */

    crt_w(0x11, crt_r(0x11) & 0x7F);        /* unlock CRTC regs 0-7 */
    for (i = 0; i < 25; i++)
        if (i != 0x11) crt_w(i, r->crtc[i]);
    crt_w(0x11, r->crtc[0x11]);

    for (i = 0; i < 9; i++) gc_w(i, r->gc[i]);

    for (i = 0; i < 21; i++) {
        (void)inb(0x3DA);
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, r->ac[i]);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);              /* video on */
}

/* Font plane (plane 2) is only reachable with the sequencer/graphics
 * controller in a "planar, no odd/even" configuration. */
static void font_access_begin(void)
{
    seq_w(0x02, 0x04);              /* write plane 2 only */
    seq_w(0x04, 0x07);              /* sequential, extended memory */
    gc_w(0x04, 0x02);               /* read plane 2 */
    gc_w(0x05, 0x00);               /* write mode 0, no odd/even */
    gc_w(0x06, 0x04);               /* map A0000, 64 KiB */
}

static void font_access_end(const vga_regs_t* r)
{
    seq_w(0x02, r->seq[2]);
    seq_w(0x04, r->seq[4]);
    gc_w(0x04, r->gc[4]);
    gc_w(0x05, r->gc[5]);
    gc_w(0x06, r->gc[6]);
}

static void dac_read_all(uint8_t* out)
{
    int i;
    outb(0x3C7, 0);
    for (i = 0; i < 768; i++) out[i] = inb(0x3C9);
}

static void dac_write_all(const uint8_t* in)
{
    int i;
    outb(0x3C8, 0);
    for (i = 0; i < 768; i++) outb(0x3C9, in[i]);
}

static void gfx_enter(void)
{
    int i;
    if (gfx_on) return;

    vga_read_regs(&saved_regs);
    dac_read_all(saved_dac);
    for (i = 0; i < 80 * 25; i++) saved_text[i] = ((volatile uint16_t*)0xB8000)[i];

    font_access_begin();
    for (i = 0; i < 8192; i++) saved_font[i] = VGA_MEM[i];
    font_access_end(&saved_regs);

    vga_write_regs(&mode13);
    for (i = 0; i < 64000; i++) VGA_MEM[i] = 0;
    gfx_on = 1;
    palette_changed = true;
}

void tanja_gfx_leave(void)
{
    int i;
    if (!gfx_on) return;
    gfx_on = 0;

    vga_write_regs(&saved_regs);

    font_access_begin();
    for (i = 0; i < 8192; i++) VGA_MEM[i] = saved_font[i];
    font_access_end(&saved_regs);

    dac_write_all(saved_dac);

    /* mode 13h clobbered planes 0/1, which hold the text characters */
    for (i = 0; i < 80 * 25; i++) ((volatile uint16_t*)0xB8000)[i] = saved_text[i];
}

int tanja_gfx_active(void) { return gfx_on; }

/* ================================================================== */
/* doomgeneric platform interface                                      */
/* ================================================================== */
static void poll_ps2(void);

pixel_t* tanja_get_screen_buffer(void) { return (pixel_t*)I_VideoBuffer; }

void DG_Init(void)
{
    gfx_enter();
}

void DG_DrawFrame(void)
{
    if (!gfx_on) return;
    poll_ps2();

    if (palette_changed) {
        int i;
        outb(0x3C8, 0);
        for (i = 0; i < 256; i++) {
            outb(0x3C9, colors[i].r >> 2);      /* DAC is 6 bits per channel */
            outb(0x3C9, colors[i].g >> 2);
            outb(0x3C9, colors[i].b >> 2);
        }
        palette_changed = false;
    }

    /*
     * The Doom renderer produces an 8-bit 320x200 frame.  Copy exactly
     * those 64,000 bytes to mode 13h; the old backend treated the frame as
     * 32-bit pixels and copied 256 KiB worth of memory traffic per frame.
     */
    {
        const uint8_t* src = (const uint8_t*)DG_ScreenBuffer;
        volatile uint8_t* dst = VGA_MEM;
        __asm__ volatile(
            "cld\n\t"
            "rep movsl"
            : "+S"(src), "+D"(dst)
            : "c"(64000 / 4)
            : "memory");
    }
}

void DG_SleepMs(uint32_t ms)
{
    timer_delay_ms(ms);
    poll_ps2();                 /* keep draining the mouse while Doom idles */
}
uint32_t DG_GetTicksMs(void)       { return get_uptime_ms(); }
void DG_SetWindowTitle(const char* t) { (void)t; }

/* ---- keyboard: PS/2 set-1 scancodes -> Doom key codes --------------- */
static const uint8_t sc_to_doom[0x59] = {
    /* 00 */ 0, KEY_ESCAPE, '1', '2', '3', '4', '5', '6',
    /* 08 */ '7', '8', '9', '0', KEY_MINUS, KEY_EQUALS, KEY_BACKSPACE, KEY_TAB,
    /* 10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 18 */ 'o', 'p', '[', ']', KEY_ENTER, KEY_RCTRL, 'a', 's',
    /* 20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 28 */ '\'', '`', KEY_RSHIFT, '\\', 'z', 'x', 'c', 'v',
    /* 30 */ 'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, KEYP_MULTIPLY,
    /* 38 */ KEY_RALT, ' ', KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    /* 40 */ KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUMLOCK, KEY_SCRLCK, KEYP_7,
    /* 48 */ KEYP_8, KEYP_9, KEYP_MINUS, KEYP_4, KEYP_5, KEYP_6, KEYP_PLUS, KEYP_1,
    /* 50 */ KEYP_2, KEYP_3, KEYP_0, KEYP_PERIOD, 0, 0, 0, KEY_F11,
    /* 58 */ KEY_F12
};

/* 0xE0-prefixed keys */
static int ext_to_doom(uint8_t sc)
{
    switch (sc) {
    case 0x48: return KEY_UPARROW;
    case 0x50: return KEY_DOWNARROW;
    case 0x4B: return KEY_LEFTARROW;
    case 0x4D: return KEY_RIGHTARROW;
    case 0x1D: return KEY_RCTRL;
    case 0x38: return KEY_RALT;
    case 0x1C: return KEY_ENTER;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;
    case 0x53: return KEY_DEL;
    case 0x35: return KEYP_DIVIDE;
    }
    return 0;
}

static int kb_ext;      /* saw an 0xE0 prefix */
static int kb_skip;     /* bytes of the Pause sequence still to swallow */

/* ---- PS/2 controller: keyboard + mouse share ports 0x60/0x64 --------- */
static int kbc_wait_write(void)
{
    int i;
    for (i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return 1;
    return 0;
}

static int kbc_wait_read(void)
{
    int i;
    for (i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return 1;
    return 0;
}

static void kbc_cmd(uint8_t c)  { kbc_wait_write(); outb(0x64, c); }
static void kbc_data(uint8_t d) { kbc_wait_write(); outb(0x60, d); }

/* send a byte to the mouse (aux port) and wait for its ACK */
static int mouse_send(uint8_t b)
{
    int tries;
    kbc_cmd(0xD4);
    kbc_data(b);
    for (tries = 0; tries < 4; tries++) {
        if (!kbc_wait_read()) return 0;
        if (inb(0x60) == 0xFA) return 1;
    }
    return 0;
}

static int     mouse_on;
static uint8_t saved_kbc_cfg;

static void mouse_init(void)
{
    uint8_t cfg;

    while (inb(0x64) & 1) (void)inb(0x60);      /* flush stale bytes */

    kbc_cmd(0xA8);                              /* enable the aux port */
    kbc_cmd(0x20);                              /* read controller config */
    cfg = kbc_wait_read() ? inb(0x60) : 0x47;
    saved_kbc_cfg = cfg;
    cfg &= (uint8_t)~0x20;                      /* aux clock on */
    cfg &= (uint8_t)~0x02;                      /* aux IRQ off: we poll */
    kbc_cmd(0x60);
    kbc_data(cfg);

    mouse_send(0xF6);                           /* defaults (100 Hz, stream) */
    mouse_on = mouse_send(0xF4);                /* start reporting */
}

static void mouse_shutdown(void)
{
    if (mouse_on) {
        mouse_send(0xF5);                       /* stop reporting */
        mouse_on = 0;
    }
    kbc_cmd(0x60);
    kbc_data(saved_kbc_cfg);
    while (inb(0x64) & 1) (void)inb(0x60);
}

/* key event queue (filled by poll_ps2, drained by DG_GetKey) */
#define KQ_SIZE 64
static struct { uint8_t pressed, key; } kq[KQ_SIZE];
static int kq_head, kq_tail;

static void kq_push(int pressed, int key)
{
    int n = (kq_tail + 1) % KQ_SIZE;
    if (n == kq_head) return;
    kq[kq_tail].pressed = (uint8_t)pressed;
    kq[kq_tail].key = (uint8_t)key;
    kq_tail = n;
}

/* mouse: 3-byte packets accumulated between Doom tics */
static int m_idx, m_pkt[3];
static int acc_dx, acc_dy, m_buttons, last_buttons;

static void mouse_byte(uint8_t b)
{
    int dx, dy;
    if (m_idx == 0 && !(b & 0x08)) return;      /* out of sync: wait for a header */
    m_pkt[m_idx++] = b;
    if (m_idx < 3) return;
    m_idx = 0;

    dx = m_pkt[1] - ((m_pkt[0] << 4) & 0x100);  /* 9-bit signed */
    dy = m_pkt[2] - ((m_pkt[0] << 3) & 0x100);
    if (m_pkt[0] & 0xC0) dx = dy = 0;           /* overflow: drop motion */
    acc_dx += dx;
    acc_dy += dy;                               /* PS/2 y is up-positive, like Doom */
    m_buttons = m_pkt[0] & 7;                   /* L=fire  R=strafe  M=forward */
}

static void poll_ps2(void)
{
    int budget = 256;

    while (budget--) {
        uint8_t st = inb(0x64), sc;

        if (!(st & 0x01)) {
            /* A mouse packet is 3 bytes and the controller only buffers one
             * at a time: if we are mid-packet, give the device a moment to
             * deliver the rest instead of picking it up a frame later. */
            int n;
            if (!m_idx) break;
            for (n = 0; n < 4000 && !(inb(0x64) & 0x01); n++) ;
            if (!(inb(0x64) & 0x01)) { m_idx = 0; break; }
            continue;
        }

        sc = inb(0x60);
        if (st & 0x20) {                        /* byte from the aux port */
            if (mouse_on) mouse_byte(sc);
            continue;
        }

        if (kb_skip) { kb_skip--; continue; }
        if (sc == 0xE1) {                       /* Pause/Break: E1 1D 45 E1 9D C5 */
            kb_skip = 5;
            kq_push(1, KEY_PAUSE);
            kq_push(0, KEY_PAUSE);
            continue;
        }
        if (sc == 0xE0) { kb_ext = 1; continue; }
        if (sc == 0xFA || sc == 0xFE || sc == 0xAA) { kb_ext = 0; continue; }

        {
            int down = !(sc & 0x80), k;
            sc &= 0x7F;
            if (kb_ext) { k = ext_to_doom(sc); kb_ext = 0; }
            else        { k = sc < sizeof(sc_to_doom) ? sc_to_doom[sc] : 0; }
            if (k) kq_push(down, k);
        }
    }
}

/* Same curve as vanilla/Chocolate Doom (mouse_acceleration 2.0, threshold 10) */
static int accelerate_mouse(int val)
{
    if (val < 0) return -accelerate_mouse(-val);
    if (val > mouse_threshold)
        return (int)((val - mouse_threshold) * mouse_acceleration + mouse_threshold);
    return val;
}

/* One ev_mouse per tic, exactly like the DOS/Linux ports: data1 = button
 * bits (0 left, 1 right, 2 middle), data2 = x, data3 = y (up = forward). */
static void post_mouse_event(void)
{
    event_t ev;

    if (!mouse_on) return;
    if (!acc_dx && !acc_dy && m_buttons == last_buttons) return;

    ev.type  = ev_mouse;
    ev.data1 = m_buttons;
    ev.data2 = accelerate_mouse(acc_dx);
    ev.data3 = accelerate_mouse(acc_dy);
    ev.data4 = 0;
    acc_dx = acc_dy = 0;
    last_buttons = m_buttons;
    D_PostEvent(&ev);
}

int DG_GetKey(int* pressed, unsigned char* key)
{
    poll_ps2();

    if (kq_head != kq_tail) {
        *pressed = kq[kq_head].pressed;
        *key     = kq[kq_head].key;
        kq_head  = (kq_head + 1) % KQ_SIZE;
        return 1;
    }

    /* the engine polls until we return 0: that is once per tic */
    post_mouse_event();
    return 0;
}

/* ================================================================== */
/* entry point / exit                                                  */
/* ================================================================== */
#ifndef TANJA_IWAD_NAME
#define TANJA_IWAD_NAME "freedoom1.wad"
#endif
#define DOOM_STACK_BYTES (256 * 1024)
static uint8_t doom_stack[DOOM_STACK_BYTES] __attribute__((aligned(16)));

static void* exit_jb[5];
static char* g_args;
static int   g_exit_code;
static char  args_copy[256];
static char* argv_store[24];

void tanja_exit(int code)
{
    g_exit_code = code;
    __builtin_longjmp(exit_jb, 1);
}

/* runs on doom_stack */
void tanja_doom_run(void)
{
    int argc = 0, i = 0;
    char* p;

    argv_store[argc++] = "doom";

    /* split the shell argument string on spaces */
    for (p = g_args; p && *p && i < (int)sizeof(args_copy) - 1; p++)
        args_copy[i++] = *p;
    args_copy[i] = 0;
    p = args_copy;
    while (*p && argc < 22) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv_store[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    /* always name the IWAD so the engine can identify it */
    {
        int has_iwad = 0;
        for (i = 1; i < argc; i++)
            if (argv_store[i][0] == '-' && argv_store[i][1] == 'i' &&
                argv_store[i][2] == 'w') has_iwad = 1;
        if (!has_iwad && argc < 22) {
            argv_store[argc++] = "-iwad";
            argv_store[argc++] = TANJA_IWAD_NAME;
        }
    }
    argv_store[argc] = 0;

    mouse_init();

    if (__builtin_setjmp(exit_jb) == 0) {
        doomgeneric_Create(argc, argv_store);
        for (;;)
            doomgeneric_Tick();
    }

    /* reached through exit() */
    mouse_shutdown();
    tanja_gfx_leave();
    while (inb(0x64) & 1) (void)inb(0x60);      /* swallow stray key-ups */
    if (g_exit_code)
        print("doom: exited with an error\n");
}

void main(char* args)
{
    g_args = args;
    kb_ext = kb_skip = kq_head = kq_tail = m_idx = 0;
    acc_dx = acc_dy = m_buttons = last_buttons = 0;
    __asm__ volatile (
        "movl %%esp, %%esi\n\t"
        "movl %0, %%esp\n\t"
        "call tanja_doom_run\n\t"
        "movl %%esi, %%esp\n\t"
        :
        : "r"((uint32_t)(doom_stack + DOOM_STACK_BYTES - 16))
        : "esi", "eax", "ecx", "edx", "cc", "memory");
}
