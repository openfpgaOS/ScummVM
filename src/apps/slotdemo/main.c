/*
 * openfpgaOS Slot Demo
 * Discovers and lists files using standard POSIX opendir/readdir/stat.
 * No of_file_slot_register needed — the kernel auto-discovers from the bridge.
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int main(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[93mSlot Demo — File Discovery\033[0m\n\n");

    DIR *d = opendir("/");
    if (!d) {
        printf("  opendir failed\n");
        while (1) usleep(100 * 1000);
    }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int slot = (int)e->d_ino - 1;
        struct stat st;
        long sz = -1;
        if (stat(e->d_name, &st) == 0)
            sz = (long)st.st_size;
        printf("  slot %d: %-12s %7ld B\n", slot, e->d_name, sz);

        count++;
    }
    closedir(d);

    printf("\n  %d file(s)\n", count);

    while (1)
        usleep(100 * 1000);

    return 0;
}
