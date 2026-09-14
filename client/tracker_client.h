#ifndef CLIENT_TRACKER_CLIENT_H
#define CLIENT_TRACKER_CLIENT_H

// Wraps the client's single connection to "whichever tracker we currently
// talk to". Handles the two kinds of failure a tracker can hand back:
//   - a dead/reset socket (that tracker process is down)              -> try
//     every tracker in tracker_info.txt until one accepts a connection.
//   - "ERR NOT_PRIMARY <ip> <port>" (we're talking to a live SECONDARY,
//     which only accepts writes on the current PRIMARY)               -> the
//     secondary tells us exactly which tracker to use instead, so we just
//     reconnect there.
// Either way, once a *different* tracker is now in use, we transparently
// re-send LOGIN with the cached credentials before retrying the caller's
// original command - this is what makes "if a tracker goes down the client
// keeps working against the other one" require no user action.

#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "../common/utils.h"
#include "../common/netio.h"

using namespace std;

class TrackerSession {
public:
    void init(vector<TrackerAddr> t) { trackers = move(t); }

    // Sends one command line, returns {status, body} where status is "OK"
    // or "ERR" (the NOT_PRIMARY/redirect case is fully handled internally
    // and never surfaces to the caller).
    pair<string, string> send(const string &line) {
        lock_guard<mutex> lk(mx);
        return sendLocked(line, 0);
    }

    void setCredentials(const string &uid, const string &pw, const string &peerIp, int peerPort) {
        lock_guard<mutex> lk(credMx);
        myUserId = uid; myPassword = pw; myPeerIp = peerIp; myPeerPort = peerPort;
    }
    void setLoggedIn(bool v) { lock_guard<mutex> lk(credMx); loggedIn = v; }
    bool isLoggedIn() { lock_guard<mutex> lk(credMx); return loggedIn; }
    string userId() { lock_guard<mutex> lk(credMx); return myUserId; }

private:
    vector<TrackerAddr> trackers;
    int fd = -1;
    mutex mx;     // guards fd and the send/recv sequence below
    mutex credMx; // guards cached login credentials

    string myUserId, myPassword, myPeerIp;
    int myPeerPort = 0;
    bool loggedIn = false;

    bool connectTo(const string &ip, int port) {
        if (fd != -1) { close(fd); fd = -1; }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        hostent *he = gethostbyname(ip.c_str());
        if (!he) { close(s); return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
        if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) { close(s); return false; }
        fd = s;
        return true;
    }

    bool connectAny() {
        for (auto &t : trackers) if (connectTo(t.ip, t.port)) return true;
        return false;
    }

    void reauthIfNeeded() {
        string uid, pw, ip; int port;
        {
            lock_guard<mutex> lk(credMx);
            if (!loggedIn) return;
            uid = myUserId; pw = myPassword; ip = myPeerIp; port = myPeerPort;
        }
        string line = "LOGIN " + uid + " " + pw + " " + ip + " " + to_string(port);
        if (sendFrame(fd, line)) {
            string resp;
            recvFrame(fd, resp); // best-effort; the caller's own command surfaces any real failure
        }
    }

    pair<string, string> sendLocked(const string &line, int depth) {
        if (depth > 4) return {"ERR", "Too many tracker redirects/retries"};
        if (fd == -1 && !connectAny()) return {"ERR", "Could not reach any tracker"};

        if (!sendFrame(fd, line)) {
            if (!connectAny()) return {"ERR", "Could not reach any tracker"};
            reauthIfNeeded();
            return sendLocked(line, depth + 1);
        }
        string resp;
        if (!recvFrame(fd, resp)) {
            if (!connectAny()) return {"ERR", "Could not reach any tracker"};
            reauthIfNeeded();
            return sendLocked(line, depth + 1);
        }
        size_t sp = resp.find(' ');
        string status = sp == string::npos ? resp : resp.substr(0, sp);
        string body = sp == string::npos ? "" : resp.substr(sp + 1);
        if (status == "ERR" && body.rfind("NOT_PRIMARY", 0) == 0) {
            vector<string> parts = split(body, ' ');
            if (parts.size() >= 3 && connectTo(parts[1], atoi(parts[2].c_str()))) {
                reauthIfNeeded();
                return sendLocked(line, depth + 1);
            }
            return {"ERR", "Primary tracker is currently unreachable"};
        }
        return {status, body};
    }
};

#endif // CLIENT_TRACKER_CLIENT_H
