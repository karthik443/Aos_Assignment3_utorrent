#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

// Small shared helpers used by both tracker and client: tokenizing,
// tracker_info.txt parsing, bitmap<->hex conversion (used to advertise
// which pieces of a file a peer has), and a mutex-guarded logger so
// multi-threaded console output doesn't interleave mid-line.

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <cctype>

using namespace std;

// ---------------------------------------------------------------- logging
inline mutex &logMutex() {
    static mutex m;
    return m;
}
inline void logLine(const string &s) {
    lock_guard<mutex> lock(logMutex());
    cout << s << endl;
}

// ------------------------------------------------------------- tokenizing
inline vector<string> split(const string &input, char delim) {
    vector<string> tokens;
    string token;
    stringstream ss(input);
    while (getline(ss, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline string join(const vector<string> &parts, char delim) {
    string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += delim;
        out += parts[i];
    }
    return out;
}

inline string trim(const string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// ----------------------------------------------------- tracker_info.txt
// Format: whitespace-separated "<ip> <port>" pairs, one tracker per pair,
// any number of trackers (grading uses exactly two, per the assignment
// spec; the parser itself does not hardcode a count so the same binaries
// scale to more trackers just by adding lines and starting more
// processes - no code changes needed).
struct TrackerAddr {
    string ip;
    int port;
};

inline vector<TrackerAddr> parseTrackerList(const string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw runtime_error("Unable to open tracker info file: " + path);
    }
    string content;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        content.append(buf, n);
    }
    close(fd);

    vector<TrackerAddr> result;
    stringstream ss(content);
    string ip, portStr;
    while (ss >> ip >> portStr) {
        result.push_back({ip, stoi(portStr)});
    }
    if (result.empty()) {
        throw runtime_error("No trackers found in " + path);
    }
    return result;
}

// --------------------------------------------------------- bitmap<->hex
// Piece-availability bitmaps travel over the wire as hex strings (4 bits =
// 1 hex digit, most-significant bit first within each nibble, piece 0 is
// the first bit of the first byte).
inline string bitmapToHex(const vector<bool> &bm) {
    string hex;
    hex.reserve((bm.size() + 3) / 4);
    for (size_t i = 0; i < bm.size(); i += 4) {
        int nibble = 0;
        for (int j = 0; j < 4; j++) {
            nibble <<= 1;
            if (i + j < bm.size() && bm[i + j]) nibble |= 1;
        }
        char c = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
        hex.push_back(c);
    }
    return hex;
}

inline vector<bool> hexToBitmap(const string &hex, size_t numPieces) {
    vector<bool> bm(numPieces, false);
    size_t idx = 0;
    for (char c : hex) {
        int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else continue;
        for (int j = 3; j >= 0 && idx < numPieces; j--) {
            bm[idx++] = (nibble >> j) & 1;
        }
    }
    return bm;
}

inline bool isValidId(const string &s) {
    // user_id / group_id / file_name must not contain whitespace, commas or
    // control characters - they are used as tokens/CSV fields throughout
    // the wire protocol and the tracker's state-snapshot format. This is a
    // documented simplification (see README "Assumptions").
    if (s.empty()) return false;
    for (char c : s) {
        if (isspace((unsigned char)c) || c == ',' || (unsigned char)c < 0x20) return false;
    }
    return true;
}

#endif // COMMON_UTILS_H
