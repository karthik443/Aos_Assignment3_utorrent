#ifndef COMMON_PROTOCOL_H
#define COMMON_PROTOCOL_H

// Shared constants for the file-sharing protocol. See README.md for the
// full wire-format writeup; this header just centralizes the numbers so
// tracker and client can't drift apart.

#include <cstdint>

// Files are split into fixed-size pieces; the last piece is whatever is
// left over. Both piece-level and whole-file SHA1 hashes are computed over
// this chunking.
constexpr long long PIECE_SIZE = 512LL * 1024;

// Soft cap mentioned in the spec ("up to 1GB"); not strictly enforced, used
// only to size a couple of sanity checks.
constexpr long long MAX_FILE_SIZE = 1024LL * 1024 * 1024;

// Sync-port convention: every tracker also listens for tracker<->tracker
// traffic on (control_port + SYNC_PORT_OFFSET). Keeping this a fixed offset
// means tracker_info.txt only needs to list the client-facing ports.
constexpr int SYNC_PORT_OFFSET = 100;

// How often a tracker pings each connected peer tracker, and how long a
// peer link may go silent before being declared dead and re-dialed.
constexpr int TRACKER_HEARTBEAT_INTERVAL_SEC = 1;
constexpr int TRACKER_PEER_TIMEOUT_SEC = 4;

// Special clientSock value used internally to mean "this command is being
// applied because a peer tracker told us to, not because a live client
// asked" - it skips session/auth bookkeeping that only makes sense for a
// real client TCP connection.
constexpr int REPLAY_SOCK = -1;

// Tit-for-tat upload admission (client/reciprocity.h, client/peer_server.h):
// GENERAL_UPLOAD_SLOTS concurrent piece transfers are open to any
// requester; once those are full, an additional RECIPROCATOR_BONUS_SLOTS
// are available but only to peers who have themselves sent us at least one
// piece. A peer we've never gotten anything from can still always compete
// for a general slot - it's just not eligible for the bonus pool - so
// reciprocators get strictly more total capacity without anyone being
// permanently locked out.
constexpr int GENERAL_UPLOAD_SLOTS = 3;
constexpr int RECIPROCATOR_BONUS_SLOTS = 2;

#endif // COMMON_PROTOCOL_H
