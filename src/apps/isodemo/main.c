/*
 * isodemo -- mount an ISO 9660 data slot and use POSIX file APIs under /cd.
 *
 * Add an APF data slot named "isodemo.iso" next to this app.  After
 * of_iso_mount(), standard opendir/stat/fopen calls work with paths below
 * the mountpoint.
 */

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "of.h"

static void park(void) {
    for (;;) usleep(100 * 1000);
}

int main(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[93mISO Demo\033[0m\n\n");

    int rc = of_iso_mount("isodemo.iso", "/cd");
    if (rc < 0) {
        printf("  of_iso_mount failed: %d\n", rc);
        printf("  Add isodemo.iso as an APF data slot.\n");
        park();
    }

    DIR *d = opendir("/cd");
    if (!d) {
        printf("  opendir(/cd) failed\n");
        park();
    }

    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "/cd/%s", e->d_name);
        long sz = (stat(path, &st) == 0) ? (long)st.st_size : -1;
        printf("  %-32s %8ld B%s\n",
               e->d_name, sz, (e->d_type == DT_DIR) ? "/" : "");
        count++;
    }
    closedir(d);

    printf("\n  %d entr%s\n", count, count == 1 ? "y" : "ies");
    park();
    return 0;
}
