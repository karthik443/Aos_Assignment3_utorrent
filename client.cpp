#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <stdexcept>
#include<iostream>
#include<vector>
#include<signal.h>
using namespace std;


int connectToTracker(string hostname,vector<int>&ports){
    
        int sock_fd;
        sockaddr_in serv_addr;
        hostent* server = gethostbyname(hostname.c_str());
        if(!server) throw runtime_error("Unable to find hostname");

        for(int port : ports) {
            sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if(sock_fd < 0) continue;

            bzero((char*)&serv_addr, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr, server->h_length);
            serv_addr.sin_port = htons(port);

            if(connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0) {
                cout << "[CLIENT] Connected to tracker on port " << port << endl;
                return sock_fd; // success
            }

            close(sock_fd); // try next port
        }

        throw runtime_error("Unable to connect to any tracker");

    
}

int main(int argc , char * argv []){
    
    

    try
    {

        signal(SIGPIPE, SIG_IGN);
    vector<int> trackerPorts = {5000, 5001};
    
    
    char buffer [256];
    if(argc<3){
        throw runtime_error("Incorrect arguments");
    }
    int sock_fd = connectToTracker(argv[1],trackerPorts);
    while(true){
        printf("Please enter any message: \n");
        bzero(buffer,256);
        fgets(buffer,255,stdin);
        if (strcmp(buffer, "exit\n") == 0) {
             break;
        }
        int n = write(sock_fd,buffer,strlen(buffer));
        if(n<=0){
            cout << "[CLIENT] Connection lost. Retrying...\n";
            close(sock_fd);
            sock_fd = connectToTracker(argv[1], trackerPorts);
            continue;
            
        }

        bzero(buffer,256);
        n = read(sock_fd,buffer,255);
        if (n < 0) {
            throw runtime_error("Failed to read to server");
        }
            
        printf("%s\n",buffer);
    }
    
    close(sock_fd);



    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}