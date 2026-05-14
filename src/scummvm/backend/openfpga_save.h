/*
 * openfpga_save.h -- Save-file manager for openfpgaOS.
 *
 * Backs ScummVM's Common::SaveFileManager with the 10 nonvolatile APF
 * save slots exposed by the SDK as POSIX paths "save:0".."save:9".
 * Each slot stores one save under a fixed-size header that records
 * the original ScummVM filename (e.g. "monkey-001.s00") so engine code
 * doesn't have to know about slot IDs.
 */

#ifndef OPENFPGA_SAVE_H
#define OPENFPGA_SAVE_H

#include "common/savefile.h"

#define OPENFPGA_MAX_SAVES   10
#define OPENFPGA_SAVE_SIZE   (256 * 1024)
#define SAVE_MAGIC           0x53564D53u   /* 'SVMS' */
#define SAVE_NAME_MAX        64
#define SAVE_HEADER_SIZE     (4 + SAVE_NAME_MAX + 4)
#define SAVE_DATA_MAX        (OPENFPGA_SAVE_SIZE - SAVE_HEADER_SIZE)

struct SaveSlotHeader {
    uint32 magic;
    char   filename[SAVE_NAME_MAX];
    uint32 dataLen;
};

/* ScummVM-facing SaveFileManager backed by APF save slots. */
class OpenFPGASaveFileManager : public Common::SaveFileManager {
public:
    Common::InSaveFile  *openRawFile(const Common::String &name) override;
    Common::InSaveFile  *openForLoading(const Common::String &name) override;
    Common::OutSaveFile *openForSaving(const Common::String &name, bool compress = true) override;
    bool                 removeSavefile(const Common::String &name) override;
    Common::StringArray  listSavefiles(const Common::String &pattern) override;
    void                 updateSavefilesList(Common::StringArray &lockedFiles) override;
    bool                 exists(const Common::String &name) override;

private:
    static bool   readHeader(int slot, SaveSlotHeader *hdr);
    static int    findSlotByName(const char *name);
    static int    findFreeSlot();
};

#endif /* OPENFPGA_SAVE_H */
