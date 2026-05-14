/*
 * openfpga_fs.h -- ScummVM filesystem for openfpgaOS.
 *
 * Backed by the kernel's mount API: main() calls of_iso_mount() on the
 * game ISO and the engine uses standard POSIX paths under the mount
 * (e.g. "/cd/000.LFL").  This factory just adapts AbstractFSNode to
 * fopen / opendir / stat -- there's no app-side ISO reader anymore.
 */

#ifndef OPENFPGA_FS_H
#define OPENFPGA_FS_H

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "backends/fs/abstract-fs.h"
#include "backends/fs/fs-factory.h"

class OpenFPGAFSNode : public AbstractFSNode {
public:
    OpenFPGAFSNode();                            /* root of the mount */
    OpenFPGAFSNode(const Common::String &path);

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
    OpenFPGAFSNode(const Common::String &path, const Common::String &name, bool isDir);

    Common::String _path;
    Common::String _name;
    bool           _isDir;
};

class OpenFPGAFilesystemFactory : public FilesystemFactory {
public:
    AbstractFSNode *makeCurrentDirectoryFileNode() const override;
    AbstractFSNode *makeFileNodePath(const Common::String &path) const override;
    AbstractFSNode *makeRootFileNode() const override;
};

#endif
