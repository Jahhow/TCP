all: client server
client: client.cpp tcp.cpp tcp.hh m.hh
	g++ tcp.cpp client.cpp -o client -std=c++14 -pthread
server: server.cpp tcp.cpp tcp.hh m.hh
	g++ tcp.cpp server.cpp -o server -std=c++14 -pthread

clean:
	rm -r *.o server client `ls | grep '^[0-9]*$'`