#include "../include/fs.h"
#include "../include/utf8.h"
#include <stdint.h>

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85
#define CTRL_X 24
#define TAB_KEY 9
#define MAX_TEXT (MAX_FILE_SIZE + 1)

#define EDITOR_TOP_ROW 3
#define EDITOR_BOTTOM_ROW 23
#define EDITOR_ROWS (EDITOR_BOTTOM_ROW - EDITOR_TOP_ROW + 1)
#define EDITOR_COLS 80
#define TAB_WIDTH 4
#define VGA_HEIGHT 25
#define VGA_TOTAL_CELLS (EDITOR_COLS * VGA_HEIGHT)
#define VGA_COLOR (0x0F << 8)

extern void print(const char*);
extern void clear_screen(void);
extern void putc(char);
extern int get_key(void);
extern int cursor;
extern void sync_cursor(void);
extern uint16_t *VGA;

/*
 * Gap buffer
 * ----------
 * The old editor kept the file contiguous and inserted a character by
 * shifting every byte from the cursor to EOF.  For a 190 KiB Torah file,
 * typing at the beginning meant copying ~190 KiB for EVERY keypress, which
 * made the editor look completely frozen.
 *
 * Keep a gap at the cursor instead. Moving the cursor moves the gap once;
 * typing and backspace then only touch a couple bytes.  Logical positions
 * remain 0..len-1, while the gap is invisible to the rest of the editor.
 */
typedef struct {
    char data[MAX_TEXT];
    int gap_start;
    int gap_end;
    int len;
} EditorBuffer;

static EditorBuffer eb;

static int text_len(void) { return eb.len; }

static char text_at(int pos)
{
    if (pos < 0 || pos >= eb.len) return 0;
    if (pos < eb.gap_start) return eb.data[pos];
    return eb.data[pos + (eb.gap_end - eb.gap_start)];
}

static int physical_pos(int pos)
{
    if (pos < eb.gap_start) return pos;
    return pos + (eb.gap_end - eb.gap_start);
}

static void move_gap(int pos)
{
    int i;
    if (pos < 0) pos = 0;
    if (pos > eb.len) pos = eb.len;
    if (pos == eb.gap_start) return;

    if (pos < eb.gap_start) {
        int n = eb.gap_start - pos;
        for (i = n - 1; i >= 0; i--)
            eb.data[eb.gap_end - n + i] = eb.data[pos + i];
        eb.gap_start -= n;
        eb.gap_end -= n;
    } else {
        int n = pos - eb.gap_start;
        for (i = 0; i < n; i++)
            eb.data[eb.gap_start + i] = eb.data[eb.gap_end + i];
        eb.gap_start += n;
        eb.gap_end += n;
    }
}

static int gap_size(void) { return eb.gap_end - eb.gap_start; }

static int insert_byte(int pos, char ch)
{
    if (eb.len >= MAX_TEXT - 1 || gap_size() <= 0) return 0;
    move_gap(pos);
    eb.data[eb.gap_start++] = ch;
    eb.len++;
    return 1;
}

static void delete_previous(int pos)
{
    if (pos <= 0) return;
    move_gap(pos);
    if (eb.gap_start > 0) {
        eb.gap_start--;
        eb.len--;
    }
}

static void delete_range(int pos, int count)
{
    if (count <= 0 || pos < 0 || pos >= eb.len) return;
    if (pos + count > eb.len) count = eb.len - pos;
    move_gap(pos);
    eb.gap_end += count;
    eb.len -= count;
}

/* Copy the logical buffer to a contiguous destination for saving. */
static void flatten(char *out)
{
    int i;
    for (i = 0; i < eb.len; i++) out[i] = text_at(i);
    out[eb.len] = 0;
}

