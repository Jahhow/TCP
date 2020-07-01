#include "tcp.hh"

namespace tcp
{
    using namespace std;
    using namespace std::chrono_literals;

    const uint MDS = MSS - TCP_HEADER_LENGTH * 4; // Max Data Size per Segment
    random_device r;
    default_random_engine e(r());
    uniform_int_distribution<uint> uniform_dist;

    uint uintRand()
    {
        return uniform_dist(e);
    }
    void printAddr(sockaddr_in &addr)
    {
        char ipstr[16];
        inet_ntop(AF_INET, &(addr.sin_addr), ipstr, 16);
        printf("%.15s:%u", ipstr, ntohs(addr.sin_port));
    }
    void printMsgWithAddr(sockaddr_in &addr, const char *msg)
    {
        putchar('[');
        printAddr(addr);
        printf("] %s", msg);
    }

    void HandlerThread::start()
    {
        started = true;
        th = thread(threadFun, this);
    }
    void HandlerThread::stop()
    {
        started = false;
        th.join();
    }
    void HandlerThread::runOnHandlerThread(Runnable *r)
    {
        m.lock();
        queue.push_back(r);
        m.unlock();
        cv.notify_one();
    }
    void HandlerThread::threadFun(HandlerThread *h)
    {
        unique_lock<mutex> l(h->m);
        while (h->started)
        {
            if (h->queue.size())
            {
                auto e = h->queue.front();
                e->run();
                delete e;
                h->queue.pop_front();
            }
            else
                h->cv.wait(l);
        }
    }
    TcpSegment::TcpSegment() { reset_HeaderLen_Flags(); }
    void TcpSegment::makeChecksum(uint nbyte)
    {
        checksum = 0;
        checksum = getCheckSum(nbyte);
    }

    // return true if valid.
    bool TcpSegment::verifyChecksum(uint nbyte)
    {
        return !getCheckSum(nbyte);
    }
    ushort TcpSegment::getCheckSum(uint nbyte)
    {
        if (nbyte & 1)
        {
            ((char *)this)[nbyte] = 0;
            ++nbyte;
        }
        uint nshort = nbyte >> 1;
        uint sum = 0;
        for (uint i = 0; i < nshort; ++i)
        {
            sum += ((ushort *)this)[i];
            sum += sum >> 16;
            sum &= 0xFFFF;
        }
        return ~sum;
    }
    void TcpSegment::reset_HeaderLen_Flags(ushort newFlags /* = 0 */)
    {
        headerLen_flags = TCP_HEADER_LENGTH << 12 | newFlags;
    }
    bool TcpSegment::isSYN() { return headerLen_flags & TcpSegment::SYN_FLAG; }
    bool TcpSegment::isACK() { return headerLen_flags & TcpSegment::ACK_FLAG; }
    bool TcpSegment::isFIN() { return headerLen_flags & TcpSegment::FIN; }

    // return 0 if overflow or interrupted
    // blocks until data available
    // 'lk': already locked unique_lock of 'recvBuf.mutx'
    char *Buf::get(uint seqStart, uint nByte, uint &resultLen, unique_lock<mutex> &lk)
    {
        uint cwndProtectedSize;
        while (1)
        {
            cwndProtectedSize = min(protectedSize, cwnd);
            auto iseq = iOf(seqStart);
            if (iseq < cwndProtectedSize)
                break;
            cvSizeChange.wait(lk);
            if (interrupted)
            {
                interrupted = false;
                resultLen = 0;
                return 0;
            }
        }
        resultLen = min(nByte, cwndProtectedSize - iOf(seqStart));
        return data() + iOf(seqStart);
    }

