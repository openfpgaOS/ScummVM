/*
 * openfpga_fs.cpp -- POSIX-backed FS factory rooted at the ISO mount.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_fs.h"
#include "common/stream.h"
#include "common/bufferedstream.h"

extern "C" {
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
}

/* ScummVM points us at "/cd" (see main.cpp), but the engine also asks
 * for the current dir via "."  We treat them as the same root. */
#define OPENFPGA_FS_ROOT  "/cd"

namespace {

bool pathIsRoot(const Common::String &p) {
    return p.empty() || p == "." || p == "/" || p == OPENFPGA_FS_ROOT;
}

/* Stream wrapper around a POSIX fd.  The openfpgaOS stdio layer keeps
 * its own internal FILE buffers; traps in __stdio_read during streamed
 * audio indicate those buffers are not safe for this backend's mounted
 * ISO path.  Use unbuffered read/lseek here instead. */
class FdReadStream : public Common::SeekableReadStream {
public:
    FdReadStream(int fd, int64 sz) : _fd(fd), _size(sz), _eos(false), _err(false) {}
    ~FdReadStream() override { if (_fd >= 0) close(_fd); }

    bool eos() const override   { return _eos; }
    bool err() const override   { return _err; }
    void clearErr() override    { _err = false; }

    uint32 read(void *buf, uint32 cnt) override {
        byte *out = (byte *)buf;
        uint32 total = 0;

        while (cnt) {
            ssize_t got = ::read(_fd, out + total, cnt);
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                _err = true;
                break;
            }
            if (got == 0) {
                _eos = true;
                break;
            }
            total += (uint32)got;
            cnt -= (uint32)got;
        }

        return total;
    }

    int64 pos() const override {
        off_t p = lseek(_fd, 0, SEEK_CUR);
        return p < 0 ? -1 : (int64)p;
    }
    int64 size() const override { return _size; }

    bool seek(int64 offset, int whence = SEEK_SET) override {
        off_t p = lseek(_fd, (off_t)offset, whence);
        if (p < 0) {
            _err = true;
            return false;
        }
        _eos = false;
        return true;
    }

private:
    int   _fd;
    int64 _size;
    bool  _eos;
    bool  _err;
};

} // namespace

OpenFPGAFSNode::OpenFPGAFSNode()
    : _path(OPENFPGA_FS_ROOT), _name(OPENFPGA_FS_ROOT), _isDir(true) {}

OpenFPGAFSNode::OpenFPGAFSNode(const Common::String &path,
                               const Common::String &name, bool isDir)
    : _path(path), _name(name), _isDir(isDir) {}

OpenFPGAFSNode::OpenFPGAFSNode(const Common::String &path) {
    if (pathIsRoot(path)) {
        _path  = OPENFPGA_FS_ROOT;
        _name  = OPENFPGA_FS_ROOT;
        _isDir = true;
        return;
    }
    /* Engine handed us either "FOO.LFL" (relative to the game dir) or
     * "/cd/FOO.LFL" (absolute under the mount).  Normalise to the
     * latter so stat/open against the kernel mount works either way. */
    if (path.hasPrefix("/")) {
        _path = path;
    } else {
        _path = Common::String(OPENFPGA_FS_ROOT) + "/" + path;
    }
    const char *slash = strrchr(_path.c_str(), '/');
    _name  = slash ? Common::String(slash + 1) : _path;

    /* stat() may leave st_mode at 0 on this kernel mount; fall back
     * to probing with opendir() so subdirs are recognized. */
    struct stat st;
    _isDir = (::stat(_path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
    if (!_isDir) {
        DIR *probe = opendir(_path.c_str());
        if (probe) { _isDir = true; closedir(probe); }
    }
}

bool OpenFPGAFSNode::exists() const {
    struct stat st;
    if (::stat(_path.c_str(), &st) == 0) return true;

    /* Case-insensitive fallback for ISO 9660 uppercased names. */
    const char *slash = strrchr(_path.c_str(), '/');
    Common::String parent = slash
        ? Common::String(_path.c_str(), slash - _path.c_str())
        : Common::String(".");
    if (parent.empty()) parent = "/";

    DIR *d = opendir(parent.c_str());
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strcasecmp(e->d_name, _name.c_str()) == 0) { found = true; break; }
    }
    closedir(d);
    return found;
}

bool OpenFPGAFSNode::getChildren(AbstractFSList &list, ListMode mode, bool /*hidden*/) const {
    if (!_isDir) return false;

    DIR *d = opendir(_path.c_str());
    if (!d) return false;

    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        Common::String childPath = _path + "/" + e->d_name;

        /* Determine dir-ness conservatively.  The kernel's ISO mount
         * leaves d_type=DT_UNKNOWN and st_mode=0 for everything, so
         * neither hint is reliable; fall back to probing with
         * opendir().  SearchMan needs this to recurse so the engine
         * can find files inside subdirectories of the ISO. */
        bool entryIsDir = false;
        if (e->d_type == DT_DIR) {
            entryIsDir = true;
        } else if (e->d_type == DT_REG) {
            entryIsDir = false;
        } else {
            struct stat st;
            if (::stat(childPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                entryIsDir = true;
            } else {
                DIR *probe = opendir(childPath.c_str());
                if (probe) { entryIsDir = true; closedir(probe); }
            }
        }

        if (mode == Common::FSNode::kListDirectoriesOnly && !entryIsDir) continue;
        if (mode == Common::FSNode::kListFilesOnly       && entryIsDir)  continue;

        list.push_back(new OpenFPGAFSNode(childPath, e->d_name, entryIsDir));
    }
    closedir(d);
    return true;
}

