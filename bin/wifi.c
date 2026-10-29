#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* wifi            interactive menu
 * wifi list       scan and print the networks in range
 * wifi status     wired + Wi-Fi state
 * wifi connect <ssid> [password]     (quote SSIDs that contain spaces)
 * wifi disconnect | forget <ssid> | saved | adapter [N] | help
 *
 * Talks to the Wi-Fi framework in kernel/wifi.c; works with any number of
 * adapters and every security type a driver reports. */

#define MAXN    32
#define VISIBLE 12

static void show_hardware(void);

static wifi_net_t nets[MAXN];
static int nn, sel, top;
static char msg[100];
static int msg_good;

static const char* sec_name(int s) {
    switch (s) {
    case WIFI_SEC_OPEN: return "open";
    case WIFI_SEC_WEP:  return "WEP";
    case WIFI_SEC_WPA:  return "WPA2";
    case WIFI_SEC_WPA3: return "WPA3";
    case WIFI_SEC_EAP:  return "802.1X";
    }
    return "?";
}
static const char* band(int ch) { return ch >= 32 ? "5G" : "2.4G"; }

static void bars(int sig, char* out) {
    int n = sig >= 75 ? 4 : sig >= 50 ? 3 : sig >= 25 ? 2 : 1, i;
    out[0] = '[';
    for (i = 0; i < 4; i++) out[1 + i] = i < n ? '#' : '.';
    out[5] = ']'; out[6] = 0;
}

static void set_msg(const char* m) { strncpy(msg, m, sizeof(msg) - 1); msg[sizeof(msg) - 1] = 0; msg_good = 0; }
static void set_ok(const char* m) { set_msg(m); msg_good = 1; }

static void ip_msg(char* m, uint32_t ip) {
    int sh;
    for (sh = 24; sh >= 0; sh -= 8) {
        itoa((int)((ip >> sh) & 255), m + strlen(m), 10);
        if (sh) strcat(m, ".");
    }
}

/* result text after wifi_connect */
static void report(int rc, int used_saved) {
    net_info_t ni;
    char m[100];
    if (rc != 0) {
        strncpy(m, wifi_strerror(rc), sizeof(m) - 1); m[sizeof(m) - 1] = 0;
        if (rc == WIFI_E_AUTH && used_saved) strcat(m, " - saved password failed, f forgets it");
        set_msg(m);
        return;
    }
    if (net_get_info(&ni) && ni.configured) { strcpy(m, "Connected. IP "); ip_msg(m, ni.ip); set_ok(m); }
    else set_msg("Associated, but no IP address (no data path / DHCP failed).");
}

static int parse_int(const char* s) { return isdigit(*s) ? atoi(s) : -1; }

/* ---------------------------------------------------------- arguments */

/* next word; "double quoted" words may contain spaces */
static int next_arg(char** pp, char* out, int cap) {
    char* p = *pp;
    int n = 0;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; out[0] = 0; return 0; }
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && n < cap - 1) out[n++] = *p++;
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

/* ------------------------------------------------------------- input */

/* line input; mask=1 echoes '*'. 0 = Enter, -1 = Esc */
static int ask(const char* prompt, char* out, int cap, int mask) {
    int n = 0, k;
    print(prompt);
    for (;;) {
        k = get_key();
        if (k == '\n' || k == '\r' || k == KEY_ENTER) { out[n] = 0; print("\n"); return 0; }
        if (k == 27) return -1;
        if ((k == 8 || k == KEY_BACKSPACE) && n > 0) {
            n--; console_putc('\b'); console_putc(' '); console_putc('\b');
        } else if (k >= 32 && k < 127 && n < cap - 1) {
            out[n++] = (char)k; console_putc(mask ? '*' : (char)k);
        }
    }
}

/* -------------------------------------------------------- non-interactive */

static void print_status(void) {
    net_info_t ni;
    char name[64], cur[40];
    int n = wifi_adapter_count();
    if (net_get_info(&ni) && ni.present)
        printf("wired : %s, link %s%s", ni.driver, ni.link ? "up" : "down",
               ni.configured ? ", " : "\n");
    else print("wired : no adapter\n");
    if (ni.present && ni.configured)
        printf("%d.%d.%d.%d\n", (int)(ni.ip >> 24), (int)((ni.ip >> 16) & 255),
               (int)((ni.ip >> 8) & 255), (int)(ni.ip & 255));
    if (!n) { show_hardware(); return; }
    wifi_adapter_name(name, sizeof(name));
    if (wifi_status(cur, sizeof(cur))) printf("wifi  : %s - connected to %s\n", name, cur);
    else printf("wifi  : %s - not connected\n", name);
    if (n > 1) printf("        (%d adapters, using #%d; 'wifi adapter N' switches)\n", n, wifi_selected());
}

