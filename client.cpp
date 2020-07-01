#include "tcp.hh"
#include "m.hh"
#include <list>
#include <fcntl.h>
#include <sys/stat.h>
using namespace tcp;
list<string> request;
int workFolder;
bool testOpenFileFail = false;
void recvThreadFun(TcpClient &c)
{
    uint resultLen;
    unique_lock<mutex> lk(c.recvBuf.mutx);
    while (c.state != TcpClient::Closed)
    {
        //printf("%lu\n", strlen(str));
        if (!request.size())
        {
            lk.unlock();
            c.close();
            return;
        }
        char *recvData = c.recv(resultLen, 1, lk);
        if (!resultLen)
            return;

        char responce = *recvData;
        c.recvBuf.__dequeue(1);
        if (!request.size())
        {
            printf(RED "Invalid response\n" NONE);
            lk.unlock();
            c.close();
            return;
        }
        string &filename = request.front();
        switch (responce)
        {
        case FileUnavailable:
            printf(RED "File %s is unavailable\n" NONE, filename.c_str());
            break;
        case File:
            while (1)
            {
                recvData = c.recv(resultLen, 4, lk);
                if (resultLen >= 4)
                    break;
                // bug
                if (c.state == TcpClient::Closed)
                    return;
            }
            uint fileSize = *(uint *)recvData;
            c.recvBuf.__dequeue(4);
            int fd = openat(workFolder, filename.c_str(), O_WRONLY | O_CREAT);
            if (testOpenFileFail || fd < 0)
            {
                printf(RED "Can't open file %s\n" NONE, filename.c_str());
                perror("");
                lk.unlock();
                c.close();
                //puts("#");
                return;
            }
            uint writeErrorCount = 0;
            for (uint i = 0; i < fileSize;)
            {
                uint remainSize = fileSize - i;
                /* while (1)
                {
                    puts("#");
                    recvData = c.recv(resultLen, remainSize,lk);
                    if (!resultLen)
                        return;
                    if (resultLen > BUF_SIZE >> 1 || resultLen >= remainSize)
                        break;
                } */
                recvData = c.recv(resultLen, remainSize, lk);
                if (!resultLen)
                    return;
                auto nWrote = write(fd, recvData, resultLen);
                if (nWrote < 0)
                {
                    ++writeErrorCount;
                    perror(RED "Write file error");
                    printf(NONE);
                    if (writeErrorCount > 10)
                    {
                        lk.unlock();
                        c.close();
                        return;
                    }
                    continue;
                }
                else
                {
                    writeErrorCount = 0;
                }
                c.recvBuf.__dequeue(nWrote);
                i += nWrote;
            }
            close(fd);
            printf(BRIGHT_BLUE "Downloaded %s\n" NONE, filename.c_str());
            break;
        }
        request.pop_front();
    }
    //puts("client.cpp: recvThreadFun() exit");
}
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s IP PORT [FILE...]\n", argv[0]);
        exit(2);
    }
    TcpClient c;
    for (int i = 0; i < argc; ++i)
    {
        char *arg = argv[i];
        if (arg[0] != '-')
            continue;
        int j = 1;
        while (1)
        {
            char ch = arg[j];
            if (!ch)
                break;
            switch (ch)
            {
            case 'l':
                c.simulatePacketLost = true;
                break;
            case 'f':
                testOpenFileFail = true;
                break;
            case 'v':
                c.verbose = true;
                break;
            case 'q':
                c.quiet = true;
                break;
            }
            ++j;
        }
    }
    c.connect(argv[1], argv[2]);
    char str[100];
    sprintf(str, "%u", ntohs(c.addr.sin_port));
    workFolder = openat(AT_FDCWD, str, O_RDONLY | O_DIRECTORY);
    if (workFolder < 0)
    {
        if (mkdirat(AT_FDCWD, str, 0777))
        {
            puts(RED "Can't create work folder" NONE);
            return 1;
        }
        workFolder = openat(AT_FDCWD, str, O_RDONLY | O_DIRECTORY);
        if (workFolder < 0)
        {
            perror(RED "Can't open work folder" NONE);
            return 1;
        }
    }
    uint resultLen;
    printMsgWithAddr(c.addr, "Connected\n");
    for (int i = 3; i < argc; ++i)
    {
        char *arg = argv[i];
        if (arg[0] != '-')
        {
            printf("Download: %s\n", arg);
            request.push_back(arg);
            auto len = strlen(arg);
            c.send(arg, len + 1, resultLen);
        }
    }
    thread recvThread(recvThreadFun, ref(c));
    /* while (c.state != TcpClient::Closed)
    {
        printf("Download: ");
        if (!fgets(str, 100, stdin))
            break;
        uint len = strlen(str);
        if (len)
            if (str[len - 1] == '\n')
            {
                if (!--len)
                    continue;
                str[len] = '\0';
            }
        printf("Download: %s\n", str);
        request.push_back(str);
        c.send(str, len + 1, resultLen);
    } */
    recvThread.join();
    //puts("^");
    c.close();
    //puts("$");
    puts("Closed");
}