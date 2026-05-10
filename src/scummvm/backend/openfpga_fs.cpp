/*
 * openfpga_fs.cpp -- Filesystem implementation for openfpgaOS
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_fs.h"
#include "common/stream.h"

extern "C" {
#include <string.h>
#include <stdio.h>
}

/* ═══════════════════════════════════════════════════════════════════
 * Global file registry
 * ═══════════════════════════════════════════════════════════════════ */

static OpenFPGAFileReg g_files[OPENFPGA_MAX_FILES];
static int g_fileCount = 0;

void openfpga_fs_register(uint32 slotId, const char *filename) {
    if (g_fileCount >= OPENFPGA_MAX_FILES) return;
    OpenFPGAFileReg *e = &g_files[g_fileCount];
    e->slotId = slotId;
    strncpy(e->filename, filename, 63);
    e->filename[63] = '\0';
    e->used = true;
    of_file_slot_register(slotId, filename);
    g_fileCount++;
}

int openfpga_fs_count() { return g_fileCount; }

const OpenFPGAFileReg *openfpga_fs_get(int index) {
    if (index < 0 || index >= g_fileCount) return nullptr;
    return &g_files[index];
}

const OpenFPGAFileReg *openfpga_fs_find(const char *name) {
    for (int i = 0; i < g_fileCount; i++) {
        if (strcasecmp(g_files[i].filename, name) == 0)
            return &g_files[i];
    }
    return nullptr;
}

/* ═══════════════════════════════════════════════════════════════════
 * StdioReadStream — wraps FILE* for ScummVM
 * ═══════════════════════════════════════════════════════════════════ */

class OpenFPGAReadStream : public Common::SeekableReadStream {
public:
    OpenFPGAReadStream(FILE *f, long size) : _f(f), _size(size) {}
    ~OpenFPGAReadStream() override { if (_f) fclose(_f); }

    bool eos() const override { return feof(_f) != 0; }
    bool err() const override { return ferror(_f) != 0; }
    void clearErr() override { clearerr(_f); }

    uint32 read(void *buf, uint32 cnt) override {
        return (uint32)fread(buf, 1, cnt, _f);
    }

    int64 pos() const override { return ftell(_f); }
    int64 size() const override { return _size; }

    bool seek(int64 offset, int whence = SEEK_SET) override {
        return fseek(_f, (long)offset, whence) == 0;
    }

private:
    FILE *_f;
    long  _size;
};

/* ═══════════════════════════════════════════════════════════════════
 * OpenFPGAFSNode
 * ═══════════════════════════════════════════════════════════════════ */

OpenFPGAFSNode::OpenFPGAFSNode() : _path("."), _name("."), _isDir(true) {}

OpenFPGAFSNode::OpenFPGAFSNode(const Common::String &path)
    : _path(path), _isDir(false) {
    /* Extract filename from path */
    const char *slash = strrchr(path.c_str(), '/');
    _name = slash ? Common::String(slash + 1) : path;

    /* Check if it's the root/current dir */
    if (path == "." || path == "/" || path.empty()) {
        _isDir = true;
        _name = ".";
    }
}

bool OpenFPGAFSNode::exists() const {
    if (_isDir) return true;
    return openfpga_fs_find(_name.c_str()) != nullptr;
}

bool OpenFPGAFSNode::getChildren(AbstractFSList &list, ListMode mode, bool hidden) const {
    if (!_isDir) return false;

    /* List all registered files */
    for (int i = 0; i < openfpga_fs_count(); i++) {
        const OpenFPGAFileReg *e = openfpga_fs_get(i);
        if (e && e->used) {
            if (mode == Common::FSNode::kListDirectoriesOnly)
                continue; /* No subdirectories */
            list.push_back(new OpenFPGAFSNode(Common::String(e->filename)));
        }
    }
    return true;
}

Common::U32String OpenFPGAFSNode::getDisplayName() const {
    return Common::U32String(_name);
}

Common::String OpenFPGAFSNode::getName() const { return _name; }
Common::String OpenFPGAFSNode::getPath() const { return _path; }
bool OpenFPGAFSNode::isDirectory() const { return _isDir; }
bool OpenFPGAFSNode::isReadable() const { return exists(); }
bool OpenFPGAFSNode::isWritable() const { return false; }

AbstractFSNode *OpenFPGAFSNode::getChild(const Common::String &name) const {
    if (!_isDir) return nullptr;
    /* Return a node for a file in this directory */
    return new OpenFPGAFSNode(name);
}

AbstractFSNode *OpenFPGAFSNode::getParent() const {
    return new OpenFPGAFSNode(); /* Always return root */
}

Common::SeekableReadStream *OpenFPGAFSNode::createReadStream() {
    if (_isDir) return nullptr;

    FILE *f = fopen(_name.c_str(), "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    return new OpenFPGAReadStream(f, sz);
}

Common::SeekableWriteStream *OpenFPGAFSNode::createWriteStream(bool atomic) {
    return nullptr; /* Read-only filesystem */
}

bool OpenFPGAFSNode::createDirectory() {
    return false;
}

/* ═══════════════════════════════════════════════════════════════════
 * OpenFPGAFilesystemFactory
 * ═══════════════════════════════════════════════════════════════════ */

AbstractFSNode *OpenFPGAFilesystemFactory::makeCurrentDirectoryFileNode() const {
    return new OpenFPGAFSNode();
}

AbstractFSNode *OpenFPGAFilesystemFactory::makeFileNodePath(const Common::String &path) const {
    return new OpenFPGAFSNode(path);
}

AbstractFSNode *OpenFPGAFilesystemFactory::makeRootFileNode() const {
    return new OpenFPGAFSNode();
}
