/*
 * openfpga_cue_archive.h -- Common::Archive that mounts a .cue + .bin
 *                            (MODE1/2352) disc image as if it were a
 *                            normal directory tree.
 *
 * Drives a CD rip created by the typical "extract data + audio
 * tracks" tools (cdrdao, ImgBurn, etc.) directly, no offline
 * conversion to .iso required.  Audio tracks are exposed by the
 * companion OpenFPGAAudioCDManager via the same cue sheet.
 *
 * Approach: parse the cue, find the data track (MODE1/2352), open the
 * .bin file, wrap it in a SeekableReadStream that strips the per-sector
 * 16-byte sync header so the upper layer sees a clean 2048-byte-per-
 * sector cooked image, then parse ISO 9660 directory records off that
 * cooked view.  Files are returned as SubReadStreams into the cooked
 * stream.
 */

#ifndef OPENFPGA_CUE_ARCHIVE_H
#define OPENFPGA_CUE_ARCHIVE_H

#include "common/archive.h"
#include "common/hashmap.h"
#include "common/path.h"
#include "common/stream.h"
#include "common/str.h"

namespace OpenFPGA {

class Mode1RawStream;

class CueArchive : public Common::Archive {
public:
    static CueArchive *create(const Common::String &cuePath);

    ~CueArchive() override;

    bool hasFile(const Common::Path &path) const override;
    int listMembers(Common::ArchiveMemberList &list) const override;
    const Common::ArchiveMemberPtr getMember(const Common::Path &path) const override;
    Common::SeekableReadStream *createReadStreamForMember(const Common::Path &path) const override;

private:
    struct Entry {
        uint32 lba;       /* LBA in 2048-byte cooked sectors */
        uint32 size;      /* byte size */
    };

    CueArchive();
    bool parseISO9660();
    bool parseDirectory(uint32 lba, uint32 size, const Common::String &prefix);

    Mode1RawStream *_cooked;             /* owns the underlying .bin */
    Common::HashMap<Common::String, Entry,
                    Common::IgnoreCase_Hash, Common::IgnoreCase_EqualTo> _files;
};

} // namespace OpenFPGA

#endif
