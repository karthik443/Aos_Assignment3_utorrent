#ifndef CLIENT_DOWNLOADER_H
#define CLIENT_DOWNLOADER_H

// Download manager: for each download_file call, queries the tracker for
// peers + piece hashes, then fetches pieces in parallel from multiple
// peers using a rarest-first piece selection strategy (classic
// BitTorrent - prioritizing scarce pieces keeps the overall swarm
// healthier and avoids everyone converging on the same popular piece).
// Multiple downloads (different files) run as independent worker pools
// concurrently, satisfying "multiple simultaneous downloads, each pulling
// from multiple peers at once".
//
// Each piece is verified against its expected SHA1 the moment it arrives;
// a bad piece is discarded and re-requested from a different peer. Once
// every piece is in and verified, the whole file's SHA1 is checked too
// (belt-and-braces, per the spec) before the .part file is atomically
// renamed to the requested destination.

#include <string>
#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>

#include "../common/utils.h"
#include "../common/netio.h"
#include "../common/protocol.h"
#include "../common/sha1.h"
#include "fileops.h"
#include "tracker_client.h"
#include "reciprocity.h"

using namespace std;

struct PeerInfo {
    string userId, ip;
    int port;
    vector<bool> bitmap;
};

struct DownloadStatus {
    string groupId, fileName;
    int totalPieces = 0;
    atomic<int> donePieces{0};
    atomic<bool> completed{false};
    atomic<bool> failed{false};
    string errorMsg;

    // Which seeder each piece actually came from, so a user can see - both
    // live (via the per-piece log line in runDownloadPass) and afterwards
    // (via show_downloads) - that a download really is pulling from
    // multiple peers rather than just one.
    mutex sourceMx;                             // guards the two fields below
    unordered_map<string, int> piecesFromPeer;  // userId -> pieces fetched from them so far
    vector<string> pieceSource;                 // pieceSource[i] = userId that supplied piece i ("" if not yet fetched)
};

inline mutex &downloadsMutex() {
    static mutex m;
    return m;
}
inline vector<shared_ptr<DownloadStatus>> &activeDownloads() {
    static vector<shared_ptr<DownloadStatus>> v;
    return v;
}

// "bob:2, carol:1" - how many pieces of this download came from each peer
// so far. Empty once nothing has completed yet.
inline string peerBreakdown(DownloadStatus &d) {
    lock_guard<mutex> lk(d.sourceMx);
    string out;
    for (auto &[peer, cnt] : d.piecesFromPeer) {
        if (!out.empty()) out += ", ";
        out += peer + ":" + to_string(cnt);
    }
    return out;
}

inline string formatShowDownloads() {
    lock_guard<mutex> lk(downloadsMutex());
    if (activeDownloads().empty()) return "No downloads yet.";
    string out;
    for (auto &d : activeDownloads()) {
        string breakdown = peerBreakdown(*d);
        if (d->completed) {
            // Kept byte-for-byte per the spec's required completion format;
            // the per-seeder breakdown goes on its own line right after.
            out += "[C] [" + d->groupId + "] " + d->fileName + "\n";
            if (!breakdown.empty()) out += "      pieces came from: " + breakdown + "\n";
        } else if (d->failed) {
            out += "[F] [" + d->groupId + "] " + d->fileName + " - " + d->errorMsg;
            if (!breakdown.empty()) out += " (pieces so far from: " + breakdown + ")";
            out += "\n";
        } else {
            out += "[D] [" + d->groupId + "] " + d->fileName + " " + to_string(d->donePieces) + "/" + to_string(d->totalPieces) + " pieces";
            if (!breakdown.empty()) out += " (from: " + breakdown + ")";
            out += "\n";
        }
    }
    return out;
}

