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

#include"../tracker/readFile.h"
using namespace std;


int connectToTracker(string hostname,vector<vector<string>>ports){

       

    // try connecting primary , if not working then it will look for secondary port to connect
        

        for(auto ip_port : ports) {

            int sock_fd;
            sockaddr_in serv_addr;
            hostent* server = gethostbyname(ip_port[0].c_str());
            if(!server){
                perror("Unable to find hostname");
                continue;
            } 

            sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if(sock_fd < 0) continue;

            bzero((char*)&serv_addr, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr, server->h_length);
            serv_addr.sin_port = htons(stoi(ip_port[1]));

            if(connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0) {
                cout << "[CLIENT]  Connected to the tracker on Port :  " << ip_port[1] << endl;
                return sock_fd;
            }

            close(sock_fd); 
        }

        throw runtime_error("Unable to connect to any tracker");

    
}

int main(int argc , char * argv []){
    
    

    try
    {

    signal(SIGPIPE, SIG_IGN);

    
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

    ///////////////////////////////client port binding 

    vector<string>ipPortVector =  split(clientIp_port,':');
    string ipAddr = ipPortVector[0];
    int clientPort =stoi(ipPortVector[1]);
   

    int client_sock_fd;
    client_sock_fd= socket(AF_INET,SOCK_STREAM,0);
    if(client_sock_fd==-1){
        perror("Unable to create socket fd");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(clientPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    int opt =1;
    setsockopt(client_sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(client_sock_fd,(sockaddr * )&addr,sizeof(addr));  
    listen(client_sock_fd, SOMAXCONN);





    /////////////////////////////////////////
    int sock_fd = connectToTracker(hostname,ports);
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
            sock_fd = connectToTracker(argv[1], ports);
            continue;
            
        }

        bzero(buffer,256);
        n = read(sock_fd,buffer,255);
        if (n < 0) {
            cout << "[CLIENT] Connection lost. Retrying...\n";
            close(sock_fd);
            sock_fd = connectToTracker(argv[1], ports);
            continue;
         
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