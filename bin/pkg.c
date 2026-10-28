#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* pkg install <name>   fetch <repo>/<name> over HTTP and install the binary
 *                      directly to /usr/bin/<name>.o (no compilation)
 * pkg remove  <name>   delete /usr/bin/<name>.o
 *
 * Default source: https://github.com/MSK-Kernel/pkg_<name>/raw/main/<name>
 * (https, certificate checked). Override with $PKG_REPO or /etc/pkg.repo:
 * then the file is <repo>/<name> (no extension), http or https.
 *
 * A marker file /var/pkg/<name> is written so pkg can tell packages it
 * installed from system commands. */

extern void* malloc(unsigned int n);
extern void  free(void* p);

#define PKG_MAX (1024u * 1024u)
#define GH_FMT_A "https://github.com/MSK-Kernel/pkg_"

static int valid_name(const char* n) {
    int i;
    if (!n[0] || n[0] == '.' || strlen(n) > 48) return 0;
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

static void mkpath(char* out, const char* a, const char* b, const char* c) {
    out[0] = 0;
    strcat(out, a); strcat(out, b); strcat(out, c);
}

static int get_repo(char* out, int cap) {
    const char* e = get_env("PKG_REPO");
    int n = 0;
    out[0] = 0;
    if (e && *e) {
        strncpy(out, e, cap - 1); out[cap - 1] = 0;
    } else if (fs_file_exists("/etc/pkg.repo")) {
        char buf[256]; uint32_t sz = sizeof(buf) - 1;
        if (fs_read_file("/etc/pkg.repo", buf, &sz) == 0) {
            buf[sz] = 0;
            while (buf[n] && buf[n] != '\n' && buf[n] != '\r' && n < cap - 1) { out[n] = buf[n]; n++; }
            out[n] = 0;
        }
    }
    if (!out[0]) return 0;
    n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = 0;
    return 1;
}

static void no_perm(void) { print("pkg: permission denied (are you root? try: sudo pkg ...)\n"); }

static void do_install(const char* name) {
    char repo[200], url[300], marker[96], bin[96];
    uint8_t* buf;
    net_info_t ni;
    uint32_t len = 0;
    int st;

    if (cmd_exists(name)) { printf("pkg: '%s' is a built-in command\n", name); return; }
    mkpath(bin, "/usr/bin/", name, ".o");
    mkpath(marker, "/var/pkg/", name, "");
    if (fs_file_exists(bin) && !fs_file_exists(marker)) {
        printf("pkg: /usr/bin/%s.o is a system command, refusing to overwrite it\n", name);
        return;
    }
    if (!net_get_info(&ni) || !ni.configured) {
        print("pkg: network not configured (try: ifconfig dhcp)\n");
        return;
    }

    if (get_repo(repo, sizeof(repo))) {          /* custom repo: <repo>/<name> */
        mkpath(url, repo, "/", name);
    } else {                                     /* default: github.com/MSK-Kernel/pkg_<name> */
        strcpy(repo, "github.com/MSK-Kernel");
        mkpath(url, GH_FMT_A, name, "/raw/main/");
        strcat(url, name);
    }
    printf("fetching %s\n", url);

    buf = (uint8_t*)malloc(PKG_MAX + 1);
    if (!buf) { print("pkg: out of memory\n"); return; }

    st = net_http_get(url, buf, PKG_MAX, &len);
    if (st < 0) {
        switch (st) {
        case NET_E_DNS:     print("pkg: cannot resolve the repo host (check DNS / the repo URL)\n"); break;
        case NET_E_CONNECT: print("pkg: cannot connect to the repo\n"); break;
        case NET_E_TLS:     printf("pkg: secure connection failed: %s\n", net_tls_error()); break;
        case NET_E_TOOBIG:  print("pkg: package is larger than 1 MiB\n"); break;
        case NET_E_URL:     print("pkg: bad repo URL (set PKG_REPO or /etc/pkg.repo)\n"); break;
        default:            print("pkg: download failed (timeout / connection dropped)\n"); break;
        }
        free(buf); return;
    }
    if (st == 404) { printf("pkg: package '%s' not found in %s\n", name, repo); free(buf); return; }
    if (st != 200) { printf("pkg: server answered HTTP %d\n", st); free(buf); return; }
    if (len == 0)  { print("pkg: server sent an empty file\n"); free(buf); return; }

    if (!fs_directory_exists("/var")) fs_create_directory("/var");
    if (!fs_directory_exists("/var/pkg")) fs_create_directory("/var/pkg");
    if (!fs_directory_exists("/usr")) fs_create_directory("/usr");
    if (!fs_directory_exists("/usr/bin")) fs_create_directory("/usr/bin");

    /* Write the binary straight to /usr/bin/<name>.o */
    if (fs_write_file(bin, (const char*)buf, len) != 0) {
        if (fs_errno == 13) no_perm(); else print("pkg: cannot write to /usr/bin\n");
        free(buf); return;
    }
    fs_chmod(bin, 0755);

    /* Marker so remove can tell pkg-installed packages from system commands.
       Content doesn't matter; existence is the flag. */
    if (fs_write_file(marker, "1", 1) != 0) {
        /* Non-fatal: the binary is already installed. Just warn. */
        print("pkg: warning: could not write install marker in /var/pkg\n");
    }

    free(buf);
    printf("got %u bytes, installed %s\n", len, name, name);
}

static void do_remove(const char* name) {
    char bin[96], marker[96];
    mkpath(bin, "/usr/bin/", name, ".o");
    mkpath(marker, "/var/pkg/", name, "");
    if (!fs_file_exists(bin)) { printf("pkg: '%s' is not installed\n", name); return; }
    if (!fs_file_exists(marker)) {
        printf("pkg: '%s' was not installed by pkg, refusing to delete a system command (use rm if you mean it)\n", name);
        return;
    }
    if (fs_delete_file(bin) != 0) {
        if (fs_errno == 13) no_perm(); else print("pkg: could not remove the binary\n");
        return;
    }
    fs_delete_file(marker); /* best effort cleanup of the marker */
    printf("removed %s\n", name);
}

void main(char* args) {
    char sub[16], name[64];
    int i = 0, j = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < 15) sub[i++] = *args++;
    sub[i] = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && j < 63) name[j++] = *args++;
    name[j] = 0;

    if ((strcmp(sub, "install") == 0 || strcmp(sub, "remove") == 0) && name[0]) {
        if (!valid_name(name)) { print("pkg: invalid package name (letters, digits, - _ . only)\n"); return; }
        if (sub[0] == 'i') do_install(name); else do_remove(name);
        return;
    }
    print("usage: pkg install <package_name>\n"
          "       pkg remove <package_name>\n");
}
