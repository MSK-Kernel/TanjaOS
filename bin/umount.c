#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* umount - unmount a filesystem
 *
 *   umount <mountpoint>       e.g. umount /mnt
 *   umount <device>           e.g. umount ata0  or  umount /dev/ata0
 *
 * Only root may unmount. The root filesystem cannot be unmounted, and a
 * mount is "busy" while the current directory is inside it. */

extern void fs_get_current_path(char* path);
extern int fs_cur_uid;

#define UM_MAX 16

static char m_mp[UM_MAX][64];
static char m_ft[UM_MAX][32];
static char m_dev[UM_MAX][32];
static int m_count;

static int slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* "mountpoint fstype device\n" lines from the VFS */
static void load_mounts(void) {
    static char buf[4096];
    uint32_t sz = sizeof(buf);
    int i = 0;
    m_count = 0;
    if (vfs_list_mounts(buf, &sz) != 0) return;
    while (i < (int)sz && m_count < UM_MAX) {
        int p = 0;
        while (i < (int)sz && buf[i] != ' ' && buf[i] != '\n' && p < 63)
            m_mp[m_count][p++] = buf[i++];
        m_mp[m_count][p] = 0;
        if (i < (int)sz && buf[i] == ' ') i++;
        p = 0;
        while (i < (int)sz && buf[i] != ' ' && buf[i] != '\n' && p < 31)
            m_ft[m_count][p++] = buf[i++];
        m_ft[m_count][p] = 0;
        if (i < (int)sz && buf[i] == ' ') i++;
        p = 0;
        while (i < (int)sz && buf[i] != '\n' && p < 31)
            m_dev[m_count][p++] = buf[i++];
        m_dev[m_count][p] = 0;
        if (i < (int)sz && buf[i] == '\n') i++;
        if (m_mp[m_count][0]) m_count++;
    }
}

/* is `cwd` the mountpoint itself or somewhere below it? */
static int path_inside(const char* cwd, const char* mp) {
    int n = slen(mp), i;
    if (n == 1 && mp[0] == '/') return 1;
    for (i = 0; i < n; i++)
        if (cwd[i] != mp[i]) return 0;
    return cwd[n] == 0 || cwd[n] == '/';
}

static void unmount_one(const char* mp) {
    char cwd[256];

    if (mp[0] == '/' && mp[1] == 0) {
        print("umount: /: cannot unmount the root filesystem\n");
        return;
    }
    cwd[0] = 0;
    fs_get_current_path(cwd);
    if (path_inside(cwd, mp)) {
        print("umount: ");
        print(mp);
        print(": target is busy (current directory is inside it)\n");
        return;
    }
    if (vfs_unmount(mp) != 0) {
        print("umount: ");
        print(mp);
        print(": unmount failed\n");
        return;
    }
    print("unmounted ");
    print(mp);
    print("\n");
}

void main(char* args) {
    char target[128];
    int n = 0, i, matched = 0;
    const char* t;

    if (!args) args = "";
    while (*args == ' ' || *args == '\t') args++;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && n < 127) {
        target[n] = args[n];
        n++;
    }
    target[n] = 0;

    if (!target[0] || streq(target, "-h") || streq(target, "--help")) {
        print("Usage: umount <mountpoint|device>\n");
        print("       umount /mnt\n");
        print("       umount /dev/ata0\n");
        return;
    }
    if (fs_cur_uid != 0) {
        print("umount: permission denied (must be root)\n");
        return;
    }

    /* tidy a trailing slash: "/mnt/" -> "/mnt" */
    while (n > 1 && target[n - 1] == '/') target[--n] = 0;

    load_mounts();

    /* 1. exact mountpoint */
    for (i = 0; i < m_count; i++) {
        if (streq(m_mp[i], target)) {
            unmount_one(m_mp[i]);
            return;
        }
    }

    /* 2. a device: "/dev/ata0" or "ata0" - unmount everything on it */
    t = target;
    if (t[0] == '/' && t[1] == 'd' && t[2] == 'e' && t[3] == 'v' && t[4] == '/')
        t += 5;
    for (i = 0; i < m_count; i++) {
        if (m_dev[i][0] && m_dev[i][0] != '-' && streq(m_dev[i], t)) {
            unmount_one(m_mp[i]);
            matched = 1;
        }
    }
    if (matched) return;

    print("umount: ");
    print(target);
    print(": not mounted\n");
}
