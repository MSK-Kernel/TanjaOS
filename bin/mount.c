#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* mount - mount a filesystem or list mounts
 *
 *   mount                       list current mounts
 *   mount [-t type] <dev> <dir> mount device (or "none") at directory
 */

extern int vfs_list_mounts(char* buf, uint32_t* size);
extern int vfs_mount(const char* mountpoint, const char* fstype, void* dev, const char* opts);
extern void* vfs_find_bdev(const char* name);
extern int fs_directory_exists(const char* path);
extern int fs_create_directory(const char* path);
extern int fs_delete_directory(const char* path);


static int slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

void main(char* args) {
    if (!args || !args[0]) {
        char buf[2048];
        uint32_t sz = sizeof(buf);
        print("MOUNTPOINT        FSTYPE   DEVICE\n");
        if (vfs_list_mounts(buf, &sz) != 0 || sz == 0) {
            print("(nothing mounted)\n");
            return;
        }
        int i = 0;
        while (i < (int)sz) {
            char mp[64] = {0}, ft[32] = {0}, dev[32] = {0};
            int p = 0;
            while (i < (int)sz && buf[i] != ' ' && buf[i] != '\n' && p < 63)
                mp[p++] = buf[i++];
            mp[p] = 0;
            if (i < (int)sz && buf[i] == ' ') i++;
            p = 0;
            while (i < (int)sz && buf[i] != ' ' && buf[i] != '\n' && p < 31)
                ft[p++] = buf[i++];
            ft[p] = 0;
            if (i < (int)sz && buf[i] == ' ') i++;
            p = 0;
            while (i < (int)sz && buf[i] != '\n' && p < 31)
                dev[p++] = buf[i++];
            dev[p] = 0;
            if (i < (int)sz && buf[i] == '\n') i++;

            print(mp);
            int pad = 18 - slen(mp);
            while (pad-- > 0) print(" ");
            print(ft);
            pad = 9 - slen(ft);
            while (pad-- > 0) print(" ");
            print(dev);
            print("\n");
        }
        return;
    }

    const char* type = "tmpfs";
    const char* devname = 0;
    const char* dir = 0;

    char tmp[256];
    int n = 0;
    while (args[n] && n < 255) { tmp[n] = args[n]; n++; }
    tmp[n] = 0;

    char* tok[8];
    int nt = 0;
    char* p = tmp;
    while (*p && nt < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[nt++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = 0; p++; }
    }

    int i = 0;
    int type_given = 0;
    if (nt >= 2 && streq(tok[0], "-t")) {
        type = tok[1];
        type_given = 1;
        i = 2;
    }
    if (i < nt) devname = tok[i++];
    if (i < nt) dir = tok[i++];
    /* a real device with no -t: treat it as a TanjaOS disk / ISO */
    if (!type_given && devname && !streq(devname, "none") && !streq(devname, "-"))
        type = "tmpfs";

    if (!dir) {
        print("Usage: mount [-t type] <device|none> <directory>\n");
        print("       mount\n");
        return;
    }

    int made_dir = 0;
    if (!fs_directory_exists(dir)) {
        made_dir = 1;
        if (fs_create_directory(dir) != 0) {
            print("mount: cannot create mount point\n");
            return;
        }
    }

    /* Devices live in /dev: accept "/dev/ata0" as well as plain "ata0". */
    void* dev = 0;
    if (devname && !streq(devname, "none") && !streq(devname, "-")) {
        int explicit_dev = 0;
        if (devname[0] == '/' && devname[1] == 'd' && devname[2] == 'e' &&
            devname[3] == 'v' && devname[4] == '/') {
            devname += 5;
            explicit_dev = 1;
        }
        dev = vfs_find_bdev(devname);
        if (!dev && explicit_dev) {
            print("mount: /dev/");
            print(devname);
            print(": no such device\n");
            if (made_dir) fs_delete_directory(dir);
            return;
        }
    }

    int rc = vfs_mount(dir, type, dev, 0);
    if (rc != 0) {
        if (made_dir) fs_delete_directory(dir);   /* do not leave the mountpoint behind */
        if (rc == -7 || rc == -3) print("mount: that directory is already a mount point\n");
        else if (rc == -2 && dev) print("mount: no TanjaOS disk or ISO9660 image found on that device\n");
        else print("mount: failed\n");
        return;
    }
    print("mounted ");
    print(type);
    print(" on ");
    print(dir);
    print("\n");
}
