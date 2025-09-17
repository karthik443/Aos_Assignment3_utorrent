# Begining
CMD to run tracker ----------------
g++ tracker.cpp -o tracker 

./tracker tracker_info.txt 1
./tracker tracker_info.txt 2

CMD to run client--------------------

g++ client.cpp -o client
./client localhost:portno tracker_info.txt
