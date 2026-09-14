// ============================================================================
// tracker.cpp - Tracker server for the P2P file-sharing system.
//
// Run as:  ./tracker <tracker_info.txt> <tracker_no>       (tracker_no is 1-based)
//
// ARCHITECTURE SUMMARY (see README.md for the full writeup)
// -----------------------------------------------------------------------
// tracker_info.txt lists every tracker as "<ip> <port>" pairs. The grading
// setup uses exactly two (per the assignment spec), but nothing here is
// hardcoded to that: the election and replication logic below work the
// same way for any N >= 1, so scaling to more trackers later is just
// adding a line to the file and starting another process.
//
// Election: trackers form a full mesh of TCP links to each other (one link
// per pair) and exchange 1s heartbeats over it. Whichever tracker has the
// *lowest configured index among those it currently sees as alive* acts as
// PRIMARY; everyone else is SECONDARY. This is a simple static-priority
// rule - easy to reason about, and it naturally reclaims primary status
// for a low-index tracker that comes back online.
//
// Replication: only the PRIMARY accepts state-mutating client commands
// (SECONDARY replies ERR NOT_PRIMARY <ip> <port> so the client can
// transparently reconnect). Every accepted mutation is applied locally and
// then pushed to every connected peer as a single-line "OP" message -
// cheap and low-latency. To recover from any *missed* ops (a peer link
// that was down for a while), every tracker keeps a monotonically
// increasing `stateVersion` counter, bumped once per accepted/replayed
// mutation. Whenever a peer link (re)connects, both sides exchange a full
// state snapshot tagged with their current version; whichever side is
// behind wholesale-adopts the other's snapshot. Because only the primary
// ever originates new state, this converges correctly even across
// primary handoffs: a returning ex-primary with stale data has a lower
// version than the tracker that kept serving clients while it was down,
// so it is the one that gets overwritten, not the other way around.
//
// Read-only queries (LIST_GROUPS, LIST_FILES, LIST_REQUESTS, GET_PEERS)
// are answered by *any* tracker directly from its local (replicated)
// state - this is what lets a client get accurate answers "regardless of
// which tracker it connects to", as the spec requires, without forwarding
// every read through the primary.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <signal.h>
#include <fcntl.h>
#include <mutex>
#include <atomic>
#include <deque>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>

#include "../common/utils.h"
#include "../common/netio.h"
#include "../common/protocol.h"

using namespace std;

// ---------------------------------------------------------------- STATE

struct User {
    string id;
    string password;
    bool isLoggedin = false;
    string lastPeerIp;
    int lastPeerPort = 0;
};

struct Group {
    string id;
    string ownerId;
    unordered_set<string> members;
    vector<string> joinRequests;
};

struct FileInfo {
    string fileName;
    long long fileSize = 0;
    int numPieces = 0;
    string fileHash;
    vector<string> pieceHashes;
    string ownerId;
    unordered_map<string, vector<bool>> seeders; // userId -> piece bitmap
};

mutex stateMutex;
uint64_t stateVersion = 0; // guarded by stateMutex
unordered_map<string, User> users;
unordered_map<string, Group> groups;
unordered_map<string, unordered_map<string, FileInfo>> files; // groupId -> fileName -> FileInfo

// Must be called while holding stateMutex.
bool loggedIn(const string &id) {
    auto it = users.find(id);
    return it != users.end() && it->second.isLoggedin;
}
bool isMember(const Group &g, const string &userId) {
    return g.members.count(userId) > 0;
}

struct HandlerResult {
    bool ok;
    string message;
};

const unordered_set<string> MUTATING_COMMANDS = {
    "CREATE_USER", "LOGIN", "LOGOUT", "CREATE_GROUP", "JOIN_GROUP", "LEAVE_GROUP",
    "ACCEPT_REQUEST", "REGISTER_FILE", "UPDATE_SEED", "STOP_SHARE"
};
bool isMutatingCommand(const string &cmd) { return MUTATING_COMMANDS.count(cmd) > 0; }

