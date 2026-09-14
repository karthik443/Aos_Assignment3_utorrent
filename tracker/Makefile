CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread

.PHONY: all clean
all: tracker

tracker: tracker.cpp ../common/utils.h ../common/netio.h ../common/protocol.h
	$(CXX) $(CXXFLAGS) tracker.cpp -o tracker

clean:
	rm -f tracker
