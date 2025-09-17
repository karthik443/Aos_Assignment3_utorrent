#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <stdexcept>
#include <arpa/inet.h>
#include <thread>
#include <signal.h>
#include<unistd.h>
 #include <fcntl.h>
#include <mutex>
#include <vector>
#include<sstream>
#include<string>
#include<unordered_map>

#include "readFile.h"

using namespace std;

void error(const char *msg)
{
    perror(msg);
    exit(1);
}


enum Tracker_role {PRIMARY,SECONDARY};
vector<string>commandLog;
Tracker_role role;
mutex state_mutex;
int hbSockFd = -1;  // GLOBAL
mutex hbSendMutex;  // protect concurrent sends
bool peerAlive = true;

////////////////////////////////////// STATE MANAGEMENT

struct User
{
    string userId;
    string Password;
    bool isLoggedin = false;

};

struct Group
{
    string groupId;
    string ownerId;
    vector<string> members;               
    vector<string> joinRequests; 
};


unordered_map<string, User> users;       // userId -> User
unordered_map<string, Group> groups;     // groupId -> Group

////////////////////////////////////////////////



void replicateCommand(const string &cmd) {
    lock_guard<mutex> lock(hbSendMutex);
    if (hbSockFd != -1) {
        string msg = "sync:" + cmd + "\n";
        if (send(hbSockFd, msg.c_str(), msg.size(), 0) <= 0) {
            cout << "[Replicator] Failed to send command to secondary." << endl;
        } else {
            cout << "[Replicator] Sent cmd: " << cmd << endl;
        }
    }
}