/* Decode a logical UTF-8 sequence without requiring a contiguous buffer. */
static int decode_at(int pos, uint32_t *cp)
{
    unsigned char b0;
    int need, i;
    uint32_t c;

    if (pos < 0 || pos >= eb.len) { *cp = UTF8_INVALID; return 1; }
    b0 = (unsigned char)text_at(pos);
    *cp = UTF8_INVALID;
    if (b0 < 0x80) { *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0) { c = b0 & 0x1F; need = 1; if (c < 2) return 1; }
    else if ((b0 & 0xF0) == 0xE0) { c = b0 & 0x0F; need = 2; }
    else if ((b0 & 0xF8) == 0xF0) { c = b0 & 0x07; need = 3; }
    else return 1;
    if (pos + need >= eb.len) return 1;
    for (i = 1; i <= need; i++) {
        unsigned char b = (unsigned char)text_at(pos + i);
        if ((b & 0xC0) != 0x80) return 1;
        c = (c << 6) | (b & 0x3F);
    }
    if ((need == 2 && c < 0x800) || (need == 3 && c < 0x10000) ||
        (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return 1;
    *cp = c;
    return need + 1;
}

static int line_start(int pos)
{
    while (pos > 0 && text_at(pos - 1) != '\n') pos--;
    return pos;
}

static int line_end(int pos)
{
    while (pos < eb.len && text_at(pos) != '\n') pos++;
    return pos;
}

static int visual_col(int pos)
{
    int col = 0, i = line_start(pos);
    while (i < pos && text_at(i) != '\n') {
        if (text_at(i) == '\t') { col += TAB_WIDTH - (col % TAB_WIDTH); i++; }
        else { uint32_t cp; int step = decode_at(i, &cp); if (step < 1) step = 1; col++; i += step; }
    }
    return col;
}

static int snap_to_tab_stop(int pos)
{
    int start = line_start(pos), col = visual_col(pos), i;
    int all_spaces = 1;
    for (i = start; i < pos; i++) if (text_at(i) != ' ') { all_spaces = 0; break; }
    if (all_spaces) {
        int snapped = (col / TAB_WIDTH) * TAB_WIDTH;
        int p = start, c = 0;
        while (p < pos && c < snapped) { p++; c++; }
        return p;
    }
    return pos;
}

static int pos_at_visual_col(int start, int target)
{
    int pos = start, col = 0;
    while (pos < eb.len && text_at(pos) != '\n') {
        int width, step = 1;
        if (text_at(pos) == '\t') width = TAB_WIDTH - (col % TAB_WIDTH);
        else { uint32_t cp; step = decode_at(pos, &cp); if (step < 1) step = 1; width = 1; }
        if (target < col + width) return snap_to_tab_stop(pos);
        col += width; pos += step;
    }
    return snap_to_tab_stop(pos);
}

static int line_number_at(int pos)
{
    int line = 0, i;
    for (i = 0; i < pos; i++) if (text_at(i) == '\n') line++;
    return line;
}

static int line_start_number(int wanted)
{
    int line = 0, i = 0;
    while (line < wanted && i < eb.len) { if (text_at(i) == '\n') line++; i++; }
    return i;
}

static int step_forward(int pos)
{
    if (pos >= eb.len) return pos;
    if (text_at(pos) == '\n') return pos + 1;
    { uint32_t cp; int step = decode_at(pos, &cp); if (step < 1) step = 1; return pos + step; }
}

static int step_backward(int pos)
{
    int i;
    if (pos <= 0) return pos;
    i = pos - 1;
    while (i > 0 && ((unsigned char)text_at(i) & 0xC0) == 0x80) i--;
    return i;
}

static int visual_rows_of_line(int ls)
{
    int width = visual_col(line_end(ls));
    if (width == 0) return 1;
    return (width - 1) / EDITOR_COLS + 1;
}

static int move_vertical(int pos, int dir)
{
    int c = visual_col(pos), row = c / EDITOR_COLS, scol = c % EDITOR_COLS;
    int ls = line_start(pos), width = visual_col(line_end(pos));
    if (dir < 0) {
        if (row > 0) return pos_at_visual_col(ls, (row - 1) * EDITOR_COLS + scol);
        { int line = line_number_at(pos); if (line == 0) return pos;
          int prev = line_start_number(line - 1); int rows = visual_rows_of_line(prev);
          return pos_at_visual_col(prev, (rows - 1) * EDITOR_COLS + scol); }
    }
    if ((row + 1) * EDITOR_COLS < width)
        return pos_at_visual_col(ls, (row + 1) * EDITOR_COLS + scol);
    { int e = line_end(pos); if (e >= eb.len) return pos; return pos_at_visual_col(e + 1, scol); }
}

static void draw_editor(int pos, int scroll_line)
{
    clear_screen();
    print("TanjaOS Editor\n");
    print("Ctrl+X = Save & Exit\n");
    print("--------------------\n");

    int line = scroll_line;
    int start = line_start_number(line);
    int screen_row = EDITOR_TOP_ROW;

    while (screen_row <= EDITOR_BOTTOM_ROW && start <= eb.len) {
        int j = start;
        do {
            int col = 0;
            while (j < eb.len && text_at(j) != '\n' && col < EDITOR_COLS) {
                if (text_at(j) == '\t') {
                    int width = TAB_WIDTH - (col % TAB_WIDTH), k;
                    for (k = 0; k < width && col < EDITOR_COLS; k++) VGA[screen_row * EDITOR_COLS + col++] = VGA_COLOR | ' ';
                    j++;
                } else {
                    uint32_t cp; int step = decode_at(j, &cp); char cell;
                    if (step < 1) step = 1;
                    if (cp == UTF8_INVALID) cell = text_at(j);
                    else if (cp < 0x80) cell = (char)cp;
                    else { int m = utf8_map_ascii(cp); cell = m ? (char)m : '?'; }
                    VGA[screen_row * EDITOR_COLS + col++] = VGA_COLOR | (uint8_t)cell;
                    j += step;
                }
            }
            while (col < EDITOR_COLS) VGA[screen_row * EDITOR_COLS + col++] = VGA_COLOR | ' ';
            screen_row++;
        } while (j < eb.len && text_at(j) != '\n' && screen_row <= EDITOR_BOTTOM_ROW);
        if (j >= eb.len) break;
        start = j + 1; line++;
    }

    int current_line = line_number_at(pos);
    if (current_line >= scroll_line) {
        int rows_before = 0, line_i = scroll_line, i = line_start_number(line_i);
        while (line_i < current_line) {
            rows_before += visual_rows_of_line(i);
            i = line_end(i); if (i < eb.len && text_at(i) == '\n') i++; line_i++;
        }
        int c = visual_col(pos), seg = c / EDITOR_COLS, col_offset = c % EDITOR_COLS;
        int final_row = EDITOR_TOP_ROW + rows_before + seg;
        if (final_row > EDITOR_BOTTOM_ROW) final_row = EDITOR_BOTTOM_ROW;
        cursor = final_row * EDITOR_COLS + col_offset;
    }
    sync_cursor();
}

static void ensure_visible(int pos, int *scroll_line)
{
    int current_line = line_number_at(pos);
    if (current_line < *scroll_line) *scroll_line = current_line;
    if (*scroll_line < 0) *scroll_line = 0;
    for (;;) {
        int rows = 0, line = *scroll_line, i = line_start_number(line);
        while (line < current_line) { rows += visual_rows_of_line(i); i = line_end(i); if (i < eb.len && text_at(i) == '\n') i++; line++; }
        rows += visual_col(pos) / EDITOR_COLS;
        if (rows < EDITOR_ROWS || *scroll_line >= current_line) break;
        (*scroll_line)++;
    }
}

void cmd_editor(char *args)
{
    if (!args || !args[0]) { print("Usage: editor <file>\n"); return; }

    static char savebuf[MAX_TEXT];
    eb.len = 0;
    eb.gap_start = 0;
    eb.gap_end = MAX_TEXT - 1;
    eb.data[eb.gap_end] = 0;

    if (fs_file_exists(args)) {
        uint32_t size = fs_get_file_size(args);
        if (size > MAX_TEXT - 1) { print("editor: file is too large (maximum 262143 bytes)\n"); return; }
        if (fs_read_file_prefix(args, savebuf, MAX_TEXT, &size) != 0) {
            print("editor: cannot read '"); print(args); print("'\n"); return;
        }
        /* Put the existing file before the gap. */
        for (uint32_t i = 0; i < size; i++) eb.data[i] = savebuf[i];
        eb.len = (int)size;
        eb.gap_start = (int)size;
    } else if (fs_create_file(args) != 0) {
        print("editor: cannot open '"); print(args); print("': No such directory\n");
        return;
    }

    int pos = 0, scroll_line = 0;
    uint16_t saved_screen[VGA_TOTAL_CELLS];
    for (int i = 0; i < VGA_TOTAL_CELLS; i++) saved_screen[i] = VGA[i];
    int saved_cursor = cursor;

    /* Put the gap at the initial cursor position. This costs one copy only. */
    move_gap(pos);

    while (1) {
        ensure_visible(pos, &scroll_line);
        draw_editor(pos, scroll_line);
        int key = get_key();

        if (key == CTRL_X) {
            flatten(savebuf);
            if (fs_write_file(args, savebuf, (uint32_t)eb.len) != 0) {
                print("editor: cannot save '"); print(args); print("': file is too large or filesystem is full\n");
                continue;
            }
            for (int i = 0; i < VGA_TOTAL_CELLS; i++) VGA[i] = saved_screen[i];
            cursor = saved_cursor; sync_cursor(); return;
        }
        if (key == KEY_LEFT) { pos = step_backward(pos); move_gap(pos); continue; }
        if (key == KEY_RIGHT) { pos = step_forward(pos); move_gap(pos); continue; }
        if (key == KEY_UP || key == KEY_DOWN) { pos = move_vertical(pos, key == KEY_UP ? -1 : 1); move_gap(pos); continue; }
        if (key == KEY_ENTER || key == '\n') { if (insert_byte(pos, '\n')) pos++; continue; }
        if (key == KEY_BACKSPACE || key == 8) { int old = pos; pos = step_backward(pos); delete_range(pos, old - pos); continue; }
        if (key == TAB_KEY) { if (insert_byte(pos, '\t')) pos++; continue; }
        if (key >= 32 && key <= 126) { if (insert_byte(pos, (char)key)) pos++; }
    }
}
