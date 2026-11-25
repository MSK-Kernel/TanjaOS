#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* pkg install <name> fetches a package executable when present and installs
 * repository payload trees (lib/, usr/, etc/, boot/) into the matching system
 * paths. A boot/ folder in the repo lands in /boot.
 * Executables are installed directly to /usr/bin/<name>.o (no compilation).
 * If the package repo also contains a lib/ directory, the contents are
 * installed to /lib so that ELF32 binaries can resolve their dependencies
 * from there (see kernel/elf.c library support).
 * pkg remove <name> delete /usr/bin/<name>.o
 *
 * Default source: https://github.com/MSK-Kernel/pkg_<name>/raw/main/<name>
 * (https, certificate checked). Override with $PKG_REPO or /etc/pkg.repo:
 * then the file is <repo>/<name> (no extension), http or https.
 *
 * A marker file /var/pkg/<name> is written so pkg can tell packages it
 * installed from system commands. */

extern void* malloc(unsigned int n);
extern void  free(void* p);
extern int net_http_save(const char* url, const char* path, unsigned int* len);
extern int net_http_get(const char* url, uint8_t* buf, uint32_t cap, uint32_t* len);

extern int fs_cur_uid;

#define PKG_MAX (36u * 1024u * 1024u)
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

static void no_perm(void) { print("pkg: Permission denied; are you root?\n"); }


/* GitHub tree API support for package payload directories. The recursive
 * Git tree response includes metadata for every entry, so it can be much
 * larger than the actual files being installed. Keep enough room for normal
 * package repositories while still bounding memory use. */
#define TREE_CAP (512u * 1024u)
static uint8_t tree_json[TREE_CAP];

static int json_string(const char* p, char* out, int cap) {
    int n = 0;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'u') return -1; /* paths in package repos need no unicode */
            if (c == '/' || c == '\\' || c == '"') { }
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else return -1;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    if (*p != '"') return -1;
    out[n] = 0;
    return n;
}

static int safe_repo_path(const char* p) {
    int i;
    if (!(strncmp(p, "lib/", 4) == 0 || strncmp(p, "usr/", 4) == 0 ||
          strncmp(p, "etc/", 4) == 0 || strncmp(p, "boot/", 5) == 0)) return 0;
    for (i = 0; p[i]; i++) {
        char c = p[i];
        if (!(isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '+')) return 0;
        if (c == '.' && p[i+1] == '.' && (i == 0 || p[i-1] == '/') &&
            (p[i+2] == 0 || p[i+2] == '/')) return 0;
    }
    return 1;
}

static int make_parent_dirs(const char* path) {
    char p[256];
    int i, n = strlen(path);
    if (n >= (int)sizeof(p)) return -1;
    strcpy(p, path);
    for (i = 1; i < n; i++) {
        if (p[i] == '/') {
            p[i] = 0;
            if (p[0] && !fs_directory_exists(p) && fs_create_directory(p) != 0) return -1;
            p[i] = '/';
        }
    }
    return 0;
}

