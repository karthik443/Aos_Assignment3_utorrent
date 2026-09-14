#ifndef COMMON_NETIO_H
#define COMMON_NETIO_H

// Every socket in this project (tracker<->client, tracker<->tracker,
// client<->client) speaks the same simple framing:
//
//      [4-byte big-endian length N][N bytes of payload]
//
// A frame's payload is opaque bytes - the same primitive carries a text
// command line (control protocol) or a raw 512KB file piece (data
// transfer), the caller just knows which one to expect. Because every send
// and receive loops until the full frame is written/read (or the socket
// fails), partial reads/writes and short writes - which TCP can produce at
// any time - are handled transparently for every caller in the codebase
// instead of being re-solved ad hoc at each call site.

#include <string>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

// A generous cap on a single frame (a bit above one 512KB piece plus
// headroom for metadata frames like piece-hash lists). Guards against a
// corrupted/garbage length prefix causing an enormous allocation.
inline const uint32_t MAX_FRAME_SIZE = 16u * 1024 * 1024;

inline bool sendAll(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

inline bool recvAll(int fd, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n == 0) return false; // peer closed cleanly
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

inline bool sendFrame(int fd, const char *data, uint32_t len) {
    uint32_t netLen = htonl(len);
    if (!sendAll(fd, reinterpret_cast<const char *>(&netLen), 4)) return false;
    if (len == 0) return true;
    return sendAll(fd, data, len);
}

inline bool sendFrame(int fd, const std::string &s) {
    return sendFrame(fd, s.data(), (uint32_t)s.size());
}

// Reads one frame. Returns false on connection close/error (out is left in
// an unspecified state); true with `out` holding exactly the payload bytes.
inline bool recvFrame(int fd, std::string &out) {
    uint32_t netLen;
    if (!recvAll(fd, reinterpret_cast<char *>(&netLen), 4)) return false;
    uint32_t len = ntohl(netLen);
    if (len > MAX_FRAME_SIZE) return false; // guard against a bogus length
    out.resize(len);
    if (len == 0) return true;
    return recvAll(fd, &out[0], len);
}

#endif // COMMON_NETIO_H
