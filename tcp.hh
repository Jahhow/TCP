#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <condition_variable>
#include <vector>
#include <mutex>
#include <string>
#include <streambuf>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <map>
#include <list>

#define NONE "\33[m"
#define BOLD "\33[1m"
#define BLINK "\33[5m"
#define CROSSED_OUT "\33[9m"

#define RED "\33[31m"
#define GREEN "\33[32m"
#define YELLOW "\33[33m"
#define BRIGHT_BLUE "\33[94m"
#define BRIGHT_CYAN "\33[96m"
#define GRAY "\33[38;5;244m"

#define BUF_SIZE 524288     // 512KB
#define TCP_HEADER_LENGTH 5 // Unit: 4-byte
#define MSS 1024
#define RTT 1500ms
#define ACK_DELAY 200ms // micro seconds = 200ms

namespace tcp
{
    using namespace std;
    using namespace std::chrono_literals;

    uint uintRand();
    void printAddr(sockaddr_in &addr);
    void printMsgWithAddr(sockaddr_in &addr, const char *msg);
    class Runnable
    {
    public:
        virtual void run() = 0;
        virtual ~Runnable(){};
    };
    class HandlerThread
    {
        list<Runnable *> queue;
        thread th;
        mutex m;
        condition_variable cv;
        bool started = false;
        static void threadFun(HandlerThread *h);

    public:
        void start();
        void stop();
        void runOnHandlerThread(Runnable *r);
    };
    class TcpSegment
    {
    public:
        ushort srcPort, destPort;
        uint seqNum;
        uint ackNum;
        ushort headerLen_flags, windowSize;
        ushort checksum, urgentPointer;
        uint option[TCP_HEADER_LENGTH - 5];
        char data[MSS - TCP_HEADER_LENGTH * 4];

        TcpSegment();
        void makeChecksum(uint nbyte);

        // return true if valid.
        bool verifyChecksum(uint nbyte);

        ushort getCheckSum(uint nbyte);
        void reset_HeaderLen_Flags(ushort newFlags = 0);

        // Flags
        static const ushort
            NS = 1 << 8,
            CWR = 1 << 7,
            ECE = 1 << 6,
            URG = 1 << 5,
            ACK_FLAG = 1 << 4,
            PSH = 1 << 3,
            RST = 1 << 2,
            SYN_FLAG = 1 << 1,
            FIN = 1 << 0;

        bool isSYN();
        bool isACK();
        bool isFIN();
    };
    class TcpClient;
    class Buf : vector<char>
    {
    public:
        uint protectedSize = 0;
        uint cwnd = -1;
        mutex mutx, eiMutx;
        condition_variable cvSizeChange;
        uint offset;
        vector<uint> gap;
        bool interrupted = false;

        // return 0 if overflow or interrupted
        // blocks until data available
        char *get(uint seqStart, uint nByte, uint &resultLen, unique_lock<mutex> &lk);

        // caller should already obtain 'mutx'
        void interrupt();

        // 'seq': num of the first data byte
        void setSeq(uint seq);
        void setCwnd(uint cwnd);
        uint iOf(uint seq);
        uint seqOf(uint i);
        uint getRwnd();
        bool put(char *data, uint dataLen, uint &resultLen, TcpClient &client, uint minResultLen = 0);
        bool putAt(uint seq, char *data, uint len, bool quiet);

        // the new seqOf(0) will be 'endSeq'
        void dequeue(uint endSeq);
        void _dequeue(uint nbyte);
        void __dequeue(uint nbyte);
    };
    class TcpClient
    {
    public:
        int sockfd;
        sockaddr_in addr;
        sockaddr_in oppoAddr;
        socklen_t size;
        uint sendSeqNum, ackNum, ackSent, recvedAckNum, duplicateRecvAck, ssthresh;
        Buf sendBuf, recvBuf;
        thread sendThread, recvThread, timerThread, ackThread;
        TcpSegment sendSeg, recvSeg;
        condition_variable timerCV, ackCV;
        int timeoutCount = 0;
        char state;
        bool timerSet;
        bool ackThreadActive = false;
        bool simulatePacketLost = false;
        bool verbose = false;
        bool quiet = false;
        char printedState = -1;
        //void (*onFinSent)(TcpClient &client) = 0;

        enum State
        {
            SlowStart,
            CongestionAvoidance,
            FastRecovery,
            Closed
        };

        TcpClient();

        bool send(char *data, uint len, uint &resultLen, uint minResultLen = 1);

        // caller should call recvBuf._dequeue(resultLen) to free the data in the buffer
        char *recv(uint &resultLen, uint nbyte, unique_lock<mutex> &lk);
        void connect(const char *host, const char *port);
        virtual void close();

    protected:
        void startConnectedState();
        bool setClosedState();
        void closeThreads();
        static void sendThreadFun(TcpClient &t);
        static void recvThreadFun(TcpClient &t);
        void processIncomingDataPacket(ssize_t nRecv, TcpSegment &recvSeg);
        static void timerThreadFun(TcpClient &t);
        static void ackThreadFun(TcpClient &t);

        // caller must obtain 'sendBuf.mutx'
        bool sendPacket(char *data, uint len, uint seq);
    };
    class Tcp1toManyServer;
    class TcpChildServer : public TcpClient
    {
    public:
        char serverState = InitialState;
        Tcp1toManyServer *parentServer;
        void *extra = 0;

        enum ServerState
        {
            InitialState,
            WaitForFirstAck,
            Connected,
            Closed
        };

        // 'seg' must have had its Checksum verified.
        // 'oppoAddr' must have been set correctly
        void processIncomingPacket(ssize_t nRecved, TcpSegment &recvedSeg);
        void close() override;
        void _close();

    protected:
        static void onFinSent(TcpChildServer &tcpChildServer);

    private:
        using TcpClient::connect;
    };
    class Tcp1toManyServer : HandlerThread
    {
    public:
        map<uint64_t, TcpChildServer *> serverMap;
        void (*onChildServerConnected)(TcpChildServer &) = 0;
        void (*onCloseChildServer)(TcpChildServer &) = 0; // called on TcpChildServer::sendThread
        void run();
        void childServerClosed(TcpChildServer *childServer);
        static uint64_t getKey(sockaddr_in &addr);
        using HandlerThread::runOnHandlerThread;
        /* 
        ~Tcp1toManyServer()
        {
            for (auto& a : serverMap)
                delete a.second;
        }
        */
    };
} // namespace tcp