Common::U32String OpenFPGAFSNode::getDisplayName() const { return Common::U32String(_name); }
Common::String    OpenFPGAFSNode::getName() const        { return _name; }
Common::String    OpenFPGAFSNode::getPath() const        { return _path; }
bool              OpenFPGAFSNode::isDirectory() const    { return _isDir; }
bool              OpenFPGAFSNode::isReadable() const     { return exists(); }
bool              OpenFPGAFSNode::isWritable() const     { return false; }

AbstractFSNode *OpenFPGAFSNode::getChild(const Common::String &name) const {
    if (!_isDir) return nullptr;
    Common::String childPath = _path;
    if (!childPath.empty() && childPath.lastChar() != '/') childPath += '/';
    childPath += name;
    return new OpenFPGAFSNode(childPath);
}

AbstractFSNode *OpenFPGAFSNode::getParent() const {
    if (pathIsRoot(_path)) return nullptr;
    const char *slash = strrchr(_path.c_str(), '/');
    if (!slash || slash == _path.c_str()) return new OpenFPGAFSNode();
    return new OpenFPGAFSNode(Common::String(_path.c_str(), slash - _path.c_str()));
}

Common::SeekableReadStream *OpenFPGAFSNode::createReadStream() {
    if (_isDir) return nullptr;

    int fd = open(_path.c_str(), O_RDONLY);

    /* ISO 9660 stores names upper-case but ScummVM asks lower-case.
     * If exact open failed, walk the parent dir for a case-insensitive
     * match and open that.  Cheap enough for the few-files-per-dir
     * structure SCUMM games have. */
    if (fd < 0) {
        const char *slash = strrchr(_path.c_str(), '/');
        Common::String parent = slash
            ? Common::String(_path.c_str(), slash - _path.c_str())
            : Common::String(".");
        if (parent.empty()) parent = "/";

        DIR *d = opendir(parent.c_str());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                if (strcasecmp(e->d_name, _name.c_str()) == 0) {
                    Common::String alt = parent + "/" + e->d_name;
                    fd = open(alt.c_str(), O_RDONLY);
                    break;
                }
            }
            closedir(d);
        }
    }

    if (fd < 0) return nullptr;

    /* Get the size from the directory record via fstat, NOT lseek(SEEK_END).
     * The engine re-opens the container per resource access (ScummEngine::
     * openFile -> file.open(_containerFile)), and on a streamed mount a
     * seek-to-end can be served by reading the file through to EOF -- which for
     * MI1's 1.24 GB monkey1.pak would mean re-reading the whole pak on every
     * open.  ISO9660 stores the size in the directory entry, so fstat is O(1).
     * Fall back to lseek only if fstat yields nothing usable. */
    off_t end;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        end = st.st_size;
    } else {
        end = lseek(fd, 0, SEEK_END);
        if (end < 0 || lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            return nullptr;
        }
    }

    /* The mounted ISO is streamed over the APF bridge, where every read()
     * is a syscall round-trip, and FdReadStream is deliberately unbuffered
     * (the kernel stdio buffer is unsafe on this path -- see above).  The
     * engine, though, parses files with byte- and word-granular reads:
     * SCUMM resource headers, the XWB/XSB indices, and worst of all MI2's
     * 11 MB speech.info, which Common::ReadStream::readString() walks one
     * readByte() at a time.  Unbuffered that is ~11 million bridge syscalls
     * for one file -- engineInit appears to hang.  Wrap every stream in a
     * memory buffer so those tiny reads are served locally and the bridge
     * only ever sees full-block reads.  The buffer lives in our address
     * space, so it sidesteps the kernel-stdio trap entirely. */
    Common::SeekableReadStream *raw = new FdReadStream(fd, (int64)end);
    return Common::wrapBufferedSeekableReadStream(raw, 32 * 1024, DisposeAfterUse::YES);
}

Common::SeekableWriteStream *OpenFPGAFSNode::createWriteStream(bool /*atomic*/) {
    return nullptr;            /* mount is read-only */
}

bool OpenFPGAFSNode::createDirectory() { return false; }

AbstractFSNode *OpenFPGAFilesystemFactory::makeCurrentDirectoryFileNode() const {
    return new OpenFPGAFSNode();
}

AbstractFSNode *OpenFPGAFilesystemFactory::makeFileNodePath(const Common::String &path) const {
    return new OpenFPGAFSNode(path);
}

AbstractFSNode *OpenFPGAFilesystemFactory::makeRootFileNode() const {
    return new OpenFPGAFSNode();
}
