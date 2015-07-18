.PHONY: all clean

CC=clang
CXX=clang++
CXXFLAGS=-std=c++11 -Wall -Wextra -Wshadow -DBOOST_SYSTEM_NO_DEPRECATED -DBOOST_ASIO_HAS_STD_CHRONO -O2 -g -pipe
LDLIBS=-lstdc++ -lboost_system -lpthread

all: dns-tcp2udp

dns-tcp2udp: Proxy.o Server.o Client.o dns-tcp2udp.o
dns-tcp2udp.o: dns-tcp2udp.hpp
Proxy.o: dns-tcp2udp.hpp
Server.o: dns-tcp2udp.hpp
Client.o: dns-tcp2udp.hpp

clean:
	rm -f -- dns-tcp2udp *.o
