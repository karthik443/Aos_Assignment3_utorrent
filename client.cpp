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

#include"readFile.h"
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
    vector<int> trackerPorts;
    
    
    char buffer [256];
    if(argc<3){
        throw runtime_error("Incorrect arguments");
    }
    string clientIp_port = argv[1];
    string filepath = argv[2];
    vector<vector<string>>ports  = getPortVector(filepath);
    string hostname = ports[0][0];
    int primaryPort = stoi(ports[0][1]);
    int secondaryPort = stoi(ports[1][1]);
    trackerPorts.push_back(primaryPort);
    trackerPorts.push_back(secondaryPort);

    int sock_fd = connectToTracker(hostname,trackerPorts);
    while(true){
        printf("Please enter any message: \n");
        bzero(buffer,256);
        fgets(buffer,255,stdin);

        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (strcmp(buffer, "exit") == 0) {
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