    // caller should already obtain 'mutx'
    void Buf::interrupt()
    {
        interrupted = true;
        cvSizeChange.notify_all();
    }
    void Buf::setSeq(uint seq) // seq num of the first data byte
    {
        offset = -seq;
    }
    void Buf::setCwnd(uint cwnd)
    {
        mutx.lock();
        Buf::cwnd = cwnd;
        cvSizeChange.notify_all();
        mutx.unlock();
    }
    uint Buf::iOf(uint seq)
    {
        return seq + offset;
    }
    uint Buf::seqOf(uint i)
    {
        return i - offset;
    }
    uint Buf::getRwnd()
    {
        return BUF_SIZE - size();
    }
    bool Buf::put(char *data, uint dataLen, uint &resultLen, TcpClient &client, uint minResultLen)
    {
        if (minResultLen > BUF_SIZE)
            return false;
        uint bufFreeSpace;
        //puts("<");
        unique_lock<mutex> lk(mutx);
        while (1)
        {
            if (client.state == TcpClient::Closed)
                return false;
            bufFreeSpace = BUF_SIZE - size();
            if (bufFreeSpace >= minResultLen)
                break;
            //puts("?");
            cvSizeChange.wait(lk);
            //puts("!");
        }
        resultLen = min(bufFreeSpace, dataLen);
        reserve(size() + resultLen);
        lk.unlock();
        //puts(")");
        eiMutx.lock();
        insert(end(), data, data + resultLen);
        eiMutx.unlock();
        //puts("=");
        mutx.lock();
        protectedSize = size();
        cvSizeChange.notify_all();
        mutx.unlock();
        //puts("?");
        return true;
    }
    bool Buf::putAt(uint seq, char *data, uint len, bool quiet)
    {
        uint seqEnd = seq + len;
        uint iGapBegin = 0;
        unique_lock<mutex> lk(mutx);
        //puts("^");
        uint iSeq = iOf(seq);
        uint iSeqEnd = iOf(seqEnd);
        if (iSeqEnd > BUF_SIZE)
            return false;
        if ((int)iSeq < 0)
            return false;
        if (iSeqEnd <= protectedSize)
            return true;
        if (iSeq > protectedSize)
        {
            if (!quiet)
                printf(YELLOW "Inconsecutive " NONE);
            uint previ, nexti; // index of 'gap'
            auto gapSize = gap.size();
            if (gapSize)
            {
                uint lastG = 0;
                bool i1set = false;
                for (uint i = 0; i < gapSize; ++i)
                {
                    uint g = iOf(gap[i]);
                    if (iSeq < g)
                    {
                        nexti = i;
                        i1set = true;
                        break;
                    }
                    lastG = g;
                }
                if (!i1set)
                {
                    if (iSeq <= size())
                    {
                        if (iSeqEnd <= size())
                            return true;
                        resize(iSeqEnd);
                    }
                    else
                    {
                        gap.push_back(seqOf(size()));
                        gap.push_back(seq);
                        resize(iSeqEnd);
                    }
                }
                else
                {
                    if (nexti & 1) // in gap
                    {
                        previ = nexti - 1;
                        uint gapStartToI = iOf(gap[previ]);
                        uint gapEndToI = iOf(gap[nexti]);
                        if (iGapBegin == iSeq)
                        {
                            if (iSeqEnd < gapEndToI)
                            {
                                gap[previ] = seqEnd;
                            }
                            else
                            {
                                auto first = gap.begin() + previ;
                                gap.erase(first, first + 2);
                                iGapBegin = previ;
                                goto InOrderData;
                            }
                        }
                        else
                        {
                            if (iSeqEnd < gapEndToI)
                            {
                                gap.insert(gap.begin() + nexti, 2, seq);
                                gap[nexti + 1] = seqEnd;
                            }
                            else
                            {
                                gap[nexti] = seq;
                                iGapBegin = nexti + 1;
                                goto InOrderData;
                            }
                        }
                    }
                    else
                    {
                        iGapBegin = nexti;
                        goto InOrderData;
                    }
                }
            }
            else
            {
                gap.push_back(seqOf(protectedSize));
                gap.push_back(seq);
                resize(iSeqEnd);
            }
        }
        else
        {
        InOrderData:
            uint nGapElementToErase = 0;
            size_t gapSize = gap.size();
            for (uint i = iGapBegin; i < gapSize; i += 2)
            {
                uint gapStartToI = iOf(gap[i]);
                if (iSeqEnd <= gapStartToI)
                    break;
                else
                {
                    uint gapEndToI = iOf(gap[i + 1]);
                    if (iSeqEnd >= gapEndToI)
                    {
                        nGapElementToErase += 2;
                    }
                    else
                    {
                        gap[i] = seqOf(iSeqEnd);
                        break;
                    }
                }
            }
            auto gapBegin = gap.begin() + iGapBegin;
            if (nGapElementToErase)
                gap.erase(gapBegin, gapBegin + nGapElementToErase);

            if (iSeqEnd > size())
                resize(iSeqEnd);
            uint new_protectedSize = gap.size() ? iOf(gap[0]) : size();
            if (new_protectedSize != protectedSize)
            {
                protectedSize = new_protectedSize;
                cvSizeChange.notify_all();
            }
        }
        copy(data, data + len, begin() + iSeq);
        return true;
    }
    void Buf::dequeue(uint endSeq) // the new seqOf(0) will be 'endSeq'
    {
        lock_guard<mutex> lk(mutx);
        __dequeue(iOf(endSeq));
    }
    void Buf::_dequeue(uint nbyte)
    {
        lock_guard<mutex> lk(mutx);
        __dequeue(nbyte);
    }