static int install_repo_trees(const char* name) {
    char api[320], raw[384], path[160], dest[256], type[24], manifest[96];
    uint32_t len = 0;
    int st;
    mkpath(api, "https://api.github.com/repos/MSK-Kernel/pkg_", name, "/git/trees/main?recursive=1");
    st = net_http_get(api, tree_json, TREE_CAP, &len);
    if (st != 200 || len == 0 || len >= TREE_CAP) {
        if (st != 404)
            printf("pkg: warning: could not read package directory manifest (HTTP %d)\n", st);
        return 0;
    }
    tree_json[len] = 0;

    /* Each tree entry is a JSON object. Extract its path and type fields. */
    const char* p = (const char*)tree_json;
    const char* end = p + len;
    int installed = 0;
    mkpath(manifest, "/var/pkg/", name, ".files");
    while (p < end) {
        const char* key = strstr(p, "\"path\"");
        const char* obj_end;
        const char* tp;
        if (!key || key >= end) break;
        p = key + 6;
        while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
        if (p >= end || json_string(p, path, sizeof(path)) < 0) { p = key + 6; while (p < end && *p != '}') p++; if (p < end) p++; continue; }
        obj_end = strchr(p, '}');
        if (!obj_end || obj_end > end) break;
        tp = strstr(p, "\"type\"");
        if (!tp || tp > obj_end) { p = obj_end + 1; continue; }
        tp += 6;
        while (tp < obj_end && (*tp == ' ' || *tp == ':' || *tp == '\t')) tp++;
        if (tp >= obj_end || json_string(tp, type, sizeof(type)) < 0) { p = obj_end + 1; continue; }
        p = obj_end + 1;

        if (strcmp(type, "blob") != 0 || !safe_repo_path(path)) continue;
        if (strlen(path) + 6 >= sizeof(dest)) continue;
        strcpy(dest, "/"); strcat(dest, path);
        if (make_parent_dirs(dest) != 0) {
            printf("pkg: warning: cannot create directory for %s\n", dest);
            continue;
        }
        mkpath(raw, "https://raw.githubusercontent.com/MSK-Kernel/pkg_", name, "/main/");
        strcat(raw, path);
        uint32_t file_len = 0;
        st = net_http_save(raw, dest, &file_len);
        if (st == 200) {
            if (strncmp(path, "usr/bin/", 8) == 0) fs_chmod(dest, 0755);
            installed++;
            /* Record the exact destination immediately after a successful
               download so pkg remove can clean partial or full installs. */
            if (fs_append_file(manifest, dest, strlen(dest)) != 0 ||
                fs_append_file(manifest, "\n", 1) != 0)
                print("pkg: warning: could not update package file manifest\n");
            printf("installed /%s (%u bytes)\n", path, file_len);
        } else {
            printf("pkg: warning: failed to fetch %s (HTTP %d)\n", path, st);
        }
    }
    return installed;
}

/* Remove only files recorded by this package. Prune empty directories
   without ever removing the system roots /lib, /usr, /etc, or /boot. */
static void prune_payload_dirs(const char* file) {
    char dir[256];
    int i, n = strlen(file);
    if (n >= (int)sizeof(dir)) return;
    strcpy(dir, file);
    for (i = n - 1; i >= 0; i--) if (dir[i] == '/') { dir[i] = 0; break; }
    while (i > 0 && dir[0]) {
        if (strcmp(dir, "/lib") == 0 || strcmp(dir, "/usr") == 0 ||
            strcmp(dir, "/etc") == 0 || strcmp(dir, "/boot") == 0 ||
            strcmp(dir, "/" ) == 0) break;
        /* fs_delete_directory() already refuses non-empty directories.
           Use its return value instead of requiring a separate ELF export. */
        if (!fs_directory_exists(dir)) break;
        if (fs_delete_directory(dir) != 0) break;
        n = strlen(dir);
        for (i = n - 1; i >= 0; i--) if (dir[i] == '/') { dir[i] = 0; break; }
    }
}

static int payload_owned_by_other(const char* path, const char* current_manifest) {
    static char listing[16384];
    static char other_data[32768];
    uint32_t list_size = sizeof(listing) - 1;
    uint32_t pos = 0;
    if (fs_list_directory("/var/pkg", listing, &list_size) != 0) return 0;
    listing[list_size] = 0;
    while (pos < list_size) {
        char entry[96], other_manifest[128];
        int n = 0;
        while (pos < list_size && listing[pos] != '\n' && n < (int)sizeof(entry) - 1)
            entry[n++] = listing[pos++];
        while (pos < list_size && listing[pos] != '\n') pos++;
        if (pos < list_size) pos++;
        entry[n] = 0;
        if (!n || entry[n - 1] == '/') continue;
        if (n < 6 || strcmp(entry + n - 6, ".files") != 0) continue;
        mkpath(other_manifest, "/var/pkg/", entry, "");
        if (strcmp(other_manifest, current_manifest) == 0) continue;
        if (fs_get_file_size(other_manifest) >= sizeof(other_data)) continue;
        uint32_t sz = sizeof(other_data) - 1;
        if (fs_read_file(other_manifest, other_data, &sz) != 0) continue;
        other_data[sz] = 0;
        uint32_t at = 0;
        while (at < sz) {
            char candidate[256];
            int m = 0;
            while (at < sz && other_data[at] != '\n' && m < (int)sizeof(candidate) - 1)
                candidate[m++] = other_data[at++];
            while (at < sz && other_data[at] != '\n') at++;
            if (at < sz) at++;
            candidate[m] = 0;
            if (m && strcmp(candidate, path) == 0) return 1;
        }
    }
    return 0;
}

