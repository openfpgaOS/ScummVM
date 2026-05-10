/*
 * openfpga_save.h -- Save file manager for openfpgaOS
 *
 * Maps ScummVM save files to APF save slots (10 slots, 256KB each).
 * Each slot stores one save file with a simple header.
 */

#ifndef OPENFPGA_SAVE_H
#define OPENFPGA_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif
#include <of.h>
#include <stdint.h>
#ifdef __cplusplus
}
#endif

#define OPENFPGA_MAX_SAVES 10
#define OPENFPGA_SAVE_SIZE (256 * 1024)

#define SAVE_MAGIC      0x53564D53  /* 'SVMS' */
#define SAVE_NAME_MAX   64
#define SAVE_HEADER_SIZE (4 + SAVE_NAME_MAX + 4)  /* magic + name + data_len */
#define SAVE_DATA_MAX   (OPENFPGA_SAVE_SIZE - SAVE_HEADER_SIZE)

struct SaveSlotHeader {
    uint32_t magic;
    char     filename[SAVE_NAME_MAX];
    uint32_t dataLen;
};

/*
 * OpenFPGASaveManager — manages save file I/O via APF save slots.
 *
 * Will eventually implement ScummVM's Common::SaveFileManager.
 */
class OpenFPGASaveManager {
public:
    OpenFPGASaveManager();
    ~OpenFPGASaveManager();

    /* Find a slot containing the given filename, or -1 */
    int findSlot(const char *filename) const;

    /* Find an empty slot, or the oldest one to overwrite */
    int findFreeSlot() const;

    /* Read a save file into a malloc'd buffer. Returns data length, or -1. */
    int loadSave(const char *filename, uint8_t **outData);

    /* Write a save file to a slot. Returns 0 on success. */
    int writeSave(const char *filename, const uint8_t *data, uint32_t len);

    /* Delete a save file */
    int deleteSave(const char *filename);

    /* List all save files matching a pattern (e.g. "tentacle.s*") */
    int listSaves(const char *pattern, char names[][SAVE_NAME_MAX], int maxEntries);

private:
    /* Read the header of a save slot */
    bool readHeader(int slot, SaveSlotHeader *hdr) const;
};

#endif /* OPENFPGA_SAVE_H */
