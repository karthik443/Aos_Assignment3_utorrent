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
using namespace std;

int main(int argc , char * argv []){
    
    int sock_fd, portno, n;
    sockaddr_in serv_addr;
    hostent * server ; 

    try
    {
       
    
    
    char buffer [256];
    if(argc<3){
        throw runtime_error("Incorrect arguments");
    }
    sock_fd= socket(AF_INET,SOCK_STREAM,0);
    portno = atoi(argv[2]);
    if(sock_fd<0){
         throw runtime_error("Unable to create socket");
    }
    server = gethostbyname(argv[1]);
    if(server==NULL){
          throw runtime_error("Unable to find hosstname");
    }
    bzero((char*)& serv_addr,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char*)server->h_addr,(char*)&serv_addr.sin_addr.s_addr,server->h_length);
    serv_addr.sin_port = htons(portno);

    if(connect(sock_fd,(struct sockaddr *)& serv_addr,sizeof(serv_addr))<0){
        throw runtime_error("Error connecting server");
    }
    while(true){
        printf("Please enter any message: \n");
        bzero(buffer,256);
        fgets(buffer,255,stdin);
        if (strcmp(buffer, "exit\n") == 0) {
             break;
        }
        n = write(sock_fd,buffer,strlen(buffer));
        if(n<0){
            throw runtime_error("Failed to write to server");
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