static int remove_payload_manifest(const char* manifest) {
    static char contents[32768];
    uint32_t size = sizeof(contents) - 1;
    uint32_t pos = 0;
    int removed = 0;
    uint32_t actual = fs_get_file_size(manifest);
    if (actual >= sizeof(contents)) {
        print("pkg: manifest too large; refusing partial payload removal\n");
        return -1;
    }
    if (fs_read_file(manifest, contents, &size) != 0) return 0;
    contents[size] = 0;
    while (pos < size) {
        char path[256];
        int n = 0;
        while (pos < size && contents[pos] != '\n' && n < (int)sizeof(path) - 1)
            path[n++] = contents[pos++];
        while (pos < size && contents[pos] != '\n') pos++;
        if (pos < size && contents[pos] == '\n') pos++;
        path[n] = 0;
        if (!n) continue;
        /* Manifest entries must remain inside the package roots. */
        if (!(strncmp(path, "/lib/", 5) == 0 || strncmp(path, "/usr/", 5) == 0 ||
              strncmp(path, "/etc/", 5) == 0 || strncmp(path, "/boot/", 6) == 0)) continue;
        if (payload_owned_by_other(path, manifest)) continue;
        if (fs_file_exists(path)) {
            /* revert, not delete: a payload file that replaced a file on the
               boot ISO must bring the original back, not hide it */
            if (fs_revert_file(path) == 0) {
                removed++;
                prune_payload_dirs(path);
            } else {
                printf("pkg: warning: could not remove %s\n", path);
            }
        }
    }
    return removed;
}

