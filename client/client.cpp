// ============================================================================
// client.cpp - Client application for the P2P file-sharing system.
//
// Run as:  ./client <IP>:<PORT> <tracker_info.txt>
//
// <IP>:<PORT> is this client's own peer-server address: the address other
// clients connect to when downloading pieces this client is seeding, and
// the address this client advertises to the tracker at login. When testing
// across multiple machines, use a real reachable IP here (not "localhost" -
// every machine's "localhost" means itself, so peers on other hosts would
// try to connect to themselves).
//
// Every command below is a thin translation from the human-facing CLI verbs
// in the assignment spec (create_user, login, upload_file, ...) into the
// tracker's wire protocol (see tracker/tracker.cpp's header comment and
// README.md), plus - for file operations - the local piece-hashing /
// peer-discovery / multi-peer-download machinery in fileops.h,
// downloader.h and peer_server.h.
// ============================================================================

#include <iostream>
#include <string>
#include <vector>
#include <signal.h>
#include <unistd.h>

#include "../common/utils.h"
#include "../common/protocol.h"
#include "tracker_client.h"
#include "fileops.h"
#include "peer_server.h"
#include "downloader.h"

using namespace std;

static string basenameOf(const string &path) {
    size_t pos = path.find_last_of('/');
    return pos == string::npos ? path : path.substr(pos + 1);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <IP>:<PORT> <tracker_info.txt>" << endl;
        return 1;
    }

    vector<string> ipPort = split(argv[1], ':');
    if (ipPort.size() != 2) { cerr << "Expected <IP>:<PORT>, got: " << argv[1] << endl; return 1; }
    string myIp = ipPort[0];
    int myPort = atoi(ipPort[1].c_str());

    TrackerSession session;
    try {
        session.init(parseTrackerList(argv[2]));
    } catch (const std::exception &e) {
        cerr << "Failed to read tracker info: " << e.what() << endl;
        return 1;
    }

    try {
        startPeerServer(myIp, myPort);
    } catch (const std::exception &e) {
        cerr << "Failed to start peer server: " << e.what() << endl;
        return 1;
    }
    logLine("[Client] Peer server listening on " + myIp + ":" + to_string(myPort));
    logLine("[Client] Ready. Type a command (create_user, login, create_group, join_group, leave_group,");
    logLine("         list_groups, list_requests, accept_request, logout, upload_file, list_files,");
    logLine("         download_file, show_downloads, stop_share, exit).");

    string line;
    while (true) {
        cout << "> " << flush;
        if (!getline(cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;
        vector<string> tok = split(line, ' ');
        string cmd = tok[0];

        if (cmd == "exit" || cmd == "quit") {
            break;

        } else if (cmd == "create_user" && tok.size() == 3) {
            auto r = session.send("CREATE_USER " + tok[1] + " " + tok[2]);
            logLine(r.first + " " + r.second);

        } else if (cmd == "login" && tok.size() == 3) {
            if (session.isLoggedIn()) { logLine("ERR Already logged in as " + session.userId() + "; logout first"); continue; }
            auto r = session.send("LOGIN " + tok[1] + " " + tok[2] + " " + myIp + " " + to_string(myPort));
            if (r.first == "OK") {
                session.setCredentials(tok[1], tok[2], myIp, myPort);
                session.setLoggedIn(true);
            }
            logLine(r.first + " " + r.second);

        } else if (cmd == "logout" && tok.size() == 1) {
            if (!session.isLoggedIn()) { logLine("ERR You are not logged in"); continue; }
            auto r = session.send("LOGOUT " + session.userId());
            if (r.first == "OK") session.setLoggedIn(false);
            logLine(r.first + " " + r.second);

        } else if (cmd == "create_group" && tok.size() == 2) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("CREATE_GROUP " + session.userId() + " " + tok[1]);
            logLine(r.first + " " + r.second);

        } else if (cmd == "join_group" && tok.size() == 2) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("JOIN_GROUP " + session.userId() + " " + tok[1]);
            logLine(r.first + " " + r.second);

        } else if (cmd == "leave_group" && tok.size() == 2) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("LEAVE_GROUP " + session.userId() + " " + tok[1]);
            logLine(r.first + " " + r.second);

        } else if (cmd == "list_groups" && tok.size() == 1) {
            auto r = session.send("LIST_GROUPS");
            if (r.first == "OK") {
                if (r.second.empty()) logLine("No groups available.");
                else { cout << "group_id owner members\n" << r.second; }
            } else logLine(r.first + " " + r.second);

        } else if (cmd == "list_requests" && tok.size() == 2) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("LIST_REQUESTS " + session.userId() + " " + tok[1]);
            if (r.first == "OK") {
                if (r.second.empty()) logLine("No pending requests.");
                else cout << r.second;
            } else logLine(r.first + " " + r.second);

        } else if (cmd == "accept_request" && tok.size() == 3) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("ACCEPT_REQUEST " + session.userId() + " " + tok[1] + " " + tok[2]);
            logLine(r.first + " " + r.second);

        } else if (cmd == "upload_file" && tok.size() == 3) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            const string &groupId = tok[1], &path = tok[2];
            string fname = basenameOf(path);
            if (!isValidId(fname)) { logLine("ERR File name must not contain spaces/commas: " + fname); continue; }
            try {
                logLine("Hashing file (this may take a while for large files)...");
                FileMeta meta = computeFileMeta(path);
                string cmdLine = "REGISTER_FILE " + session.userId() + " " + groupId + " " + fname + " " +
                                  to_string(meta.fileSize) + " " + to_string(meta.numPieces) + " " + meta.fileHash +
                                  " " + join(meta.pieceHashes, ',');
                auto r = session.send(cmdLine);
                if (r.first == "OK") {
                    auto rec = make_shared<LocalFileRecord>();
                    rec->groupId = groupId; rec->fileName = fname; rec->localPath = path;
                    rec->fileSize = meta.fileSize; rec->numPieces = meta.numPieces;
                    rec->fileHash = meta.fileHash; rec->pieceHashes = meta.pieceHashes;
                    rec->haveBitmap.assign(meta.numPieces, true);
                    registerLocalFile(rec);
                }
                logLine(r.first + " " + r.second);
            } catch (const std::exception &e) {
                logLine(string("ERR ") + e.what());
            }

        } else if (cmd == "list_files" && tok.size() == 2) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto r = session.send("LIST_FILES " + session.userId() + " " + tok[1]);
            if (r.first == "OK") {
                if (r.second.empty()) logLine("No files shared in this group.");
                else { cout << "name size(bytes) pieces seeders\n" << r.second; }
            } else logLine(r.first + " " + r.second);

        } else if (cmd == "download_file" && tok.size() == 4) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            startDownload(session, session.userId(), tok[1], tok[2], tok[3]);

        } else if (cmd == "show_downloads" && tok.size() == 1) {
            cout << formatShowDownloads();

        } else if (cmd == "stop_share" && tok.size() == 3) {
            if (!session.isLoggedIn()) { logLine("ERR You must login first"); continue; }
            auto rec = findLocalFile(tok[1], tok[2]);
            if (rec) { lock_guard<mutex> lk(rec->mx); rec->sharingStopped = true; }
            auto r = session.send("STOP_SHARE " + session.userId() + " " + tok[1] + " " + tok[2]);
            logLine(r.first + " " + r.second);

        } else {
            logLine("ERR Unknown command or wrong number of arguments: " + line);
        }
    }

    if (session.isLoggedIn()) session.send("LOGOUT " + session.userId());
    return 0;
}
