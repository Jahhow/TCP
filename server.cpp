#include "tcp.hh"
#include "m.hh"
#include <list>
#include <string>
#include <fcntl.h> /* Definition of AT_* constants */
#include <sys/stat.h>
using namespace tcp;
class ChildServerExtra
{
public:
    thread serverThread;
};
bool simulatePacketLost = false;
bool verbose = false;
bool quiet = false;
class FileCache
{
public:
    uint size;
    char *data = 0;
};
map<string, FileCache> file;
void serverRecvThreadFun(TcpChildServer &childServer /* , list<string> &request */)
{
    uint resultLen;
    unique_lock<mutex> lk(childServer.recvBuf.mutx);
    while (childServer.state != TcpChildServer::Closed)
    {
        char *recvData = childServer.recv(resultLen, 100, lk);
        if (resultLen)
        {
            uint maxlen = min(resultLen, 100U);
            char *nc = find(recvData, recvData + maxlen, '\0');
            if (nc == recvData + maxlen)
            {
                if (maxlen < 100U)
                    continue;
                *--nc = '\0';
            }
            char *filename = recvData;
            putchar('[');
            printAddr(childServer.oppoAddr);
            printf("]: ");
            auto findFile = file.find(filename);
            if (findFile == file.end())
            {
                printf("File unavailable: %s\n", filename);
                lk.unlock();
                char response = FileUnavailable;
                childServer.send(&response, 1, resultLen);
            }
            else
            {
                printf("Sending file %s\n", filename);
                lk.unlock();
                char response = File;
                childServer.send(&response, 1, resultLen);
                //puts("#");
                uint fileSize = findFile->second.size;
                childServer.send((char *)&fileSize, 4, resultLen, 4);
                char *data = findFile->second.data;
                char *end = data + fileSize;
                //puts("&");
                while (data < end)
                {
                    childServer.send(data, end - data, resultLen, BUF_SIZE >> 1);
                    data += resultLen;
                    //puts("+");
                }
            }
            //puts("$");
            lk.lock();
            //puts("^");
            childServer.recvBuf.__dequeue(nc + 1 - recvData);
        }
    }
}
void serverThreadFun(TcpChildServer &childServer)
{
    //list<string> request;
    thread recvThread(serverRecvThreadFun, ref(childServer) /* , ref(request) */);
    printMsgWithAddr(childServer.oppoAddr, "Connected\n");
    /* while (childServer.state != TcpChildServer::Closed)
    {
        if (!fgets(str, 100, stdin))
        {
            childServer.close();
            break;
        }
        childServer.send(str, strlen(str));
    } */
    //puts("[");
    recvThread.join();
    //puts("]");
}
void onChildServerConnected(TcpChildServer &childServer)
{
    ChildServerExtra *e = new ChildServerExtra;
    childServer.extra = e;
    e->serverThread = thread(serverThreadFun, ref(childServer));
    childServer.simulatePacketLost = simulatePacketLost;
    childServer.verbose = verbose;
    childServer.quiet = quiet;
}
void onCloseChildServer(TcpChildServer &childServer)
{
    //puts("&");
    ChildServerExtra *e = (ChildServerExtra *)childServer.extra;
    if (e)
    {
        e->serverThread.join();
        delete e;
        //puts(")");
    }
    printMsgWithAddr(childServer.oppoAddr, "Closed\n");
}
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        char *arg = argv[i];
        if (arg[0] != '-')
        {
            char *filename = arg;
            int fd = openat(AT_FDCWD, filename, O_RDONLY);
            if (fd == -1)
                continue;
            struct stat st;
            if (fstat(fd, &st))
            {
                close(fd);
                continue;
            }
            FileCache fc;
            auto size = st.st_size;
            fc.size = size;
            fc.data = new char[size];
            file[filename] = fc;
            char *data = fc.data;
            char *end = data + size;
            puts(filename);
            while (data < end)
            {
                auto n = read(fd, data, end - data);
                if (n <= 0)
                {
                    printf(RED "Error caching file %s\n" NONE, filename);
                    exit(1);
                }
                //printf("%ld\n", n);
                data += n;
            }
            close(fd);
        }
        else
        {
            int j = 1;
            while (1)
            {
                char ch = arg[j];
                if (!ch)
                    break;
                switch (ch)
                {
                case 'l':
                    simulatePacketLost = true;
                    break;
                case 'v':
                    verbose = true;
                    break;
                case 'q':
                    quiet = true;
                    break;
                }
                ++j;
            }
        }
    }
    Tcp1toManyServer s;
    s.onChildServerConnected = onChildServerConnected;
    s.onCloseChildServer = onCloseChildServer;
    s.run();

    // /**開起檔案讀取文字 **/
    // fdesc = open(argv[2], O_RDONLY);
    // if (fdesc == -1)
    // {
    //     perror("open file error!");
    //     exit(1);
    // }
    // ndata = read(fdesc, data, MAXDATA);
    // if (ndata < 0)
    // {
    //     perror("read file error !");
    //     exit(1);
    // }
    // data[ndata + 1] = '\0';

    // /* 發送資料給 Server */
    // size = sizeof(serverAddr);
    // if (sendto(sockfd, data, ndata, 0, (struct sockaddr *)&serverAddr, size) == -1)
    // {
    //     perror("write to server error !");
    //     exit(1);
    // }
}