// Connects to a peer, fetches one piece, verifies it and writes it to
// disk. Returns false on any failure (network error, ERR response - which
// includes a peer choking us because its upload slots are full, see
// peer_server.h - or a hash mismatch) so the caller can retry against a
// different peer. `myUserId` identifies us to the peer so it can credit us
// for reciprocity next time we're the one serving pieces.
inline bool fetchPiece(const PeerInfo &peer, LocalFileRecord &rec, int idx, const string &myUserId) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    hostent *he = gethostbyname(peer.ip.c_str());
    if (!he) { close(fd); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer.port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return false; }

    string req = "GET_PIECE " + myUserId + " " + rec.groupId + " " + rec.fileName + " " + to_string(idx);
    if (!sendFrame(fd, req)) { close(fd); return false; }
    string header;
    if (!recvFrame(fd, header)) { close(fd); return false; }
    vector<string> h = split(header, ' ');
    if (h.empty() || h[0] != "OK") { close(fd); return false; } // includes ERR BUSY (choked) - caller just tries someone else
    string data;
    if (!recvFrame(fd, data)) { close(fd); return false; }
    close(fd);

    string actualHash = SHA1::hash(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    if (rec.pieceHashes[idx] != actualHash) return false; // corrupted in transit - caller retries elsewhere
    if (!writePieceToDisk(rec.localPath, idx, data)) return false;
    recordPieceReceivedFrom(peer.userId); // tit-for-tat: this peer just earned goodwill with us
    return true;
}

// Runs one rarest-first pass over `pending`, using up to `numWorkers`
// threads pulling from a shared queue. Pieces that fail against every
// currently-known peer are left in `pending` for the caller to retry
// (typically after refreshing the peer list once).
inline void runDownloadPass(vector<PeerInfo> &peers, LocalFileRecord &rec,
                             shared_ptr<DownloadStatus> status, vector<int> &pending, const string &myUserId) {
    // Rarest-first ordering.
    unordered_map<int, int> rarity;
    for (int idx : pending) {
        int c = 0;
        for (auto &p : peers) if (idx < (int)p.bitmap.size() && p.bitmap[idx]) c++;
        rarity[idx] = c;
    }
    sort(pending.begin(), pending.end(), [&](int a, int b) { return rarity[a] < rarity[b]; });

    mutex queueMx;
    deque<int> queue(pending.begin(), pending.end());
    unordered_map<int, unordered_set<string>> triedPeers; // pieceIdx -> userIds already tried this pass
    vector<int> stillPending;
    atomic<size_t> rrCounter{0}; // round-robins piece assignment across equally-viable peers

    int numWorkers = max(1, min(4, (int)peers.size()));
    vector<thread> workers;
    for (int w = 0; w < numWorkers; w++) {
        workers.emplace_back([&]() {
            while (true) {
                int idx = -1;
                {
                    lock_guard<mutex> lk(queueMx);
                    if (queue.empty()) return;
                    idx = queue.front();
                    queue.pop_front();
                }
                // Pick a peer that has this piece and hasn't already failed us
                // this pass, rotating across all currently-viable candidates
                // instead of always taking the first match - otherwise every
                // worker converges on the same one peer whenever more than
                // one seeder has the full file, which defeats the point of
                // downloading from multiple peers at once.
                const PeerInfo *chosen = nullptr;
                {
                    lock_guard<mutex> lk(queueMx);
                    vector<const PeerInfo *> candidates;
                    for (auto &p : peers)
                        if (idx < (int)p.bitmap.size() && p.bitmap[idx] && !triedPeers[idx].count(p.userId))
                            candidates.push_back(&p);
                    if (!candidates.empty())
                        chosen = candidates[rrCounter++ % candidates.size()];
                }
                if (!chosen) {
                    lock_guard<mutex> lk(queueMx);
                    stillPending.push_back(idx);
                    continue;
                }
                PeerInfo chosenCopy = *chosen; // fetchPiece + the logging below run without queueMx held
                bool ok = fetchPiece(chosenCopy, rec, idx, myUserId);
                if (ok) {
                    {
                        lock_guard<mutex> lk(rec.mx);
                        rec.haveBitmap[idx] = true;
                    }
                    int doneSoFar = ++status->donePieces;
                    {
                        lock_guard<mutex> lk(status->sourceMx);
                        status->piecesFromPeer[chosenCopy.userId]++;
                        if (idx < (int)status->pieceSource.size()) status->pieceSource[idx] = chosenCopy.userId;
                    }
                    // The whole point: make it visible, live, that different
                    // pieces are coming from different seeders concurrently.
                    logLine("[Download] [" + rec.groupId + "] " + rec.fileName + " piece " +
                            to_string(idx + 1) + "/" + to_string(rec.numPieces) + " <- " + chosenCopy.userId +
                            " (" + chosenCopy.ip + ":" + to_string(chosenCopy.port) + ")  [" +
                            to_string(doneSoFar) + "/" + to_string(status->totalPieces) + " done]");
                } else {
                    lock_guard<mutex> lk(queueMx);
                    triedPeers[idx].insert(chosen->userId);
                    queue.push_back(idx); // retry with a different peer
                }
            }
        });
    }
    for (auto &t : workers) t.join();
    pending = stillPending;
}