static void do_install(const char* name) {
    char repo[200], url[300], marker[96], bin[96], manifest[96];
    net_info_t ni;
    uint32_t len = 0;
    int st;
    int payload_count = 0;

    if (cmd_exists(name)) { printf("pkg: '%s' is a built-in command\n", name); return; }
    mkpath(bin, "/usr/bin/", name, ".o");
    mkpath(marker, "/var/pkg/", name, "");
    mkpath(manifest, "/var/pkg/", name, ".files");
    if (fs_file_exists(bin) && !fs_file_exists(marker)) {
        printf("pkg: /usr/bin/%s.o is a system command, refusing to overwrite it\n", name);
        return;
    }
    if (!net_get_info(&ni) || !ni.configured) {
        print("pkg: network not configured, try: ifconfig dhcp\n");
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

    if (!fs_directory_exists("/var")) fs_create_directory("/var");
    if (!fs_directory_exists("/var/pkg")) fs_create_directory("/var/pkg");
    if (!fs_directory_exists("/usr")) fs_create_directory("/usr");
    if (!fs_directory_exists("/usr/bin")) fs_create_directory("/usr/bin");

    /* Start a fresh manifest for this installation. */
    if (fs_file_exists(manifest)) fs_delete_file(manifest);
    if (fs_write_file(manifest, "", 0) != 0)
        print("pkg: warning: could not create package file manifest\n");

    /* GitHub packages may consist entirely of directory payloads (lib/,
       usr/, etc.) and therefore have no executable at the repository root.
       Install those files before attempting the optional root executable. */
    if (!get_repo(repo, sizeof(repo)))
        payload_count = install_repo_trees(name);

    /* Stream the optional package executable straight to disk. A small
       receive buffer is enough, so large packages do not need a huge
       contiguous malloc. */
    if (fs_file_exists(bin)) fs_delete_file(bin);
    st = net_http_save(url, bin, &len);
    if (st < 0) {
        if (fs_file_exists(bin)) fs_delete_file(bin);
        switch (st) {
        case NET_E_DNS:     print("pkg: cannot resolve the repo host (check DNS / repo URL)\n"); break;
        case NET_E_CONNECT: print("pkg: cannot connect to the repo\n"); break;
        case NET_E_TLS:     printf("pkg: secure connection failed: %s\n", net_tls_error()); break;
        case NET_E_TOOBIG:  print("\npkg: package is too large\n"); break;
        case NET_E_URL:     print("pkg: bad repo URL (set PKG_REPO or /etc/pkg.repo)\n"); break;
        default:            print("\npkg: not enough storage or connection dropped\n"); break;
        }
        return;
    }
    if (st == 404) {
        if (fs_file_exists(bin)) fs_delete_file(bin);
        if (payload_count > 0) {
            if (fs_write_file(marker, "1", 1) != 0)
                print("pkg: warning: could not write install marker in /var/pkg\n");
            return;
        }
        if (fs_file_exists(manifest)) fs_delete_file(manifest);
        printf("pkg: package '%s' not found in %s\n", name, repo);
        return;
    }
    if (st != 200) { printf("pkg: server answered HTTP %d\n", st); if (fs_file_exists(bin)) fs_delete_file(bin); return; }
    if (len == 0)  { print("pkg: server sent an empty file\n"); if (fs_file_exists(bin)) fs_delete_file(bin); return; }
    if (fs_chmod(bin, 0755) != 0) {
        fs_delete_file(bin);
        print("pkg: installed file but could not set executable permissions; removed it\n");
        return;
    }

    /* Marker so remove can tell pkg-installed packages from system commands.
       Content doesn't matter; existence is the flag. */
    if (fs_write_file(marker, "1", 1) != 0) {
        /* Non-fatal: the binary is already installed. Just warn. */
        print("pkg: warning: could not write install marker in /var/pkg\n");
    }

    printf("got %u bytes, installed %s\n", len, name);
}

static void do_remove(const char* name) {
    char bin[96], marker[96], manifest[96];
    int payload_removed = 0;
    mkpath(bin, "/usr/bin/", name, ".o");
    mkpath(marker, "/var/pkg/", name, "");
    mkpath(manifest, "/var/pkg/", name, ".files");
    if (!fs_file_exists(marker)) {
        if (!fs_file_exists(bin)) { printf("pkg: '%s' is not installed\n", name); return; }
        printf("pkg: '%s' was not installed by pkg\n", name);
        return;
    }
    if (fs_file_exists(bin) && fs_delete_file(bin) != 0) {
        if (fs_errno == 13) no_perm(); else print("pkg: could not remove the binary\n");
        return;
    }
    if (fs_file_exists(manifest)) {
        payload_removed = remove_payload_manifest(manifest);
        if (payload_removed < 0) return;
        fs_delete_file(manifest);
    }
    fs_delete_file(marker);
    printf("removed %s\n", name);
}

#define MAX_PKGS 16

/* pkg install a b c: one at a time, in the order typed. The names are all
 * checked first, so a typo stops the command before anything is installed. */
void main(char* args) {
    char sub[16];
    char names[MAX_PKGS][64];
    int count = 0, i = 0, k, dup;

    /* Package installation/removal changes system-wide /usr/bin and /var/pkg. */
    if (fs_cur_uid != 0) {
        print("pkg: Permission denied, are you root?\n");
        return;
    }
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < 15) sub[i++] = *args++;
    sub[i] = 0;

    if (strcmp(sub, "install") == 0 || strcmp(sub, "remove") == 0) {
        for (;;) {
            int j = 0;
            char tok[64];
            while (*args == ' ') args++;
            if (!*args) break;
            while (*args && *args != ' ' && j < 63) tok[j++] = *args++;
            tok[j] = 0;
            while (*args && *args != ' ') args++;          /* over-long name: skip the rest */
            if (!valid_name(tok)) {
                printf("pkg: invalid package name '%s' (letters, digits, - _ . only)\n", tok);
                return;
            }
            for (dup = 0, k = 0; k < count; k++)
                if (strcmp(names[k], tok) == 0) dup = 1;
            if (dup) continue;                             /* typed twice: do it once */
            if (count == MAX_PKGS) {
                printf("pkg: too many packages at once (max %d)\n", MAX_PKGS);
                return;
            }
            strcpy(names[count++], tok);
        }
        if (count) {
            for (k = 0; k < count; k++) {
                if (count > 1)
                    printf("[%d/%d] %s %s\n", k + 1, count, sub[0] == 'i' ? "installing" : "removing", names[k]);
                if (sub[0] == 'i') do_install(names[k]); else do_remove(names[k]);
            }
            return;
        }
    }
    print("usage: pkg install <package_name> [package_name ...]\n"
          "       pkg remove <package_name> [package_name ...]\n");
}
