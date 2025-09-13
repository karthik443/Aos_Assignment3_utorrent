#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std;

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

void handleClient(int clientSock_fd, sockaddr_in clientSocAddr){
    try
    {
        /* code */
    
    while(1){
        char buffer[1024];
        ssize_t bytes= recv(clientSock_fd,buffer,sizeof(buffer)-1,0);
        if(bytes<=0){
            throw runtime_error("Unable fetch info from client");
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
    
    
    int sock_fd;
    sock_fd= socket(AF_INET,SOCK_STREAM,0);
    if(sock_fd==-1){
        throw runtime_error("Unable to create socket fd");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5001);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock_fd,(sockaddr * )&addr,sizeof(addr));  
    listen(sock_fd, SOMAXCONN);

    cout << "Tracker running on port 5000...\n";

    while(true){
        sockaddr_in clientSockAddr;
        socklen_t len = sizeof(clientSockAddr);
        int newSocket_fd = accept(sock_fd,(sockaddr*)&clientSockAddr,&len);
        if(newSocket_fd==-1){
            throw runtime_error("unable to accept");
        }
        
        thread(handleClient,newSocket_fd,clientSockAddr).detach();
    }

    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}