// If `rec.localPath` (the "<dest>.part" file) already exists - a previous
// download of the same (group, file) into the same destination was
// interrupted (client killed, crashed, network died) - this checks every
// piece that the file is currently long enough to contain against its
// expected hash, and marks the ones that verify as already "have". This is
// what lets a re-issued download_file pick up where it left off instead of
// re-fetching bytes we already have correctly on disk. It's always safe:
// a piece that doesn't verify (leftover garbage, or genuinely a different
// file that happened to reuse this path) is simply left "missing" and
// fetched normally, exactly like a fresh download - we never trust old
// bytes without re-checking their hash.
inline int resumeFromExistingPart(LocalFileRecord &rec) {
    int fd = open(rec.localPath.c_str(), O_RDONLY);
    if (fd < 0) return 0; // nothing to resume from
    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return 0; }

    int recovered = 0;
    for (int i = 0; i < rec.numPieces; i++) {
        long long offset = (long long)i * PIECE_SIZE;
        long long pieceLen = (i == rec.numPieces - 1 && rec.fileSize % PIECE_SIZE != 0)
                                  ? rec.fileSize % PIECE_SIZE
                                  : PIECE_SIZE;
        if (offset + pieceLen > (long long)st.st_size) break; // file doesn't reach this piece yet

        string buf(pieceLen, '\0');
        size_t got = 0;
        bool ok = true;
        while (got < (size_t)pieceLen) {
            ssize_t n = pread(fd, &buf[0] + got, pieceLen - got, offset + (off_t)got);
            if (n <= 0) { ok = false; break; }
            got += (size_t)n;
        }
        if (ok && SHA1::hash(reinterpret_cast<const uint8_t *>(buf.data()), buf.size()) == rec.pieceHashes[i]) {
            rec.haveBitmap[i] = true;
            recovered++;
        }
    }
    close(fd);
    return recovered;
}

inline void downloadWorker(TrackerSession *session, string groupId, string fileName, string destPath,
                            long long fileSize, int numPieces, string fileHash, vector<string> pieceHashes,
                            vector<PeerInfo> peers, shared_ptr<DownloadStatus> status) {
    string myUserId = session->userId();

    auto rec = make_shared<LocalFileRecord>();
    rec->groupId = groupId; rec->fileName = fileName;
    rec->localPath = destPath + ".part";
    rec->fileSize = fileSize; rec->numPieces = numPieces;
    rec->fileHash = fileHash; rec->pieceHashes = pieceHashes;
    rec->haveBitmap.assign(numPieces, false);

    int recovered = resumeFromExistingPart(*rec);
    if (recovered > 0) {
        status->donePieces += recovered;
        {
            lock_guard<mutex> lk(status->sourceMx);
            status->piecesFromPeer["(resumed)"] += recovered;
            for (int i = 0; i < numPieces; i++)
                if (rec->haveBitmap[i]) status->pieceSource[i] = "(resumed)";
        }
        logLine("[Download] [" + groupId + "] " + fileName + ": resuming a previous attempt - " +
                to_string(recovered) + "/" + to_string(numPieces) + " piece(s) already verified on disk");
    }
    registerLocalFile(rec); // do this only after haveBitmap reflects any resumed pieces, so peer_server.h can serve them immediately

    vector<int> pending;
    for (int i = 0; i < numPieces; i++) if (!rec->haveBitmap[i]) pending.push_back(i);

    runDownloadPass(peers, *rec, status, pending, myUserId);

    if (!pending.empty()) {
        // One refresh of the peer list in case new seeders appeared, then one more pass.
        auto resp = session->send("GET_PEERS " + myUserId + " " + groupId + " " + fileName);
        if (resp.first == "OK") {
            vector<string> lines = split(resp.second, '\n');
            if (lines.size() >= 3) {
                int n = atoi(lines[2].c_str());
                vector<PeerInfo> refreshed;
                for (int i = 0; i < n && (size_t)(3 + i) < lines.size(); i++) {
                    vector<string> f = split(lines[3 + i], ' ');
                    if (f.size() == 4) refreshed.push_back({f[0], f[1], atoi(f[2].c_str()), hexToBitmap(f[3], numPieces)});
                }
                if (!refreshed.empty()) { peers = refreshed; runDownloadPass(peers, *rec, status, pending, myUserId); }
            }
        }
    }

    if (!pending.empty()) {
        status->failed = true;
        status->errorMsg = to_string(pending.size()) + " piece(s) could not be obtained from any peer";
        logLine("[Download] FAILED [" + groupId + "] " + fileName + ": " + status->errorMsg);
        return;
    }

    // Final whole-file integrity check.
    SHA1 whole;
    {
        int fd = open(rec->localPath.c_str(), O_RDONLY);
        if (fd < 0) { status->failed = true; status->errorMsg = "Could not reopen downloaded file for verification"; return; }
        vector<char> buf(PIECE_SIZE);
        ssize_t n;
        while ((n = read(fd, buf.data(), buf.size())) > 0) whole.update(reinterpret_cast<const uint8_t *>(buf.data()), n);
        close(fd);
    }
    if (whole.finalHex() != fileHash) {
        status->failed = true;
        status->errorMsg = "Whole-file hash mismatch after download";
        logLine("[Download] FAILED [" + groupId + "] " + fileName + ": whole-file hash mismatch");
        return;
    }

    if (rename(rec->localPath.c_str(), destPath.c_str()) != 0) {
        status->failed = true;
        status->errorMsg = "Could not move completed download into place";
        return;
    }
    { lock_guard<mutex> lk(rec->mx); rec->localPath = destPath; }

    status->completed = true;
    logLine("[C] [" + groupId + "] " + fileName);

    // Start seeding the now-complete file to other peers.
    string fullBitmap = bitmapToHex(vector<bool>(numPieces, true));
    session->send("UPDATE_SEED " + session->userId() + " " + groupId + " " + fileName + " " + fullBitmap);
}