static void show_hardware(void) {
    static char hw[4][56];
    int i, n = wifi_hw_list(hw, 4);
    if (!n) { print("wifi  : no Wi-Fi card found on the PCI bus\n"); return; }
    print("wifi  : Wi-Fi card detected but there is no driver for it yet:\n");
    for (i = 0; i < n; i++) printf("          %s\n", hw[i]);
}

static int need_adapter(void) {
    if (wifi_adapter_count()) return 1;
    show_hardware();
    print("Wired Ethernet configures itself at boot (see: ifconfig).\n");
    return 0;
}

static void cmd_list(void) {
    char b[8];
    int i, n;
    if (!need_adapter()) return;
    n = wifi_scan(nets, MAXN);
    if (n < 0) { printf("wifi: %s\n", wifi_strerror(n)); return; }
    if (!n) { print("no networks found\n"); return; }
    for (i = 0; i < n; i++) {
        char pw[2];
        bars(nets[i].signal, b);
        printf("%-32s %s %-6s %-4s ch%-3d%s\n", nets[i].ssid, b, sec_name(nets[i].security),
               band(nets[i].channel), nets[i].channel, wifi_saved_pass(nets[i].ssid, pw, 2) ? " saved" : "");
    }
}

static void cmd_saved(void) {
    static char l[16][33];
    int i, n = wifi_saved_list(l, 16);
    if (!n) { print("no saved networks\n"); return; }
    for (i = 0; i < n; i++) printf("%s\n", l[i]);
}

static void cmd_connect(char* rest) {
    char ssid[40], pass[80];
    int rc, used_saved = 0, have_pass, sec = -1, i, n;
    if (!need_adapter()) return;
    if (!next_arg(&rest, ssid, sizeof(ssid))) { print("usage: wifi connect <ssid> [password]\n"); return; }
    have_pass = next_arg(&rest, pass, sizeof(pass));
    n = wifi_scan(nets, MAXN);
    for (i = 0; i < n; i++) if (strcmp(nets[i].ssid, ssid) == 0) { sec = nets[i].security; break; }
    if (sec == WIFI_SEC_EAP) { print("wifi: 802.1X enterprise networks are not supported\n"); return; }
    if (!have_pass) {
        pass[0] = 0;
        if (wifi_saved_pass(ssid, pass, sizeof(pass))) used_saved = 1;
        else if (sec != WIFI_SEC_OPEN && sec != -1) { print("wifi: that network needs a password: wifi connect <ssid> <password>\n"); return; }
    }
    rc = wifi_connect(ssid, pass);
    if (rc == 0) {
        net_info_t ni;
        printf("connected to %s\n", ssid);
        if (net_get_info(&ni) && ni.configured) print("address obtained\n");
        else print("note: no IP address (adapter driver has no data path yet)\n");
    } else {
        printf("wifi: %s\n", wifi_strerror(rc));
        if (rc == WIFI_E_AUTH && used_saved) print("(the saved password was used; 'wifi forget' removes it)\n");
    }
}

static void usage(void) {
    print("usage: wifi                  interactive menu\n"
          "       wifi list             scan and list networks\n"
          "       wifi status\n"
          "       wifi connect <ssid> [password]   (quote names with spaces)\n"
          "       wifi disconnect | saved | forget <ssid>\n"
          "       wifi adapter [N]      list / choose the adapter\n");
}

static void cmd_adapter(char* rest) {
    char a[16], name[64];
    int i, n = wifi_adapter_count();
    if (!need_adapter()) return;
    if (next_arg(&rest, a, sizeof(a))) {
        int idx = parse_int(a);
        if (idx < 0 || wifi_select(idx) != 0) { printf("wifi: no adapter #%s\n", a); return; }
        printf("using adapter #%d\n", idx);
        return;
    }
    for (i = 0; i < n; i++) {
        wifi_adapter_info(i, name, sizeof(name));
        printf("%c #%d %s\n", i == wifi_selected() ? '*' : ' ', i, name);
    }
}

/* -------------------------------------------------------------- menu */

static void rescan(void) {
    print("Scanning...\n");
    nn = wifi_scan(nets, MAXN);
    if (nn < 0) { set_msg(wifi_strerror(nn)); nn = 0; }
    else set_msg(nn ? "" : "No networks found.");
    sel = 0; top = 0;
}