void heartBeat_Sender(const string &peerIp, int peer_port){

    try
    {
        
    
    while(true){
        // cout<<"Trying again\n";
        int sockFd = socket(AF_INET,SOCK_STREAM,0);
        if(sockFd<0){
            throw runtime_error("Failed to connect Reciever socket");
        }
    
    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(peer_port);
    inet_pton(AF_INET,peerIp.c_str(),&peer_addr.sin_addr);
    

    if(connect(sockFd,(sockaddr*)&peer_addr,sizeof(peer_addr))==0){
        cout<<"[Sender] connected to peer. sending hearbeats"<<endl;
        {
            lock_guard<mutex> lock(hbSendMutex);
            hbSockFd = sockFd;
        }
         {
                    lock_guard<mutex> lock(state_mutex);
                    for (auto &cmd : commandLog) {
                        string msg = "sync:" + cmd + "\n";
                        send(sockFd, msg.c_str(), msg.size(), 0);
                    }
                    string done = "sync:__SYNC_DONE__\n";
                    send(sockFd, done.c_str(), done.size(), 0);
        }

        while (true)
        {
            string hb = "heartbeat destination :"+ to_string(peer_port)+"\n";
            if(send(sockFd,hb.c_str(),hb.size(),0)<=0){
                cout<<"[Sender] connection lost. will retry .."<<endl;
                break;
            }
            this_thread::sleep_for(chrono::seconds(1));
        }
        {
        lock_guard<mutex> lock(hbSendMutex);
        hbSockFd = -1;
         }
        
    }
    else{
        cout<<"[Sender] could not connect to peer. Retrying.."<<endl;
    }
    close(sockFd);
    this_thread::sleep_for(chrono::seconds(2));

    }


    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }

}
void heartBeatRecv(int listen_port){

    try
    {
        int serverFd = socket(AF_INET,SOCK_STREAM,0);
        if(serverFd<0){
            throw runtime_error("Unable to open connect to sender");
        }
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(listen_port);
        int opt = 1;;


        setsockopt(serverFd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        if(bind(serverFd,(sockaddr*)&serv_addr,sizeof(serv_addr))<0){
            throw runtime_error("bind");
        }
        listen(serverFd,5);
        cout<<"[Reciver] listening for hearbeats on port  :"<<listen_port<<endl;

        while(true && peerAlive){
            cout<<"Reciever looping again";
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);

            int clientFd = accept(serverFd,(sockaddr *)&client_addr,&len);
            if(clientFd<=0){
                perror("unable to recieve");
                continue;
            }
            cout<<"[Reciever] Peer connected\n";

            char buffer[128];
            string partial;
            auto last_rev = chrono::steady_clock::now();
             while (true) {
                ssize_t bytes = recv(clientFd, buffer, sizeof(buffer)-1, 0);
                if (bytes <= 0) {
                    peerAlive = false;
                    cout << "[Receiver] Lost connection to peer\n";
                    close(clientFd);
                    break;
                }
                peerAlive = true;
                buffer[bytes] = '\0';
                partial.append(buffer);

                size_t pos;
                while ((pos = partial.find('\n')) != string::npos) {
                    string line = partial.substr(0, pos);
                    partial.erase(0, pos + 1);

                    if (line.rfind("heartbeat", 0) == 0) {
                        cout << "[Receiver] Got heartbeat\n";
                    }
                    else if (line.rfind("sync:", 0) == 0) {
                        string cmd = line.substr(5);
                        if (cmd == "__SYNC_DONE__") {
                            cout << "[Receiver] Initial sync complete.\n";
                        } else {
                            lock_guard<mutex> lock(state_mutex);
                            commandLog.push_back(cmd);
                            cout << "[Receiver] Synced cmd: " << cmd << endl;
                        }
                    }
                }
            }

        }

    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
    
}

void appendCommandLog(const string cmd){
    lock_guard<mutex>lock(state_mutex);
    commandLog.push_back(cmd);
}
void monitorPeer() {
    while(true){
        this_thread::sleep_for(chrono::seconds(1));
        if(role == Tracker_role::SECONDARY && peerAlive == false){
            cout << "[Monitor] Secondary taking primary position" << endl;
            role = Tracker_role::PRIMARY;
            peerAlive = true; // prevent repeated promotion
        }
    }
}
//////////////////////////////////////////////////////client command Executors 

















/////////////////////////////////////////////////////client Command Executors 
void handleClient(int clientSock_fd, sockaddr_in clientSocAddr){
    try
    {
        /* code */
    
    while(1){
        char buffer[1024];
        ssize_t bytes= recv(clientSock_fd,buffer,sizeof(buffer)-1,0);
        if(bytes<=0){
            perror("client connection failed");
            // continue;
            break;
        }
        buffer[bytes] = '\0';
        cout<<"client: "<<buffer;
        if(role==Tracker_role::PRIMARY){
            // lock_guard<mutex> lock(state_mutex);
            string cmd(buffer);
            appendCommandLog(cmd);
            replicateCommand(buffer);
            cout<<"[Primary] logged & Serving client request , current size: "<<commandLog.size()<<endl;
        }else{
            cout<<"[secondary] Ignoring client request ";
        }
        string message = "Server: " + string(buffer);
        int n  = write(clientSock_fd, message.c_str(), message.length());
        if (n < 0) perror("ERROR writing to socket");
    }
   
   
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
    close(clientSock_fd);

}



int main(int argc, char *argv[])
{
    try
    {
 
    signal(SIGPIPE,SIG_IGN);
    if(argc!=3){
        throw runtime_error("Incorrect args");
    }
    string filepath = argv[1];
    cout<<filepath<<" : filepath";
   
    vector<vector<string>>ports  = getPortVector(filepath);
    cout<<"After \n";
    int trackerNo = stoi(argv[2]);
    trackerNo--;

    int port = stoi(ports[trackerNo][1]);
    int peerPort = stoi(ports[(trackerNo+1)%2][1]);
    cout<<"port "<<port<<" peerport"<<peerPort<<endl;
    string ipaddr = ports[trackerNo][0];
    role = (trackerNo==0? Tracker_role::PRIMARY : Tracker_role::SECONDARY);

       cout << "Tracker running on port " << port
         << " Role: " << ((role == Tracker_role::PRIMARY) ? "PRIMARY" : "SECONDARY") << endl;

    // int ipaddr = 
    int sock_fd;
    sock_fd= socket(AF_INET,SOCK_STREAM,0);
    if(sock_fd==-1){
        perror("Unable to create socket fd");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock_fd,(sockaddr * )&addr,sizeof(addr));  
    listen(sock_fd, SOMAXCONN);

    cout << "Tracker running on "<<port<<" ...\n";

//hearbeat threads 
    thread hb_thread;
    if(role==Tracker_role::PRIMARY){
      hb_thread =  thread(heartBeat_Sender, "127.0.0.1",peerPort+100); // send to peer's hb port
    }else{
      hb_thread =  thread(heartBeatRecv, port + 100);  // listen for peer on port+100

    }
    hb_thread.detach();
    thread monitorThread(monitorPeer);
    monitorThread.detach();
    while(true){
        // if(role==Tracker_role::SECONDARY && peerAlive==false){
        //     cout<<"Secondary taking primary position"<<endl;
        //     role = Tracker_role::PRIMARY;
        //     peerAlive = true;
        // }
        sockaddr_in clientSockAddr;
        socklen_t len = sizeof(clientSockAddr);
        int newSocket_fd = accept(sock_fd,(sockaddr*)&clientSockAddr,&len);
        if(newSocket_fd<=0){
            this_thread::sleep_for(chrono::milliseconds(100));
            // perror("Unable to accept socket fd");
            // continue;
        }else{
              thread(handleClient,newSocket_fd,clientSockAddr).detach();
        }   
        
      
    }

    
    
    

    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}