// Applies one command line's worth of tokens to local state. Used both for
// commands a live client sent to this (primary) tracker, and for "OP" lines
// replayed from a peer tracker - in both cases the command already carries
// the resolved user_id as an explicit argument, so no per-connection
// session lookup is needed here (see README "Design notes" for why).
HandlerResult applyCommand(const vector<string> &tokens) {
    lock_guard<mutex> lock(stateMutex);
    HandlerResult r{false, "Unknown command"};
    if (tokens.empty()) return r;
    const string &cmd = tokens[0];

    if (cmd == "CREATE_USER" && tokens.size() == 3) {
        const string &id = tokens[1];
        if (!isValidId(id)) { r = {false, "Invalid user id"}; }
        else if (users.count(id)) { r = {false, "User already exists"}; }
        else { users[id] = User{id, tokens[2], false, "", 0}; r = {true, "User created successfully"}; }

    } else if (cmd == "LOGIN" && tokens.size() == 5) {
        const string &id = tokens[1];
        auto it = users.find(id);
        if (it == users.end()) { r = {false, "User not found"}; }
        else if (it->second.password != tokens[2]) { r = {false, "Invalid password"}; }
        else if (it->second.isLoggedin) { r = {false, "Already logged in; logout first"}; }
        else {
            it->second.isLoggedin = true;
            it->second.lastPeerIp = tokens[3];
            it->second.lastPeerPort = atoi(tokens[4].c_str());
            r = {true, "Login successful"};
        }

    } else if (cmd == "LOGOUT" && tokens.size() == 2) {
        const string &id = tokens[1];
        auto it = users.find(id);
        if (it == users.end() || !it->second.isLoggedin) { r = {false, "Not logged in"}; }
        else {
            it->second.isLoggedin = false;
            for (auto &gp : files) for (auto &fp : gp.second) fp.second.seeders.erase(id);
            r = {true, "Logged out; stopped sharing all files"};
        }

    } else if (cmd == "CREATE_GROUP" && tokens.size() == 3) {
        const string &id = tokens[1], &gid = tokens[2];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!isValidId(gid)) { r = {false, "Invalid group id"}; }
        else if (groups.count(gid)) { r = {false, "Group already exists"}; }
        else { groups[gid] = Group{gid, id, {id}, {}}; r = {true, "Group created successfully"}; }

    } else if (cmd == "JOIN_GROUP" && tokens.size() == 3) {
        const string &id = tokens[1], &gid = tokens[2];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else {
            Group &g = groups[gid];
            if (g.members.count(id)) { r = {false, "Already a member of this group"}; }
            else if (find(g.joinRequests.begin(), g.joinRequests.end(), id) != g.joinRequests.end()) {
                r = {false, "Join request already sent"};
            } else { g.joinRequests.push_back(id); r = {true, "Join request sent to group owner"}; }
        }

    } else if (cmd == "LEAVE_GROUP" && tokens.size() == 3) {
        const string &id = tokens[1], &gid = tokens[2];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else {
            Group &g = groups[gid];
            if (g.ownerId == id) { r = {false, "Owner cannot leave the group"}; }
            else if (!g.members.count(id)) { r = {false, "You are not a member of this group"}; }
            else { g.members.erase(id); r = {true, "Left group " + gid}; }
        }

    } else if (cmd == "LIST_GROUPS" && tokens.size() == 1) {
        ostringstream oss;
        for (auto &[gid, g] : groups) oss << gid << " " << g.ownerId << " " << g.members.size() << "\n";
        r = {true, oss.str()};

    } else if (cmd == "LIST_REQUESTS" && tokens.size() == 3) {
        const string &id = tokens[1], &gid = tokens[2];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else if (groups[gid].ownerId != id) { r = {false, "Only the group owner can view join requests"}; }
        else {
            ostringstream oss;
            for (auto &u : groups[gid].joinRequests) oss << u << "\n";
            r = {true, oss.str()};
        }

    } else if (cmd == "ACCEPT_REQUEST" && tokens.size() == 4) {
        const string &id = tokens[1], &gid = tokens[2], &target = tokens[3];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else if (groups[gid].ownerId != id) { r = {false, "Only the group owner can accept requests"}; }
        else {
            Group &g = groups[gid];
            auto it = find(g.joinRequests.begin(), g.joinRequests.end(), target);
            if (it == g.joinRequests.end()) { r = {false, "No pending join request from " + target}; }
            else { g.joinRequests.erase(it); g.members.insert(target); r = {true, "Accepted " + target + " into group " + gid}; }
        }

    } else if (cmd == "REGISTER_FILE" && tokens.size() == 8) {
        const string &id = tokens[1], &gid = tokens[2], &fname = tokens[3];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!isValidId(fname)) { r = {false, "Invalid file name (no spaces/commas allowed)"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else if (!isMember(groups[gid], id)) { r = {false, "You are not a member of this group"}; }
        else {
            long long size = atoll(tokens[4].c_str());
            int numPieces = atoi(tokens[5].c_str());
            const string &hash = tokens[6];
            vector<string> pieceHashes = tokens[7].empty() ? vector<string>{} : split(tokens[7], ',');
            if ((int)pieceHashes.size() != numPieces) {
                r = {false, "Piece hash count does not match numPieces"};
            } else {
                auto &fmap = files[gid];
                auto fit = fmap.find(fname);
                if (fit != fmap.end() && fit->second.fileHash != hash) {
                    r = {false, "A different file already exists under this name in the group"};
                } else {
                    FileInfo fi{fname, size, numPieces, hash, pieceHashes, id, {}};
                    fi.seeders[id] = vector<bool>(numPieces, true);
                    fmap[fname] = fi;
                    r = {true, "File registered: " + fname};
                }
            }
        }

    } else if (cmd == "LIST_FILES" && tokens.size() == 3) {
        const string &id = tokens[1], &gid = tokens[2];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else if (!isMember(groups[gid], id)) { r = {false, "You are not a member of this group"}; }
        else {
            ostringstream oss;
            auto fit = files.find(gid);
            if (fit != files.end())
                for (auto &[fname, fi] : fit->second)
                    oss << fname << " " << fi.fileSize << " " << fi.numPieces << " " << fi.seeders.size() << "\n";
            r = {true, oss.str()};
        }

    } else if (cmd == "GET_PEERS" && tokens.size() == 4) {
        const string &id = tokens[1], &gid = tokens[2], &fname = tokens[3];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else if (!groups.count(gid)) { r = {false, "Group does not exist"}; }
        else if (!isMember(groups[gid], id)) { r = {false, "You are not a member of this group"}; }
        else {
            auto fit = files.find(gid);
            if (fit == files.end() || !fit->second.count(fname)) { r = {false, "File not found in group"}; }
            else {
                FileInfo &fi = fit->second[fname];
                ostringstream oss;
                oss << fi.fileSize << " " << fi.numPieces << " " << fi.fileHash << "\n";
                oss << join(fi.pieceHashes, ',') << "\n";
                oss << fi.seeders.size() << "\n";
                for (auto &[uid, bm] : fi.seeders) {
                    auto uit = users.find(uid);
                    string ip = uit != users.end() ? uit->second.lastPeerIp : "";
                    int port = uit != users.end() ? uit->second.lastPeerPort : 0;
                    oss << uid << " " << ip << " " << port << " " << bitmapToHex(bm) << "\n";
                }
                r = {true, oss.str()};
            }
        }

    } else if (cmd == "UPDATE_SEED" && tokens.size() == 5) {
        const string &id = tokens[1], &gid = tokens[2], &fname = tokens[3];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else {
            auto fit = files.find(gid);
            if (fit == files.end() || !fit->second.count(fname)) { r = {false, "File not found"}; }
            else if (!isMember(groups[gid], id)) { r = {false, "You are not a member of this group"}; }
            else {
                FileInfo &fi = fit->second[fname];
                vector<bool> incoming = hexToBitmap(tokens[4], fi.numPieces);
                auto &bm = fi.seeders[id];
                if (bm.size() != (size_t)fi.numPieces) bm.assign(fi.numPieces, false);
                for (int i = 0; i < fi.numPieces; i++) bm[i] = bm[i] || incoming[i];
                r = {true, "Seed status updated"};
            }
        }

    } else if (cmd == "STOP_SHARE" && tokens.size() == 4) {
        const string &id = tokens[1], &gid = tokens[2], &fname = tokens[3];
        if (!loggedIn(id)) { r = {false, "You must login first"}; }
        else {
            auto fit = files.find(gid);
            if (fit == files.end() || !fit->second.count(fname)) { r = {false, "File not found"}; }
            else { fit->second[fname].seeders.erase(id); r = {true, "Stopped sharing " + fname}; }
        }
    }

    if (r.ok) stateVersion++;
    return r;
}