    // caller must already obtain 'mutx'
    void Buf::__dequeue(uint nbyte)
    {
        if (!nbyte)
            return;
        protectedSize -= nbyte;
        offset -= nbyte;
        //puts(">");
        eiMutx.lock();
        erase(begin(), begin() + nbyte);
        eiMutx.unlock();
        //puts("=");
        cvSizeChange.notify_all();
    }

    TcpClient::TcpClient() {}

    bool TcpClient::send(char *data, uint len, uint &resultLen, uint minResultLen)
    {
        return sendBuf.put(data, len, resultLen, *this, minResultLen);
    }

    // caller should call recvBuf._dequeue(resultLen) to free the data in the buffer
    char *TcpClient::recv(uint &resultLen, uint nbyte, unique_lock<mutex> &lk)
    {
        if (state == Closed)
        {
            resultLen = 0;
            return 0;
        }
        return recvBuf.get(recvBuf.seqOf(0), nbyte, resultLen, lk);
    }
    void TcpClient::connect(const char *host, const char *port)
    {
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        {
            perror("socket failed!");
            exit(1);
        }

        bzero(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        sendSeg.srcPort = addr.sin_port = htons(0);

        if (bind(sockfd, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            perror("bind failed!");
            exit(1);
        }

        bzero((char *)&oppoAddr, sizeof(oppoAddr));
        oppoAddr.sin_family = AF_INET;
        sendSeg.destPort = oppoAddr.sin_port = htons(stoi(port));
        hostent *hp = gethostbyname(host);
        if (hp == 0)
        {
            fprintf(stderr, "could not obtain address of %s\n", port);
            exit(-1);
        }
        bcopy(hp->h_addr_list[0], (caddr_t)&oppoAddr.sin_addr, hp->h_length);

        sendSeg.seqNum = sendSeqNum = uintRand();
        sendSeg.headerLen_flags |= TcpSegment::SYN_FLAG;
        sendSeg.makeChecksum(TCP_HEADER_LENGTH * 4);
        if (sendto(sockfd, &sendSeg, TCP_HEADER_LENGTH * 4, 0, (sockaddr *)&oppoAddr, sizeof(oppoAddr)) == -1)
        {
            perror("send SYN error");
            exit(1);
        }
        puts("SYN");

        size = sizeof(oppoAddr);
        auto nRecved = recvfrom(sockfd, &recvSeg, TCP_HEADER_LENGTH * 4, 0, (sockaddr *)&oppoAddr, &size);
        if (nRecved < 0)
        {
            puts("recv SYN ACK error");
            exit(1);
        }
        if (nRecved < TCP_HEADER_LENGTH * 4)
        {
            puts("SYN ACK invalid: too short");
            exit(1);
        }
        if (!recvSeg.isSYN() || !recvSeg.isACK())
        {
            puts("Packet is not SYN ACK");
            exit(1);
        }
        if (recvSeg.ackNum != ++sendSeqNum)
        {
            puts("SYN ACK has incorrect ACK num");
            //printf("%u\n", recvSeg.ackNum);
            exit(1);
        }
        if (!recvSeg.verifyChecksum(TCP_HEADER_LENGTH * 4))
        {
            puts("SYN ACK invalid");
            exit(1);
        }
        puts("SYN ACK");

        sendSeg.seqNum = sendSeqNum;
        sendSeg.ackNum = ackNum = recvSeg.seqNum + 1;
        sendSeg.reset_HeaderLen_Flags(TcpSegment::ACK_FLAG);
        sendSeg.makeChecksum(TCP_HEADER_LENGTH * 4);
        if (sendto(sockfd, &sendSeg, TCP_HEADER_LENGTH * 4, 0, (sockaddr *)&oppoAddr, sizeof(oppoAddr)) == -1)
        {
            perror("send 3rd-Handshake error");
            exit(1);
        }
        puts("    ACK");
        startConnectedState();
        recvThread = thread(recvThreadFun, ref(*this));

        size = sizeof(addr);
        if (getsockname(sockfd, (sockaddr *)&addr, &size))
        {
            perror("getsockname error");
            exit(1);
        }
    }
    bool TcpClient::setClosedState()
    {
        char oldState;
        //puts("!");
        recvBuf.mutx.lock();
        //puts("@");
        sendBuf.mutx.lock();
        //puts("&");
        oldState = TcpClient::state;
        TcpClient::state = Closed;
        sendBuf.mutx.unlock();
        recvBuf.mutx.unlock();
        return oldState != Closed;
    }
    void TcpClient::close()
    {
        if (!setClosedState())
            return;
        closeThreads();
        shutdown(sockfd, SHUT_RD);
        //puts("1");
        recvThread.join();
    }
    void TcpClient::closeThreads()
    {
        //puts("*");
        sendBuf.mutx.lock();
        sendBuf.interrupt();
        ackCV.notify_all();
        sendBuf.mutx.unlock();
        recvBuf.mutx.lock();
        recvBuf.interrupt();
        recvBuf.mutx.unlock();
        timerCV.notify_all();
        //puts("^");
        sendThread.join();
        //puts("2");
        if (this_thread::get_id() != timerThread.get_id())
            timerThread.join();
        //puts("3");
        ackThread.join();
        //puts("4");
    }

    void TcpClient::startConnectedState()
    {
        state = SlowStart;
        duplicateRecvAck = 0;
        ssthresh = 64 << 10;
        timerSet = false;
        sendBuf.setSeq(sendSeqNum);
        sendBuf.setCwnd(MDS);
        recvBuf.setSeq(ackNum);
        recvedAckNum = sendSeqNum;
        ackSent = ackNum;
        sendThread = thread(sendThreadFun, ref(*this));
        timerThread = thread(timerThreadFun, ref(*this));
        ackThread = thread(ackThreadFun, ref(*this));
    }
    void TcpClient::sendThreadFun(TcpClient &t)
    {
        unique_lock<mutex> lk(t.sendBuf.mutx);
        while (t.state != Closed)
        {
            lk.unlock();
            uint resultLen;
            lk.lock();
            char *data = t.sendBuf.get(t.sendSeqNum, MDS, resultLen, lk);
            if (resultLen)
            {
                t.sendPacket(data, resultLen, t.sendSeqNum);
                t.sendSeqNum += resultLen;
                if (!t.timerSet)
                {
                    t.timerCV.notify_all();
                }
            }
        }
        t.sendSeg.reset_HeaderLen_Flags(TcpSegment::FIN);
        t.sendSeg.makeChecksum(TCP_HEADER_LENGTH * 4);
        sendto(t.sockfd, &t.sendSeg, TCP_HEADER_LENGTH * 4, 0, (sockaddr *)&t.oppoAddr, sizeof(oppoAddr));
        // if (t.onFinSent)
        //     t.onFinSent(t);
    }
    void TcpClient::recvThreadFun(TcpClient &t)
    {
        while (t.state != Closed)
        {
            t.size = sizeof(oppoAddr);
            ssize_t nRecv = recvfrom(t.sockfd, &t.recvSeg, MSS, 0, (sockaddr *)&t.oppoAddr, &t.size);
            //printf("nRecv: %ld\n", nRecv);
            if (nRecv < 0)
            {
                perror("recvfrom error");
                exit(1);
            }
            //puts("%");
            if (nRecv < TCP_HEADER_LENGTH * 4 || !t.recvSeg.verifyChecksum(nRecv))
                continue;
            //puts("!");
            if (t.recvSeg.isFIN())
            {
                t.close();
                return;
            }
            if (t.recvSeg.isSYN() || !t.recvSeg.isACK())
            {
                puts("Invalid flag received");
                exit(1);
            }
            t.processIncomingDataPacket(nRecv, t.recvSeg);
        }
    }
    void TcpClient::processIncomingDataPacket(ssize_t nRecv, TcpSegment &recvSeg)
    {
        uint dataLen = nRecv - TCP_HEADER_LENGTH * 4;
        if (dataLen)
        {
            bool putAtSucceed = recvBuf.putAt(recvSeg.seqNum, recvSeg.data, dataLen, quiet);
            if (putAtSucceed)
            {
                if (verbose)
                    printf("Received packet. DataLen: %u, Seq: %x\n", dataLen, recvSeg.seqNum);
            }
            uint newAckNum;
            recvBuf.mutx.lock();
            newAckNum = recvBuf.seqOf(recvBuf.protectedSize);
            recvBuf.mutx.unlock();
            sendBuf.mutx.lock();
            if (newAckNum == ackNum)
            {
                --ackSent; // allowing duplicate ack
                ackCV.notify_all();
            }
            else
            {
                ackNum = newAckNum;
                if (!ackThreadActive)
                {
                    ackCV.notify_all();
                }
            }
            sendBuf.mutx.unlock();
            if (!putAtSucceed)
            {
                if (!quiet)
                    printf("Dropped packet. DataLen: %u, Seq: %x\n", dataLen, recvSeg.seqNum);
                return;
            }
        }
        else
        {
            if (!quiet)
                printf(GREEN "Received ACK: %x" NONE "\n", recvSeg.ackNum);
        }
        // if (!sendBuf.protectedSize)
        //     return;
        uint newRecvedAck = recvSeg.ackNum;
        //printf("Unacked size: %d\n", sendBuf.protectedSize);
        if (newRecvedAck == recvedAckNum)
        {
            if (dataLen == 0) // if this is definitely an ack packet
            {
                ++duplicateRecvAck;
                if (!quiet)
                    printf(YELLOW "Duplicate ACK %d" NONE "\n", duplicateRecvAck);
                if (state == FastRecovery)
                {
                    sendBuf.setCwnd(sendBuf.cwnd + MDS);
                }
                else
                {
                    if (duplicateRecvAck == 3)
                    {
                        if (!quiet)
                            puts(GRAY "Fast Recovery" NONE);
                        ssthresh = sendBuf.cwnd >> 1;
                        sendBuf.setCwnd(ssthresh + 3);
                        state = FastRecovery;
                        sendBuf.mutx.lock();
                        sendSeqNum = recvedAckNum;
                        sendBuf.interrupt();
                        sendBuf.mutx.unlock();
                    }
                }
            }
        }
        else
        {
            duplicateRecvAck = 0;
            uint ackNumDiff = newRecvedAck - recvedAckNum;
            if ((int)ackNumDiff < 0)
                return;
            recvedAckNum = newRecvedAck;
            //puts("#");
            sendBuf.mutx.lock();
            if ((int)(newRecvedAck - sendSeqNum) > 0)
            {
                //puts("$");
                sendSeqNum = newRecvedAck;
                sendBuf.interrupt();
            }
            sendBuf.mutx.unlock();
            //puts("*");
            sendBuf.dequeue(newRecvedAck);
            //puts("@");
            uint newCwnd;
            switch (state)
            {
            case SlowStart:
                newCwnd = sendBuf.cwnd + ackNumDiff;
                if (newCwnd >= ssthresh)
                    state = CongestionAvoidance;
                break;
            case CongestionAvoidance:
                if (sendBuf.cwnd)
                    newCwnd = sendBuf.cwnd + ackNumDiff * ackNumDiff / sendBuf.cwnd;
                break;
            case FastRecovery:
                newCwnd = ssthresh;
                state = CongestionAvoidance;
                break;
            }
            sendBuf.setCwnd(newCwnd);
            timerCV.notify_all();
            if (printedState != state && !quiet)
            {
                switch (state)
                {
                case SlowStart:
                    puts(GRAY "Slow Start" NONE);
                    break;
                case CongestionAvoidance:
                    puts(GRAY "Congestion Avoidance" NONE);
                    break;
                    /*
                case FastRecovery:
                    puts(GRAY "Fast Recovery" NONE);
                    break; */
                }
                printedState = state;
            }
        }
    }
    void TcpClient::timerThreadFun(TcpClient &t)
    {
        unique_lock<mutex> lk(t.sendBuf.mutx);
        while (t.state != Closed)
        {
            //puts("%");
            if (t.timerSet = t.sendBuf.protectedSize)
            {
                if (t.timerCV.wait_for(lk, RTT) == cv_status::timeout)
                {
                    if (!t.quiet)
                        printf("Timeout %d\n", ++t.timeoutCount);

                    /* if (t.timeoutCount > 30)
                    {
                        lk.unlock();
                        t.close();
                        return;
                    } */
                    t.ssthresh = t.sendBuf.cwnd >> 1;
                    t.sendBuf.cwnd = MDS;
                    t.state = SlowStart;
                    t.duplicateRecvAck = 0;
                    t.sendSeqNum = t.recvedAckNum;
                    t.sendBuf.interrupt();
                }
                else
                {
                    t.timeoutCount = 0;
                }
            }
            else
                t.timerCV.wait(lk);
        }
        //puts("*");
    }
    void TcpClient::ackThreadFun(TcpClient &t)
    {
        unique_lock<mutex> lk(t.sendBuf.mutx);
        while (t.state != Closed)
        {
            if (t.ackThreadActive)
            {
                t.sendPacket(0, 0, t.sendSeg.seqNum);
                t.ackCV.wait_for(lk, ACK_DELAY);
            }
            else
            {
                if (!t.quiet)
                    puts("ACK thread IDLE");
                t.ackCV.wait(lk);
            }
            t.ackThreadActive = t.ackSent != t.ackNum;
        }
    }

    // caller must obtain 'sendBuf.mutx'
    bool TcpClient::sendPacket(char *data, uint len, uint seq)
    {
        uint segSize = len + TCP_HEADER_LENGTH * 4;
        copy(data, data + len, sendSeg.data);
        sendSeg.seqNum = seq;
        sendSeg.ackNum = ackNum;
        sendSeg.makeChecksum(segSize);
        ackSent = ackNum;
        if (verbose)
            printf("cwnd = %u, thrsshold = %u\n", sendBuf.cwnd / MDS, ssthresh / MDS);
        if (simulatePacketLost && (int)uintRand() < 0)
        {
            if (len)
                puts(YELLOW "Simulate Packet Lost" NONE);
            else if (sendSeg.isFIN())
                puts(YELLOW "Simulate FIN Lost" NONE);
            else
                puts(YELLOW "Simulate ACK Lost" NONE);
            return true;
        }
        if (sendSeg.isFIN())
        {
            puts("FIN");
        }
        else
        {
            if (len)
            {
                if (verbose)
                    printf("Send packet. DataLen: %u, Seq: %x, Ack: %x\n", len, seq, ackNum);
            }
            else
            {
                if (!quiet)
                    printf("Send ACK: %x\n", ackNum);
            }
        }
        return sendto(sockfd, &sendSeg, segSize, 0, (sockaddr *)&oppoAddr, sizeof(oppoAddr)) != -1;
    }

    void Tcp1toManyServer::run()
    {
        int sockfd;           /* file description into transport */
        sockaddr_in myaddr;   /* address of this service */
        sockaddr_in src_addr; /* address of client    */
        TcpSegment seg;
        uint addrSize;

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        {
            perror("socket failed");
            exit(1);
        }
        bzero((char *)&myaddr, sizeof(myaddr));
        myaddr.sin_family = AF_INET;

        if (bind(sockfd, (sockaddr *)&myaddr, sizeof(myaddr)) < 0)
        {
            perror("bind failed\n");
            exit(1);
        }

        uint resultLen = sizeof(myaddr);
        if (getsockname(sockfd, (sockaddr *)&myaddr, &resultLen))
        {
            perror("getsockname error");
            exit(1);
        }

        printf("Server running at ");
        printAddr(myaddr);
        putchar('\n');

        HandlerThread::start();

        while (1)
        {
            addrSize = sizeof(src_addr);
            ssize_t nRecved = recvfrom(sockfd, &seg, MSS, 0, (sockaddr *)&src_addr, &addrSize);
            if (nRecved < 0)
            {
                perror("recvfrom error");
                break;
            }
            //printf("recved %ld byte\n", nRecved);
            if (nRecved < TCP_HEADER_LENGTH * 4)
            {
                puts("Packet too short");
                continue;
            }
            if (!seg.verifyChecksum(nRecved))
            {
                puts("Packet checksum invalid");
                continue;
            }
            auto key = getKey(src_addr);
            auto iter = serverMap.find(key);
            TcpChildServer *childServer;
            if (iter == serverMap.end())
            {
                childServer = serverMap[key] = new TcpChildServer;
                childServer->oppoAddr = src_addr;
                childServer->sockfd = sockfd;
                childServer->parentServer = this;
            }
            else
            {
                childServer = iter->second;
            }
            childServer->processIncomingPacket(nRecved, seg);
        }
        HandlerThread::stop();
    }
    void Tcp1toManyServer::childServerClosed(TcpChildServer *childServer)
    {
        serverMap.erase(getKey(childServer->oppoAddr));
        if (onCloseChildServer)
            onCloseChildServer(*childServer);
        delete childServer;
    }
    uint64_t Tcp1toManyServer::getKey(sockaddr_in &addr)
    {
        uint64_t t;
        t = ((uint64_t)addr.sin_addr.s_addr) << 32;
        t |= addr.sin_port;
        return t;
    }

    // 'seg' must have had its Checksum verified.
    // 'oppoAddr' must have been set correctly
    void TcpChildServer::processIncomingPacket(ssize_t nRecved, TcpSegment &recvedSeg)
    {
        switch (serverState)
        {
        case InitialState:
            if (!recvedSeg.isSYN())
                return;
            puts("SYN");

            sendSeg.srcPort = addr.sin_port = htons(0);
            sendSeg.destPort = oppoAddr.sin_port;
            sendSeqNum = 1 + (sendSeg.seqNum = uintRand());
            ackNum = sendSeg.ackNum = recvedSeg.seqNum + 1;
            //printf("%u\n", sendSeg.ackNum);
            sendSeg.reset_HeaderLen_Flags(TcpSegment::SYN_FLAG | TcpSegment::ACK_FLAG);
            sendSeg.makeChecksum(TCP_HEADER_LENGTH * 4);
            if (sendto(sockfd, &sendSeg, TCP_HEADER_LENGTH * 4, 0, (sockaddr *)&oppoAddr, sizeof(oppoAddr)) == -1)
            {
                perror("send SYN+ACK error");
                exit(1);
            }
            puts("SYN ACK");
            sendSeg.reset_HeaderLen_Flags(TcpSegment::ACK_FLAG);
            serverState = WaitForFirstAck;
            break;

        case WaitForFirstAck:
            if (!recvedSeg.isACK() || recvedSeg.ackNum != sendSeqNum)
                return;
            puts("    ACK");
            startConnectedState();
            if (parentServer->onChildServerConnected)
                parentServer->onChildServerConnected(*this);
            serverState = Connected;
            break;

        case Connected:
            if (recvedSeg.isFIN())
            {
                //puts("%");
                close();
                return;
            }
            if (!recvedSeg.isACK() || recvedSeg.isSYN())
                return;
            processIncomingDataPacket(nRecved, recvedSeg);
            break;
        }
    }
    class CloseTcpChildServerRunnable : public Runnable
    {
    public:
        TcpChildServer *cs;
        CloseTcpChildServerRunnable(TcpChildServer *cs) : cs(cs) {}
        virtual void run()
        {
            cs->_close();
        };
    };
    void TcpChildServer::close()
    {
        if (!setClosedState())
            return;
        parentServer->runOnHandlerThread(new CloseTcpChildServerRunnable(this));
    }
    void TcpChildServer::_close()
    {
        serverState = Closed;
        //TcpClient::onFinSent = (void (*)(TcpClient &))onFinSent;
        closeThreads();
        parentServer->childServerClosed(this);
    }

    //void TcpChildServer::onFinSent(TcpChildServer &tcpChildServer) { }
} // namespace tcp