static void draw(void) {
    char name[64], cur[40], b[8], pw[2];
    int i, n = wifi_adapter_count();
    clear_screen();
    wifi_adapter_name(name, sizeof(name));
    print_color(" Wi-Fi\n", COLOR_LIGHT_GREEN);
    printf(" Adapter: %s", name);
    if (n > 1) printf("   (#%d of %d, a switches)", wifi_selected(), n);
    print("\n");
    if (wifi_status(cur, sizeof(cur))) printf(" Status : connected to %s\n", cur);
    else print(" Status : not connected\n");
    print("\n");
    if (nn > VISIBLE) printf(" Networks %d-%d of %d:\n", top + 1, top + VISIBLE > nn ? nn : top + VISIBLE, nn);
    else print(" Networks:\n");
    for (i = top; i < nn && i < top + VISIBLE; i++) {
        bars(nets[i].signal, b);
        printf(" %c %-24s %s %-6s %-4s%s\n", i == sel ? '>' : ' ', nets[i].ssid, b,
               sec_name(nets[i].security), band(nets[i].channel),
               wifi_saved_pass(nets[i].ssid, pw, 2) ? " *" : "");
    }
    print("\n");
    if (msg[0]) { print_color(" ", COLOR_WHITE); print_color(msg, msg_good ? COLOR_LIGHT_GREEN : COLOR_RED); print("\n\n"); }
    print(" Enter connect | h hidden | r rescan | d disconnect | f forget | q quit\n");
    if (n > 1) print(" a next adapter\n");
}

static void do_connect(void) {
    char pass[80];
    wifi_net_t* n;
    int rc, used_saved = 0;
    if (!nn) return;
    n = &nets[sel];
    pass[0] = 0;
    if (n->security == WIFI_SEC_EAP) { set_msg("802.1X enterprise networks are not supported."); return; }
    if (n->security != WIFI_SEC_OPEN) {
        if (wifi_saved_pass(n->ssid, pass, sizeof(pass))) used_saved = 1;
        else if (ask("\n Password (Esc cancels): ", pass, sizeof(pass), 1) != 0) { set_msg("Cancelled."); return; }
    }
    print(" Connecting...\n");
    rc = wifi_connect(n->ssid, pass);
    report(rc, used_saved);
}

static void do_hidden(void) {
    char ssid[40], pass[80];
    int rc, used_saved = 0;
    if (ask("\n Network name (Esc cancels): ", ssid, sizeof(ssid), 0) != 0 || !ssid[0]) { set_msg("Cancelled."); return; }
    pass[0] = 0;
    if (wifi_saved_pass(ssid, pass, sizeof(pass))) used_saved = 1;
    else if (ask(" Password (empty = open network): ", pass, sizeof(pass), 1) != 0) { set_msg("Cancelled."); return; }
    print(" Connecting...\n");
    rc = wifi_connect(ssid, pass);
    report(rc, used_saved);
}

static void menu(void) {
    int k;
    if (!need_adapter()) return;
    msg[0] = 0;
    rescan();
    for (;;) {
        draw();
        k = get_key();
        msg[0] = 0;
        if (k == 'q' || k == 'Q' || k == 27) break;
        else if (k == KEY_UP && sel > 0) { sel--; if (sel < top) top = sel; }
        else if (k == KEY_DOWN && sel < nn - 1) { sel++; if (sel >= top + VISIBLE) top = sel - VISIBLE + 1; }
        else if (k == 'r' || k == 'R') rescan();
        else if (k == 'h' || k == 'H') do_hidden();
        else if (k == 'a' || k == 'A') {
            int n = wifi_adapter_count();
            if (n > 1) { wifi_select((wifi_selected() + 1) % n); rescan(); }
        }
        else if (k == 'd' || k == 'D') { wifi_disconnect(); set_ok("Disconnected."); }
        else if ((k == 'f' || k == 'F') && nn) {
            if (wifi_forget(nets[sel].ssid) == 0) set_ok("Forgot the saved password.");
            else set_msg("Nothing saved for this network.");
        }
        else if (k == '\n' || k == '\r' || k == KEY_ENTER) do_connect();
    }
    clear_screen();
}

void main(char* args) {
    char sub[16], ssid[40];
    char* rest = args;

    if (!next_arg(&rest, sub, sizeof(sub))) { menu(); return; }
    if (strcmp(sub, "menu") == 0) menu();
    else if (strcmp(sub, "list") == 0 || strcmp(sub, "scan") == 0) cmd_list();
    else if (strcmp(sub, "status") == 0) print_status();
    else if (strcmp(sub, "saved") == 0) cmd_saved();
    else if (strcmp(sub, "connect") == 0) cmd_connect(rest);
    else if (strcmp(sub, "adapter") == 0) cmd_adapter(rest);
    else if (strcmp(sub, "disconnect") == 0) {
        if (!need_adapter()) return;
        print(wifi_disconnect() == 0 ? "disconnected\n" : "wifi: could not disconnect\n");
    }
    else if (strcmp(sub, "forget") == 0) {
        if (!next_arg(&rest, ssid, sizeof(ssid))) { print("usage: wifi forget <ssid>\n"); return; }
        print(wifi_forget(ssid) == 0 ? "forgotten\n" : "nothing saved for that network\n");
    }
    else usage();
}
