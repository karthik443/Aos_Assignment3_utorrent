#ifndef CLIENT_PEER_SERVER_H
#define CLIENT_PEER_SERVER_H

// Every client is also a seeder: this listens on the peer_port the user
// gave on the command line and serves piece requests from other clients.
// Protocol (client<->client, both directions use the common frame format):
//   request:  "GET_PIECE <requester_user_id> <group_id> <file_name> <piece_index>"
//   response: "OK <piece_index> <length> <sha1hex>" followed by a second
//             frame containing exactly <length> raw bytes, or a single
//             frame "ERR <reason>" if the piece isn't available or all
//             upload slots are currently taken (see admission control
//             below) - the requester's download manager just treats that
//             like any other failed attempt and tries a different peer.
//
// Admission control (tit-for-tat, see reciprocity.h): GENERAL_UPLOAD_SLOTS
// concurrent transfers are open to anyone; once full, RECIPROCATOR_BONUS_SLOTS
// more are available but only to requesters who have themselves sent this
// client at least one piece before. This is deliberately simple - no
// queueing, no preempting an in-flight transfer - admission is decided once
// per request with a couple of atomic counters, so it can't deadlock or
// leave a slot stuck if a thread dies unexpectedly (the slot is released in
// the same function that acquired it, on every exit path).

#include <string>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "../common/netio.h"
#include "../common/utils.h"
#include "../common/protocol.h"
#include "fileops.h"
#include "reciprocity.h"

using namespace std;

inline atomic<int> &generalSlotsInUse() { static atomic<int> n{0}; return n; }
inline atomic<int> &bonusSlotsInUse() { static atomic<int> n{0}; return n; }

// Tries to reserve one upload slot for `requesterId`. Returns {admitted,
// usedBonusSlot} - the caller must call releaseUploadSlot(usedBonusSlot)
// exactly once, iff admitted, once it's done sending (or has failed out).
inline pair<bool, bool> acquireUploadSlot(const string &requesterId) {
    if (generalSlotsInUse().fetch_add(1) < GENERAL_UPLOAD_SLOTS) {
        return {true, false};
    }
    generalSlotsInUse()--; // didn't actually get a general slot; give it back

    if (isReciprocator(requesterId) && bonusSlotsInUse().fetch_add(1) < RECIPROCATOR_BONUS_SLOTS) {
        return {true, true};
    }
    if (isReciprocator(requesterId)) bonusSlotsInUse()--; // reserved-but-full case, give it back

    return {false, false};
}
inline void releaseUploadSlot(bool usedBonusSlot) {
    if (usedBonusSlot) bonusSlotsInUse()--;
    else generalSlotsInUse()--;
}

inline void servePeerConnection(int fd) {
    string req;
    if (recvFrame(fd, req)) {
        vector<string> tok = split(trim(req), ' ');
        if (tok.size() == 5 && tok[0] == "GET_PIECE") {
            const string &requesterId = tok[1];
            auto rec = findLocalFile(tok[2], tok[3]);
            int idx = atoi(tok[4].c_str());
            bool available = false;
            long long pieceLen = 0;
            string path;
            string expectedHash;
            if (rec) {
                lock_guard<mutex> lk(rec->mx);
                if (!rec->sharingStopped && idx >= 0 && idx < rec->numPieces &&
                    idx < (int)rec->haveBitmap.size() && rec->haveBitmap[idx]) {
                    available = true;
                    path = rec->localPath;
                    pieceLen = (idx == rec->numPieces - 1 && rec->fileSize % PIECE_SIZE != 0)
                                   ? rec->fileSize % PIECE_SIZE
                                   : PIECE_SIZE;
                    expectedHash = rec->pieceHashes[idx];
                }
            }
            if (!available) {
                sendFrame(fd, string("ERR Piece not available"));
            } else {
                auto [admitted, usedBonus] = acquireUploadSlot(requesterId);
                if (!admitted) {
                    sendFrame(fd, string("ERR BUSY upload slots full, try another peer or retry shortly"));
                } else {
                    string data;
                    if (readPieceFromDisk(path, idx, pieceLen, data)) {
                        sendFrame(fd, "OK " + to_string(idx) + " " + to_string(data.size()) + " " + expectedHash);
                        sendFrame(fd, data);
                    } else {
                        sendFrame(fd, string("ERR Local read failure"));
                    }
                    releaseUploadSlot(usedBonus);
                }
            }
        } else {
            sendFrame(fd, string("ERR Bad request"));
        }
    }
    close(fd);
}

inline void peerServerAcceptLoop(int listenFd) {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listenFd, (sockaddr *)&addr, &len);
        if (fd >= 0) thread(servePeerConnection, fd).detach();
    }
}

// Binds and starts the peer server; returns the bound port (for logging).
inline int startPeerServer(const string &bindIp, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw runtime_error("socket() failed for peer server");
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) throw runtime_error("bind() failed for peer server on port " + to_string(port));
    listen(fd, SOMAXCONN);
    thread(peerServerAcceptLoop, fd).detach();
    return port;
}

#endif // CLIENT_PEER_SERVER_H