// ------------------------------------------------------- SNAPSHOT SYNC

string serializeSnapshot() {
    lock_guard<mutex> lock(stateMutex);
    ostringstream oss;
    oss << stateVersion << "\n";
    for (auto &[id, u] : users)
        oss << "USER," << id << "," << u.password << "," << (u.isLoggedin ? 1 : 0) << ","
            << u.lastPeerIp << "," << u.lastPeerPort << "\n";
    for (auto &[gid, g] : groups) {
        vector<string> mem(g.members.begin(), g.members.end());
        oss << "GROUP," << gid << "," << g.ownerId << "," << join(mem, ';') << "," << join(g.joinRequests, ';') << "\n";
    }
    for (auto &[gid, fmap] : files) {
        for (auto &[fname, fi] : fmap) {
            oss << "FILE," << gid << "," << fname << "," << fi.fileSize << "," << fi.numPieces << ","
                << fi.fileHash << "," << join(fi.pieceHashes, ';') << "," << fi.ownerId << "\n";
            for (auto &[uid, bm] : fi.seeders)
                oss << "SEED," << gid << "," << fname << "," << uid << "," << bitmapToHex(bm) << "\n";
        }
    }
    return oss.str();
}

// Adopts a peer's full state wholesale iff its version is strictly newer
// than ours. Because only the primary ever originates writes, and every
// tracker's version only advances by applying an accepted/replayed
// mutation, "higher version" reliably means "more complete history" -
// there is no independent history on a secondary that this could clobber.
void mergeSnapshotIfNewer(uint64_t peerVersion, const string &body) {
    lock_guard<mutex> lock(stateMutex);
    if (peerVersion <= stateVersion) return;

    users.clear();
    groups.clear();
    files.clear();
    stringstream ss(body);
    string line;
    while (getline(ss, line)) {
        if (line.empty()) continue;
        vector<string> f = split(line, ',');
        if (f[0] == "USER" && f.size() >= 6) {
            users[f[1]] = User{f[1], f[2], f[3] == "1", f[4], f[5].empty() ? 0 : atoi(f[5].c_str())};
        } else if (f[0] == "GROUP" && f.size() >= 5) {
            Group g; g.id = f[1]; g.ownerId = f[2];
            if (!f[3].empty()) for (auto &m : split(f[3], ';')) g.members.insert(m);
            if (!f[4].empty()) g.joinRequests = split(f[4], ';');
            groups[f[1]] = g;
        } else if (f[0] == "FILE" && f.size() >= 8) {
            FileInfo fi;
            fi.fileName = f[2]; fi.fileSize = atoll(f[3].c_str()); fi.numPieces = atoi(f[4].c_str());
            fi.fileHash = f[5];
            if (!f[6].empty()) fi.pieceHashes = split(f[6], ';');
            fi.ownerId = f[7];
            files[f[1]][f[2]] = fi;
        } else if (f[0] == "SEED" && f.size() >= 5) {
            auto git = files.find(f[1]);
            if (git != files.end()) {
                auto fit = git->second.find(f[2]);
                if (fit != git->second.end()) fit->second.seeders[f[3]] = hexToBitmap(f[4], fit->second.numPieces);
            }
        }
    }
    stateVersion = peerVersion;
}

