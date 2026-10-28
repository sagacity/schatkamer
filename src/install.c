// Installs the Schatkamer web player from data embedded in this payload.
// The launcher part is adapted from the TV4 Play launcher by John Törnblom (GPL-3.0-or-later).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "install.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PLAYER_ID "BREW18762"

#define INCASSET(name, file)                  \
    __asm__(".section .rodata\n"              \
            ".global " #name "\n"             \
            ".global " #name "_end\n"         \
            ".global " #name "_size\n"        \
            ".align 16\n" #name ":\n"         \
            ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n" \
            ".quad " #name "_end - " #name "\n" \
            ".previous\n");                   \
    extern const uint8_t name[];              \
    extern const size_t name##_size;

INCASSET(player_param, "launcher/param.json");
INCASSET(player_icon, "launcher/icon0.png");
INCASSET(player_pic1, "launcher/pic1.png");
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);

// Asks the system to register the title whose files are under /user/app/<id>.
static int register_app(const char *title_id)
{
    int (*install_dir)(const char *, const char *, void *) = 0;
    uint32_t handle;
    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
        install_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    return install_dir ? install_dir(title_id, "/user/app/", 0) : sceAppInstUtilAppInstallAll(0);
}

static int make_dirs(const char *path)
{
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

static int write_file(const char *path, const void *data, size_t size)
{
    if (make_dirs(path)) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = !size || fwrite(data, size, 1, f) == 1;
    if (fclose(f) || !ok) return -1;
    // The system refuses to spawn an eboot.bin or load a module without the execute bit.
    return chmod(path, 0777);
}

// ----------------------------------------------------------------- launcher

#define IOVEC_ENTRY(x) {x ? x : 0, x ? strlen(x) + 1 : 0}

static int remount_system_ex(void)
{
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_UPDATE);
}

// Removes a directory tree; `root` must be the player's own folder.
static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            struct stat st;
            if (!lstat(child, &st) && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}


// True when the file holds exactly these bytes.
static int same_content(const char *path, const uint8_t *data, size_t size)
{
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *buf = malloc(size ? size : 1);
    int same = buf && fread(buf, 1, size, f) == size && !memcmp(buf, data, size);
    free(buf);
    fclose(f);
    return same;
}

int install_player(void)
{
    if (same_content("/user/app/" PLAYER_ID "/sce_sys/param.json", player_param, player_param_size) &&
        same_content("/user/app/" PLAYER_ID "/sce_sys/icon0.png", player_icon, player_icon_size) &&
        same_content("/user/app/" PLAYER_ID "/sce_sys/pic1.png", player_pic1, player_pic1_size))
        return 0;

    int err = sceAppInstUtilInitialize();
    if (err) { printf("Player: sceAppInstUtilInitialize 0x%08X\n", err); return -1; }
    remount_system_ex();
    // A removed player can leave its system folder behind; start from nothing.
    remove_tree("/system_ex/app/" PLAYER_ID);
    if (mkdir("/system_ex/app/" PLAYER_ID, 0755) && errno != EEXIST) { perror("mkdir"); return -1; }
    if (write_file("/system_ex/app/" PLAYER_ID "/eboot.bin", NULL, 0) ||
        write_file("/system_ex/app/" PLAYER_ID "/sce_sys/param.json", player_param, player_param_size) ||        write_file("/user/app/" PLAYER_ID "/sce_sys/icon0.png", player_icon, player_icon_size) ||
        write_file("/user/app/" PLAYER_ID "/sce_sys/pic1.png", player_pic1, player_pic1_size) ||
        write_file("/user/app/" PLAYER_ID "/sce_sys/param.json", player_param, player_param_size)) {
        perror("Player: write");
        return -1;
    }
    err = register_app(PLAYER_ID);
    printf("Player %s: install 0x%08X\n", PLAYER_ID, (unsigned)err);
    fflush(stdout);
    return err ? -1 : 0;
}