// Synchronously resolves peers/metadata (so a bad group/file name reports
// an error immediately), then hands off the actual transfer to a
// background thread so the CLI stays responsive.
inline void startDownload(TrackerSession &session, const string &userId, const string &groupId, const string &fileName, const string &destPath) {
    auto resp = session.send("GET_PEERS " + userId + " " + groupId + " " + fileName);
    if (resp.first != "OK") { logLine("ERR " + resp.second); return; }

    vector<string> lines = split(resp.second, '\n');
    if (lines.size() < 3) { logLine("ERR Malformed tracker response"); return; }
    vector<string> meta = split(lines[0], ' ');
    if (meta.size() != 3) { logLine("ERR Malformed tracker response"); return; }
    long long fileSize = atoll(meta[0].c_str());
    int numPieces = atoi(meta[1].c_str());
    string fileHash = meta[2];
    vector<string> pieceHashes = numPieces > 0 ? split(lines[1], ',') : vector<string>{};
    int numPeers = atoi(lines[2].c_str());

    vector<PeerInfo> peers;
    for (int i = 0; i < numPeers && (size_t)(3 + i) < lines.size(); i++) {
        vector<string> f = split(lines[3 + i], ' ');
        if (f.size() == 4) peers.push_back({f[0], f[1], atoi(f[2].c_str()), hexToBitmap(f[3], numPieces)});
    }
    if (peers.empty()) { logLine("ERR No peers currently have this file"); return; }
    if ((int)pieceHashes.size() != numPieces) { logLine("ERR Malformed piece hash list from tracker"); return; }

    auto status = make_shared<DownloadStatus>();
    status->groupId = groupId; status->fileName = fileName; status->totalPieces = numPieces;
    status->pieceSource.assign(numPieces, "");
    { lock_guard<mutex> lk(downloadsMutex()); activeDownloads().push_back(status); }

    logLine("Starting download of [" + groupId + "] " + fileName + " (" + to_string(numPieces) + " pieces, " +
            to_string(peers.size()) + " peer(s) available). Use show_downloads to check progress.");

    thread(downloadWorker, &session, groupId, fileName, destPath, fileSize, numPieces, fileHash, pieceHashes, peers, status).detach();
}

#endif // CLIENT_DOWNLOADER_H