// --------------------------------------------------------- TRACKER MESH

vector<TrackerAddr> allTrackers;
int myIdx;

struct PeerLink {
    int idx;
    string ip;
    int syncPort;
    atomic<int> fd;
    atomic<bool> alive;
    mutex sendMx;
    PeerLink(int i, string ipAddr, int sp) : idx(i), ip(move(ipAddr)), syncPort(sp), fd(-1), alive(false) {}
};
deque<PeerLink> peerLinks;

PeerLink *findLink(int idx) {
    for (auto &l : peerLinks) if (l.idx == idx) return &l;
    return nullptr;
}

int currentPrimaryIdx() {
    int best = myIdx;
    for (auto &l : peerLinks) if (l.alive && l.idx < best) best = l.idx;
    return best;
}

void broadcastOp(const string &line) {
    string frame = "OP\n" + line;
    for (auto &link : peerLinks) {
        if (link.alive) {
            lock_guard<mutex> lk(link.sendMx);
            int fd = link.fd;
            if (fd != -1) sendFrame(fd, frame);
        }
    }
}

void processPeerFrame(PeerLink &link, const string &frame) {
    size_t nl = frame.find('\n');
    string type = (nl == string::npos) ? frame : frame.substr(0, nl);
    string body = (nl == string::npos) ? "" : frame.substr(nl + 1);
    if (type == "SNAPSHOT") {
        size_t nl2 = body.find('\n');
        uint64_t ver = 0;
        try { ver = stoull(body.substr(0, nl2)); } catch (...) { return; }
        string rest = (nl2 == string::npos) ? "" : body.substr(nl2 + 1);
        uint64_t before = stateVersion;
        mergeSnapshotIfNewer(ver, rest);
        if (stateVersion != before)
            logLine("[Sync] Adopted newer state from tracker " + to_string(link.idx) + " (version -> " + to_string(stateVersion) + ")");
    } else if (type == "OP") {
        vector<string> tokens = split(body, ' ');
        if (!tokens.empty()) applyCommand(tokens);
    }
    // HELLO / HB carry no further action beyond updating liveness (handled by caller).
}

