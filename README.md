# Commands to run the application
CMD to run tracker ---------------- Go inside tracker folder
g++ tracker.cpp -o tracker

./tracker ../tracker_info.txt 1
./tracker ../tracker_info.txt 2

CMD to run client-------------------- go inside client folder

g++ client.cpp ../tracker/readFile.h -o client
./client localhost:8080 ../tracker_info.txt


Approach : 

1. To establish client-tracker communication I binded the tracker socket with port mentioned in trackerinfo.txt. 
2. client will send its messages to the port mentioned in the same tracker_info.txt file
3. tracker will execute the request accordingly the client will send. 

# tracker to tracker sync

1. I start both the trackers in two different ports mentioned 
2. Both trackers will again listen in two another ports i.e currentport+ 100 . It will send/listen the heartbeat signals of another port
3. secondary tracker will take its position as primary if primary stops sending hearbeats.
4. And in the same way whenever client sends a command , will execute the command in primary and i will send the same command to secondary to maintain consistent state.
5. Additionally when the secondary connects to primary for the first time, I will send all current history of primary to secondary to bring synchronisation between two trackers. 

used mutex locks for shared variables , user logins, session management etc