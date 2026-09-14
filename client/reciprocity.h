#ifndef CLIENT_RECIPROCITY_H
#define CLIENT_RECIPROCITY_H

// Minimal tit-for-tat bookkeeping, in the spirit of BitTorrent's own
// answer to the free-rider problem (see bittorrentecon.pdf): a client
// remembers how many pieces each other peer has actually sent it, and
// peer_server.h uses that to give reciprocating peers extra upload
// capacity once its upload slots are contended (see UPLOAD_SLOTS /
// RECIPROCATOR_BONUS_SLOTS there). A peer that has never sent us anything
// still gets served - just only from the general pool, not the bonus one -
// so a brand new peer isn't permanently locked out (BitTorrent's
// "optimistic unchoke" idea, simplified).
//
// This is deliberately per-process, in-memory, and not part of any
// replicated state: reciprocity is a local, client-to-client trust signal,
// not something the tracker needs to know or arbitrate.

#include <string>
#include <unordered_map>
#include <mutex>

using namespace std;

inline mutex &reciprocityMutex() {
    static mutex m;
    return m;
}
inline unordered_map<string, int> &piecesReceivedFrom() {
    static unordered_map<string, int> m;
    return m;
}

inline void recordPieceReceivedFrom(const string &peerUserId) {
    lock_guard<mutex> lk(reciprocityMutex());
    piecesReceivedFrom()[peerUserId]++;
}

inline bool isReciprocator(const string &peerUserId) {
    lock_guard<mutex> lk(reciprocityMutex());
    auto it = piecesReceivedFrom().find(peerUserId);
    return it != piecesReceivedFrom().end() && it->second > 0;
}

#endif // CLIENT_RECIPROCITY_H