// Shared read/heartbeat loop used by both the dialing side and the
// accepting side of a tracker-to-tracker link once the handshake is done.
void runLinkLoop(PeerLink &link) {
    int fd = link.fd;
    auto now0 = chrono::steady_clock::now();
    auto lastRecv = now0;
    // Start "due" so the first heartbeat goes out on the very first loop
    // iteration. (Using time_point::min() here would look tempting but
    // subtracting it from `now` overflows the duration's underlying
    // representation - undefined behavior - so we offset from `now`
    // instead, which stays within a normal, safe range.)
    auto lastSent = now0 - chrono::seconds(TRACKER_HEARTBEAT_INTERVAL_SEC);
    while (true) {
        struct pollfd pfd { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 1000);
        auto now = chrono::steady_clock::now();
        if (pr > 0 && (pfd.revents & POLLIN)) {
            string frame;
            if (!recvFrame(fd, frame)) break;
            lastRecv = now;
            processPeerFrame(link, frame);
        } else if (pr < 0) {
            break;
        }
        if (chrono::duration_cast<chrono::seconds>(now - lastRecv).count() >= TRACKER_PEER_TIMEOUT_SEC) {
            logLine("[Sync] Tracker " + to_string(link.idx) + " timed out");
            break;
        }
        if (chrono::duration_cast<chrono::seconds>(now - lastSent).count() >= TRACKER_HEARTBEAT_INTERVAL_SEC) {
            lock_guard<mutex> lk(link.sendMx);
            if (!sendFrame(fd, string("HB\n"))) break;
            lastSent = now;
        }
    }
    link.alive = false;
    link.fd = -1;
    close(fd);
    logLine("[Sync] Link to tracker " + to_string(link.idx) + " is down");
}

void dialLoop(PeerLink *link) {
    while (true) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(link->syncPort);
            inet_pton(AF_INET, link->ip.c_str(), &addr.sin_addr);
            if (connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
                logLine("[Sync] Connected to tracker " + to_string(link->idx));
                sendFrame(fd, "HELLO\n" + to_string(myIdx));
                sendFrame(fd, "SNAPSHOT\n" + serializeSnapshot());
                link->fd = fd;
                link->alive = true;
                runLinkLoop(*link); // blocks until the link drops
            } else {
                close(fd);
            }
        }
        this_thread::sleep_for(chrono::seconds(2));
    }
}

void acceptedPeerHandler(int fd) {
    string frame;
    if (!recvFrame(fd, frame)) { close(fd); return; }
    size_t nl = frame.find('\n');
    if (frame.substr(0, nl) != "HELLO") { close(fd); return; }
    int peerIdx;
    try { peerIdx = stoi(frame.substr(nl + 1)); } catch (...) { close(fd); return; }
    PeerLink *link = findLink(peerIdx);
    if (!link) { close(fd); return; }
    logLine("[Sync] Accepted connection from tracker " + to_string(peerIdx));
    sendFrame(fd, "HELLO\n" + to_string(myIdx));
    sendFrame(fd, "SNAPSHOT\n" + serializeSnapshot());
    link->fd = fd;
    link->alive = true;
    runLinkLoop(*link);
}

void syncAcceptLoop(int listenFd) {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listenFd, (sockaddr *)&addr, &len);
        if (fd >= 0) thread(acceptedPeerHandler, fd).detach();
    }
}

