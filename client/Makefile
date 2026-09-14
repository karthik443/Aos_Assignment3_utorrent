CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread

.PHONY: all clean
all: client

HEADERS = tracker_client.h fileops.h peer_server.h downloader.h reciprocity.h \
          ../common/utils.h ../common/netio.h ../common/protocol.h ../common/sha1.h

client: client.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f client
