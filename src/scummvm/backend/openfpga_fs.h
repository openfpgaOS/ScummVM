/*
 * openfpga_fs.h -- Filesystem for openfpgaOS
 *
 * Minimal AbstractFSNode/FilesystemFactory that expose registered
 * APF data slot files as a flat directory at ".".
 */

#ifndef OPENFPGA_FS_H
#define OPENFPGA_FS_H

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "backends/fs/abstract-fs.h"
#include "backends/fs/fs-factory.h"

extern "C" {
#include <of.h>
}

#define OPENFPGA_MAX_FILES 16

struct OpenFPGAFileReg {
    uint32 slotId;
    char   filename[64];
    bool   used;
};

/* Global file registry */
void openfpga_fs_register(uint32 slotId, const char *filename);
int  openfpga_fs_count();
const OpenFPGAFileReg *openfpga_fs_get(int index);
const OpenFPGAFileReg *openfpga_fs_find(const char *name);

/* ── FSNode ──────────────────────────────────────────────────────── */

class OpenFPGAFSNode : public AbstractFSNode {
public:
    OpenFPGAFSNode();                            /* Root / current dir "." */
    OpenFPGAFSNode(const Common::String &path);  /* File or dir by path */

    bool exists() const override;
    bool getChildren(AbstractFSList &list, ListMode mode, bool hidden) const override;
    Common::U32String getDisplayName() const override;
    Common::String getName() const override;
    Common::String getPath() const override;
    bool isDirectory() const override;
    bool isReadable() const override;
    bool isWritable() const override;

    AbstractFSNode *getChild(const Common::String &name) const override;
    AbstractFSNode *getParent() const override;

    Common::SeekableReadStream *createReadStream() override;
    Common::SeekableWriteStream *createWriteStream(bool atomic) override;
    bool createDirectory() override;

private:
    Common::String _path;
    Common::String _name;
    bool _isDir;
};

/* ── Factory ─────────────────────────────────────────────────────── */

class OpenFPGAFilesystemFactory : public FilesystemFactory {
public:
    AbstractFSNode *makeCurrentDirectoryFileNode() const override;
    AbstractFSNode *makeFileNodePath(const Common::String &path) const override;
    AbstractFSNode *makeRootFileNode() const override;
};

#endif
