//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * openfpgaOS SDK — MiSTer disk-image builder.
 *
 * Assembles the FAT32 .vhd that the openfpgaOS MiSTer core mounts via
 * the OSD.  Built on FatFs itself (the same implementation the firmware
 * uses to mount the image), so format compatibility is exact and the
 * nonvolatile slot files are preallocated CONTIGUOUSLY with f_expand —
 * the firmware's power-cut safety story depends on never touching FAT
 * metadata during a save write.
 *
 * Image layout (the slot→path contract in targets/mister/file.c):
 *   /os.ini                  OS config        (slot 2)
 *   /app.elf                 default app      (slot 3)
 *   /bank.ofsf               MIDI soundfont   (slot 7, optional)
 *   /config/shared.cfg       SDK shared cfg   (slot 8,  256 KB prealloc)
 *   /config/duke3d.cfg       per-game cfg     (slot 9,  256 KB prealloc)
 *   /saves/slot_0..9.sav     save slots       (10-19,   256 KB prealloc)
 *   /assets/<files>          app data, registered by directory scan
 *
 * Usage:
 *   mkimage <out.vhd> <size_mb> [src=dst]...
 *
 * Each src=dst copies a host file into the image (dst is an absolute
 * in-image path, e.g. game.elf=/app.elf or sfx.bin=/assets/sfx.bin).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#define SECTOR 512u
#define NV_SLOT_BYTES (256u * 1024u)
/* Must match the firmware's OF_TARGET_SAVE_MAX_SLOTS
 * (openfpgaOS: src/firmware/os/targets/mister/target_platform.h) — the
 * runtime maps save slot ids 10..(10+N-1) onto these files and refuses
 * writes to files that don't exist at full size. */
#define NV_SAVE_SLOTS 10

/* ── file-backed diskio ─────────────────────────────────────────── */

static int img_fd = -1;
static LBA_t img_sectors;

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return img_fd >= 0 ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    ssize_t n = pread(img_fd, buff, (size_t)count * SECTOR, (off_t)sector * SECTOR);
    return n == (ssize_t)((size_t)count * SECTOR) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    ssize_t n = pwrite(img_fd, buff, (size_t)count * SECTOR, (off_t)sector * SECTOR);
    return n == (ssize_t)((size_t)count * SECTOR) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = img_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default:               return RES_PARERR;
    }
}

/* ── helpers ────────────────────────────────────────────────────── */

static void die(const char *what, int rc) {
    fprintf(stderr, "mkimage: %s failed (%d)\n", what, rc);
    exit(1);
}

static void make_nv_slot(const char *path) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) die(path, fr);
    fr = f_expand(&f, NV_SLOT_BYTES, 1);     /* contiguous, or fail */
    if (fr != FR_OK) die("f_expand", fr);
    /* Zero the payload so logically-empty slots read deterministically. */
    static BYTE zeros[4096];
    UINT bw;
    for (UINT done = 0; done < NV_SLOT_BYTES; done += sizeof(zeros)) {
        fr = f_write(&f, zeros, sizeof(zeros), &bw);
        if (fr != FR_OK || bw != sizeof(zeros)) die("zero-fill", fr);
    }
    f_close(&f);
    printf("  nv   %s (%u KB, contiguous)\n", path, NV_SLOT_BYTES / 1024);
}

static void copy_in(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "mkimage: cannot open %s\n", src); exit(1); }

    FIL f;
    FRESULT fr = f_open(&f, dst, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) die(dst, fr);

    static BYTE buf[65536];
    size_t n;
    UINT bw;
    long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fr = f_write(&f, buf, (UINT)n, &bw);
        if (fr != FR_OK || bw != n) die("f_write", fr);
        total += (long)n;
    }
    f_close(&f);
    fclose(in);
    printf("  copy %s -> %s (%ld bytes)\n", src, dst, total);
}

/* ── main ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.vhd> <size_mb> [src=dst]...\n", argv[0]);
        return 1;
    }

    const char *out = argv[1];
    long mb = strtol(argv[2], NULL, 10);
    if (mb < 48 || mb > 4095) {
        fprintf(stderr, "mkimage: size must be 48..4095 MB (FAT32)\n");
        return 1;
    }

    img_fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (img_fd < 0) { perror(out); return 1; }
    if (ftruncate(img_fd, (off_t)mb * 1024 * 1024) != 0) { perror("ftruncate"); return 1; }
    img_sectors = (LBA_t)((unsigned long long)mb * 1024 * 1024 / SECTOR);

    static FATFS fs;
    static BYTE work[64 * 1024];
    MKFS_PARM parm = { FM_FAT32 | FM_SFD, 0, 0, 0, 0 };  /* no MBR — bare FAT */
    FRESULT fr = f_mkfs("", &parm, work, sizeof(work));
    if (fr != FR_OK) die("f_mkfs", fr);
    fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) die("f_mount", fr);

    printf("mkimage: %s (%ld MB FAT32)\n", out, mb);

    f_mkdir("/saves");
    f_mkdir("/config");
    f_mkdir("/assets");

    /* Nonvolatile slots — preallocated, contiguous, zeroed. */
    char path[32];
    for (int i = 0; i < NV_SAVE_SLOTS; i++) {
        snprintf(path, sizeof(path), "/saves/slot_%d.sav", i);
        make_nv_slot(path);
    }
    make_nv_slot("/config/shared.cfg");
    make_nv_slot("/config/duke3d.cfg");

    /* Payload files. */
    for (int i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i] || eq[1] != '/') {
            fprintf(stderr, "mkimage: bad spec '%s' (want src=/dst)\n", argv[i]);
            return 1;
        }
        *eq = '\0';
        copy_in(argv[i], eq + 1);
    }

    f_unmount("");
    close(img_fd);
    printf("mkimage: done\n");
    return 0;
}
