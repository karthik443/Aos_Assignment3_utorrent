#ifndef CLIENT_FILEOPS_H
#define CLIENT_FILEOPS_H

// Piece splitting/hashing and the client's local "what files do I have
// bytes for" registry. The registry is shared between three things: the
// CLI (upload_file/stop_share), the peer server (serves GET_PIECE requests
// out of it), and the download manager (writes newly-verified pieces into
// it). Everything is behind LocalFileRecord::mx / a global registry mutex
// so concurrent access from those threads is safe.

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>

#include "../common/protocol.h"
#include "../common/sha1.h"
#include "../common/utils.h"

using namespace std;

struct FileMeta {
    long long fileSize = 0;
    int numPieces = 0;
    string fileHash;
    vector<string> pieceHashes;
};

// Streams the file in PIECE_SIZE chunks (never holds more than one piece in
// memory at once, per the "handle up to 1GB without excessive memory usage"
// requirement) computing both the per-piece and whole-file SHA1 hashes in a
// single pass.
inline FileMeta computeFileMeta(const string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw runtime_error("Cannot open file: " + path);
    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); throw runtime_error("fstat failed on: " + path); }

    FileMeta meta;
    meta.fileSize = st.st_size;
    meta.numPieces = (int)((meta.fileSize + PIECE_SIZE - 1) / PIECE_SIZE);
    if (meta.numPieces == 0) meta.numPieces = 0; // empty file: zero pieces

    SHA1 wholeFile;
    vector<char> buf(PIECE_SIZE);
    long long remaining = meta.fileSize;
    while (remaining > 0) {
        size_t want = (size_t)min<long long>(PIECE_SIZE, remaining);
        size_t got = 0;
        while (got < want) {
            ssize_t n = read(fd, buf.data() + got, want - got);
            if (n <= 0) { close(fd); throw runtime_error("Unexpected EOF/error reading: " + path); }
            got += (size_t)n;
        }
        SHA1 piece;
        piece.update(reinterpret_cast<const uint8_t *>(buf.data()), got);
        meta.pieceHashes.push_back(piece.finalHex());
        wholeFile.update(reinterpret_cast<const uint8_t *>(buf.data()), got);
        remaining -= (long long)got;
    }
    meta.fileHash = wholeFile.finalHex();
    close(fd);
    return meta;
}

struct LocalFileRecord {
    string groupId, fileName, localPath;
    long long fileSize = 0;
    int numPieces = 0;
    string fileHash;
    vector<string> pieceHashes;
    vector<bool> haveBitmap; // pieces verified present on disk at localPath
    bool sharingStopped = false;
    mutex mx;
};

inline mutex &registryMutex() {
    static mutex m;
    return m;
}
inline unordered_map<string, shared_ptr<LocalFileRecord>> &localFiles() {
    static unordered_map<string, shared_ptr<LocalFileRecord>> reg;
    return reg;
}
inline string fileKey(const string &groupId, const string &fileName) { return groupId + "|" + fileName; }

inline shared_ptr<LocalFileRecord> findLocalFile(const string &groupId, const string &fileName) {
    lock_guard<mutex> lk(registryMutex());
    auto it = localFiles().find(fileKey(groupId, fileName));
    return it == localFiles().end() ? nullptr : it->second;
}
inline void registerLocalFile(shared_ptr<LocalFileRecord> rec) {
    lock_guard<mutex> lk(registryMutex());
    localFiles()[fileKey(rec->groupId, rec->fileName)] = rec;
}

// Reads exactly one piece's bytes off disk (pread is atomic w.r.t. its
// offset argument, so this is safe to call concurrently from multiple
// peer-serving threads on the same fd/path).
inline bool readPieceFromDisk(const string &path, long long pieceIndex, long long pieceSize, string &out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.resize(pieceSize);
    off_t offset = pieceIndex * PIECE_SIZE;
    size_t got = 0;
    bool ok = true;
    while (got < (size_t)pieceSize) {
        ssize_t n = pread(fd, &out[0] + got, pieceSize - got, offset + (off_t)got);
        if (n <= 0) { ok = false; break; }
        got += (size_t)n;
    }
    close(fd);
    return ok;
}

inline bool writePieceToDisk(const string &path, long long pieceIndex, const string &bytes) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return false;
    off_t offset = pieceIndex * PIECE_SIZE;
    size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        ssize_t n = pwrite(fd, bytes.data() + written, bytes.size() - written, offset + (off_t)written);
        if (n <= 0) { ok = false; break; }
        written += (size_t)n;
    }
    close(fd);
    return ok;
}

#endif // CLIENT_FILEOPS_H
