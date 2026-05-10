/*
 * openfpga_save.cpp -- Save file manager implementation
 *
 * The new SDK exposes APF save slots through POSIX paths of the form
 * "save:N". fopen("save:N", "rb"/"wb") reads/writes the slot; fclose()
 * auto-flushes a write back to SD card with the actual byte count.
 */

#include "openfpga_save.h"

extern "C" {
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
}

OpenFPGASaveManager::OpenFPGASaveManager() {
}

OpenFPGASaveManager::~OpenFPGASaveManager() {
}

static FILE *openSlot(int slot, const char *mode) {
    char path[16];
    snprintf(path, sizeof(path), "save:%d", slot);
    return fopen(path, mode);
}

bool OpenFPGASaveManager::readHeader(int slot, SaveSlotHeader *hdr) const {
    FILE *f = openSlot(slot, "rb");
    if (!f) return false;
    size_t n = fread(hdr, 1, sizeof(SaveSlotHeader), f);
    fclose(f);
    if (n != sizeof(SaveSlotHeader)) return false;
    return hdr->magic == SAVE_MAGIC;
}

int OpenFPGASaveManager::findSlot(const char *filename) const {
    SaveSlotHeader hdr;
    for (int i = 0; i < OPENFPGA_MAX_SAVES; i++) {
        if (readHeader(i, &hdr)) {
            if (strncmp(hdr.filename, filename, SAVE_NAME_MAX) == 0)
                return i;
        }
    }
    return -1;
}

int OpenFPGASaveManager::findFreeSlot() const {
    SaveSlotHeader hdr;
    for (int i = 0; i < OPENFPGA_MAX_SAVES; i++) {
        if (!readHeader(i, &hdr))
            return i;
    }
    /* All slots used — overwrite slot 0 (could be smarter) */
    return 0;
}

int OpenFPGASaveManager::loadSave(const char *filename, uint8_t **outData) {
    int slot = findSlot(filename);
    if (slot < 0)
        return -1;

    FILE *f = openSlot(slot, "rb");
    if (!f) return -1;

    SaveSlotHeader hdr;
    if (fread(&hdr, 1, sizeof(SaveSlotHeader), f) != sizeof(SaveSlotHeader) ||
        hdr.magic != SAVE_MAGIC || hdr.dataLen > SAVE_DATA_MAX) {
        fclose(f);
        return -1;
    }

    uint8_t *data = (uint8_t *)malloc(hdr.dataLen);
    if (!data) { fclose(f); return -1; }

    if (fread(data, 1, hdr.dataLen, f) != hdr.dataLen) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    *outData = data;
    return (int)hdr.dataLen;
}

int OpenFPGASaveManager::writeSave(const char *filename, const uint8_t *data, uint32_t len) {
    if (len > SAVE_DATA_MAX)
        return -1;

    /* Find existing slot or allocate new one */
    int slot = findSlot(filename);
    if (slot < 0)
        slot = findFreeSlot();

    FILE *f = openSlot(slot, "wb");
    if (!f) return -1;

    /* Write header */
    SaveSlotHeader hdr;
    hdr.magic = SAVE_MAGIC;
    memset(hdr.filename, 0, SAVE_NAME_MAX);
    strncpy(hdr.filename, filename, SAVE_NAME_MAX - 1);
    hdr.dataLen = len;

    if (fwrite(&hdr, 1, sizeof(SaveSlotHeader), f) != sizeof(SaveSlotHeader)) {
        fclose(f);
        return -1;
    }

    /* Write data */
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }

    /* fclose() auto-flushes to SD with the actual write size */
    fclose(f);
    return 0;
}

int OpenFPGASaveManager::deleteSave(const char *filename) {
    int slot = findSlot(filename);
    if (slot < 0)
        return -1;
    /* Truncate by opening for write and immediately closing — writes
     * a zero-byte save, which clears the magic and effectively erases. */
    FILE *f = openSlot(slot, "wb");
    if (!f) return -1;
    fclose(f);
    return 0;
}

int OpenFPGASaveManager::listSaves(const char *pattern, char names[][SAVE_NAME_MAX], int maxEntries) {
    int count = 0;
    SaveSlotHeader hdr;

    for (int i = 0; i < OPENFPGA_MAX_SAVES && count < maxEntries; i++) {
        if (readHeader(i, &hdr)) {
            /* Simple wildcard match: only support trailing '*' */
            const char *star = strchr(pattern, '*');
            if (star) {
                int prefixLen = (int)(star - pattern);
                if (strncmp(hdr.filename, pattern, prefixLen) == 0) {
                    strncpy(names[count], hdr.filename, SAVE_NAME_MAX - 1);
                    names[count][SAVE_NAME_MAX - 1] = '\0';
                    count++;
                }
            } else {
                if (strcmp(hdr.filename, pattern) == 0) {
                    strncpy(names[count], hdr.filename, SAVE_NAME_MAX - 1);
                    names[count][SAVE_NAME_MAX - 1] = '\0';
                    count++;
                }
            }
        }
    }
    return count;
}
