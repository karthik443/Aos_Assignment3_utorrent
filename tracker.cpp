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
using namespace std;

void error(const char *msg)
{
    perror(msg);
    exit(1);
}


void ignoreSigPipe() {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);
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
        while (true)
        {
            string hb = "heartbeat destination :"+ to_string(peer_port);
            if(send(sockFd,hb.c_str(),hb.size(),0)<=0){
                cout<<"[Sender] connection lost. will retry .."<<endl;
                break;
            }
            this_thread::sleep_for(chrono::seconds(1));
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

        while(true){
            // cout<<"Reciever looping again";
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);

            int clientFd = accept(serverFd,(sockaddr *)&client_addr,&len);
            if(clientFd<=0){
                perror("unable to recieve");
                continue;
            }
            cout<<"[Reciever] Peer connected\n";

            char buffer[128];
            auto last_rev = chrono::steady_clock::now();
            while(true){
                size_t bytes = recv(clientFd,buffer,sizeof(buffer)-1,0);
                if(bytes<=0){
                    cout<<"[Reciver] Lost connection to peer \n";
                    close(clientFd);
                    break;
                }
                buffer[bytes] = '\0';
                last_rev = chrono::steady_clock::now();
                cout<<"[Reciever] Got heartbeat: "<<buffer<<endl;
            }

        }

    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
    
}


void handleClient(int clientSock_fd, sockaddr_in clientSocAddr){
    try
    {
        /* code */
    
    while(1){
        char buffer[1024];
        ssize_t bytes= recv(clientSock_fd,buffer,sizeof(buffer)-1,0);
        if(bytes<=0){
            perror("Unable fetch info from client");
            continue;
        }
        buffer[bytes] = '\0';
        
        cout<<"client: "<<buffer;
        string message = "Server: " + string(buffer);
       // bzero(buffer,sizeof(buffer));
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
        /* code */
    ignoreSigPipe();
    if(argc!=2){
        throw runtime_error("Incorrect args");
    }
    int port = stoi(argv[1]);
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
    thread hb_recv(heartBeatRecv, port + 100);  // listen for peer on port+100
    thread hb_send(heartBeat_Sender, "127.0.0.1", (port == 5000 ? 5101 : 5100)); // send to peer's hb port

    while(true){
        sockaddr_in clientSockAddr;
        socklen_t len = sizeof(clientSockAddr);
        int newSocket_fd = accept(sock_fd,(sockaddr*)&clientSockAddr,&len);
        if(newSocket_fd==-1){
            perror("Unable to accept socket fd");
            continue;
        }
        
        thread(handleClient,newSocket_fd,clientSockAddr).detach();
    }
    hb_recv.join();
    hb_send.join();

    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}