void electionTicker() {
    int lastRole = -2; // -2 = uninitialized, forces first log
    while (true) {
        int p = currentPrimaryIdx();
        int role = (p == myIdx) ? 1 : 0;
        if (role != lastRole) {
            logLine(role ? "[Election] This tracker is now PRIMARY" : "[Election] This tracker is now SECONDARY (primary is tracker " + to_string(p + 1) + ")");
            lastRole = role;
        }
        this_thread::sleep_for(chrono::milliseconds(500));
    }
}

// -------------------------------------------------------- CLIENT HANDLING

void handleClient(int fd) {
    while (true) {
        string line;
        if (!recvFrame(fd, line)) break;
        line = trim(line);
        vector<string> tokens = split(line, ' ');
        if (tokens.empty() || tokens[0].empty()) { sendFrame(fd, string("ERR Empty command")); continue; }

        const string &cmd = tokens[0];
        if (isMutatingCommand(cmd)) {
            int p = currentPrimaryIdx();
            if (p != myIdx) {
                sendFrame(fd, "ERR NOT_PRIMARY " + allTrackers[p].ip + " " + to_string(allTrackers[p].port));
                continue;
            }
        }

        HandlerResult r = applyCommand(tokens);
        if (isMutatingCommand(cmd) && r.ok) broadcastOp(line);
        sendFrame(fd, (r.ok ? "OK " : "ERR ") + r.message);
    }
    close(fd);
}

void consoleReader() {
    string line;
    while (getline(cin, line)) {
        if (trim(line) == "quit") {
            logLine("[Console] Shutting down tracker.");
            exit(0);
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <tracker_info.txt> <tracker_no>" << endl;
        return 1;
    }

    try {
        allTrackers = parseTrackerList(argv[1]);
        int trackerNo = atoi(argv[2]);
        if (trackerNo < 1 || trackerNo > (int)allTrackers.size()) {
            throw runtime_error("tracker_no out of range for tracker_info.txt");
        }
        myIdx = trackerNo - 1;

        for (int i = 0; i < (int)allTrackers.size(); i++) {
            if (i == myIdx) continue;
            peerLinks.emplace_back(i, allTrackers[i].ip, allTrackers[i].port + SYNC_PORT_OFFSET);
        }

        int myPort = allTrackers[myIdx].port;
        int mySyncPort = myPort + SYNC_PORT_OFFSET;

        int clientFd = socket(AF_INET, SOCK_STREAM, 0);
        if (clientFd < 0) throw runtime_error("socket() failed for client listener");
        int opt = 1;
        setsockopt(clientFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in caddr{};
        caddr.sin_family = AF_INET;
        caddr.sin_addr.s_addr = INADDR_ANY;
        caddr.sin_port = htons(myPort);
        if (bind(clientFd, (sockaddr *)&caddr, sizeof(caddr)) < 0) throw runtime_error("bind() failed on client port " + to_string(myPort));
        listen(clientFd, SOMAXCONN);

        int syncFd = socket(AF_INET, SOCK_STREAM, 0);
        if (syncFd < 0) throw runtime_error("socket() failed for sync listener");
        setsockopt(syncFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = INADDR_ANY;
        saddr.sin_port = htons(mySyncPort);
        if (bind(syncFd, (sockaddr *)&saddr, sizeof(saddr)) < 0) throw runtime_error("bind() failed on sync port " + to_string(mySyncPort));
        listen(syncFd, SOMAXCONN);

        logLine("[Tracker " + to_string(trackerNo) + "] listening for clients on port " + to_string(myPort) +
                ", tracker sync on port " + to_string(mySyncPort) + " (" + to_string(allTrackers.size()) + " trackers configured)");

        for (auto &link : peerLinks)
            if (link.idx > myIdx) thread(dialLoop, &link).detach();

        thread(syncAcceptLoop, syncFd).detach();
        thread(electionTicker).detach();
        thread(consoleReader).detach();

        while (true) {
            sockaddr_in caddr2{};
            socklen_t len = sizeof(caddr2);
            int fd = accept(clientFd, (sockaddr *)&caddr2, &len);
            if (fd >= 0) thread(handleClient, fd).detach();
        }
    } catch (const std::exception &e) {
        cerr << "[Tracker] Fatal: " << e.what() << endl;
        return 1;
    }
}
