#include "uv.h"
#include "util.h"
#include "utilc.h"
#include "worker.h"
#include <map>

using namespace Server;

#define PACK_MAX_SIZE        1700                // rtp����󳤶�

namespace RtpDecode {
    struct Sequence {
        uint16_t seq;
        Sequence():seq(0){}
        Sequence(uint16_t& s):seq(s){}

        bool operator==(const Sequence& rs) const {
            return this->seq == rs.seq;
        }
        bool operator<(const Sequence& rs) const {
            if (seq == rs.seq) {
                return false;
            } else if (seq < rs.seq) {
                if(rs.seq - seq > 32767)
                    return false;
                else
                    return true;
            } else if (seq > rs.seq) {
                if(seq - rs.seq > 32767)
                    return true;
                else
                    return false;
            }
            return false;
        } 
        bool operator>(const Sequence& rs) const {
            return rs < *this;
        }
        bool operator<=(const Sequence& rs) const {
            return *this<rs || *this==rs;
        }
        bool operator>=(const Sequence& rs) const {
            return *this>rs || *this==rs;
        }
    };

    class CRtpPacket {
    public:
        uint8_t     m_nType;    //���� 0rtp 1ps
        char       *m_pData;   //����
        uint32_t    m_nLen;    //����
        //CRtpPacket *m_pPre;
        //CRtpPacket *m_pNext;
        //rtpͷ��Ϣ
        uint8_t v;  	 /* packet type */
        uint8_t p;  	 /* padding flag */
        uint8_t x;  	 /* header extension flag */
        uint8_t cc; 	 /* CSRC count */
        uint8_t m;	     /* marker bit */
        uint8_t pt;  	 /* payload type */
        uint16_t seq;	 /* sequence number */
        uint32_t ts;	 /* timestamp */
        uint32_t ssrc;	 /* synchronization source */
        //����rtpͷ
        uint32_t m_nHeaderLen;     //ͷ����
        uint32_t m_nPlayloadLen;   //�غɳ���
        bool     m_bIsPsHeader;    //�غ������Ƿ���PSͷ
        //ps��Ϣ
        uint16_t seqBegin;
        uint16_t seqEnd;

        CRtpPacket();
        ~CRtpPacket();
        bool Parse(char *buf, uint32_t size);
    };

    class CRtpStream {
    public:
        uv_loop_t   m_uvLoop;
        uv_async_t  m_uvAsync;
        uv_udp_t    m_uvSkt;             // rtp����
        uv_timer_t  m_uvTimer;           // ���ճ�ʱ��ʱ��
        string      m_strRemoteIP;       // ���ͷ�IP
        int         m_nRemotePort;       // ���ͷ��˿�
        int         m_nPort;             // ���ն˿�
        void       *m_pUser;             // �û�����
        bool        m_bRun;
        int         m_nHandleNum;        // ������uv���������Щ���ͨ��uv_close�ر�

        //CRtpPacket *m_listHead;
        //CRtpPacket *m_listTail;
        std::map<Sequence, CRtpPacket *>
                    m_PacketList;
        uint16_t    m_nDoneSeq;          // �����֡�����кţ�С�ڸ�ֵ�İ�����
        bool        m_bDoneFirst;        // ������һ֡

        CRtpStream(void* usr, uint32_t port, string remoteIP, uint32_t remotePort);

        void AddPacket(char* data, uint32_t len);
    };

    //////////////////////////////////////////////////////////////////////////

    CRtpPacket::CRtpPacket()
        : m_nType(0)
        , m_pData(NULL)
        , m_nLen(0)
        //, m_pPre(NULL)
        //, m_pNext(NULL)
        , m_bIsPsHeader(false)
    {}

    CRtpPacket::~CRtpPacket() {
        if(m_nType && m_pData)
            free(m_pData);
    }

    bool CRtpPacket::Parse(char *buf, uint32_t size) {
        m_pData = buf;
        m_nLen = size;

        if(m_nLen < 12)
            return false;

        /** ����rtpͷ */
        v = m_pData[0]>6;
        p = (m_pData[0]>5)&0X1;
        x = (m_pData[0]>4)&0X1;
        cc = m_pData[0]&0XF;
        m = m_pData[1]>7;
        pt = m_pData[1]&0X8F;
        seq = m_pData[2];
        seq <<= 8;
        seq |= m_pData[3];
        ts = m_pData[4];
        ts <<= 8;
        ts |= m_pData[5];
        ts <<= 8;
        ts |= m_pData[6];
        ts <<= 8;
        ts |= m_pData[7];
        ssrc = m_pData[8];
        ssrc <<= 8;
        ssrc |= m_pData[9];
        ssrc <<= 8;
        ssrc |= m_pData[10];
        ssrc <<= 8;
        ssrc |= m_pData[11];

        seqBegin = seqEnd = seq;

        if(v != 2)
            return false;

        // ����ͷ���غɵĳ���
        m_nHeaderLen = 12;
        m_nPlayloadLen = m_nLen - 12;
        if(cc) {
            uint32_t ccLen = 4 * cc;
            if(m_nPlayloadLen < ccLen)
                return false;
            m_nHeaderLen += ccLen;
            m_nPlayloadLen -= ccLen;
        }
        if(x) {
            if(m_nPlayloadLen < 4)
                return false;
            uint32_t xLen = m_pData[m_nHeaderLen];
            xLen <<= 8 ;
            xLen |= m_pData[m_nHeaderLen + 1] ;
            xLen *= 4 ;
            xLen += 4;
            if(m_nPlayloadLen < xLen)
                return false;
            m_nHeaderLen += xLen;
            m_nPlayloadLen -= xLen;
        }
        if(p) {
            if(m_nLen == 0)
                return false;
            uint32_t Padding = m_pData[m_nLen - 1] ;
            if ( m_nPlayloadLen < Padding )
                return false;
            m_nPlayloadLen -= Padding ;
        }

        //�ж��غ��Ƿ�ΪPSͷ
        if(m_nPlayloadLen > 4) {
            char *platload = m_pData + m_nHeaderLen;
            if (platload[0] == 0 && platload[1] == 0 && platload[2] == 1 && platload[3] == 0xBA)
                m_bIsPsHeader = true;
        }
        return true;
    }

    //////////////////////////////////////////////////////////////////////////
    /** udp�������뻺��ռ� */
    static void echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        *buf = uv_buf_init((char*)malloc(PACK_MAX_SIZE), PACK_MAX_SIZE);
    }

    /** udp�������ݣ���ȡһ���� */
    static void after_read(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
        CRtpStream *dec = (CRtpStream*)handle->data;
        if(nread < 0){
            Log::error("read error: %s",uv_strerror(nread));
            free(buf->base);
        }
        if(nread == 0)
            return;

        //udp��Դ��ƥ�䣬����������
        struct sockaddr_in* addr_in =(struct sockaddr_in*)addr;
        int port = ntohs(addr_in->sin_port);
        char ipv4addr[64]={0};
        uv_ip4_name(addr_in, ipv4addr, 64);
        string ip = ipv4addr;
        if(dec->m_nRemotePort != port || dec->m_strRemoteIP != ip) {
            //Log::error("this is not my rtp data");
            return;
        }

        //���ó�ʱ��ʱ��
        uv_timer_again(&dec->m_uvTimer);

        dec->AddPacket(buf->base, nread);
    }

    /** ��ʱ��ʱ����ʱ�ص� */
    static void timer_cb(uv_timer_t* handle) {
        CRtpStream *dec = (CRtpStream*)handle->data;
        CLiveWorker* pLive = (CLiveWorker*)dec->m_pUser;
        //pLive->RtpOverTime();
    }

    /** �ر�uv����Ļص� */
    static void udp_uv_close_cb(uv_handle_t* handle){
        CRtpStream *dec = (CRtpStream*)handle->data;
        dec->m_nHandleNum--;
        //uv���ȫ���رպ�ֹͣloop
        if(!dec->m_nHandleNum){
            dec->m_bRun = false;
            uv_stop(&dec->m_uvLoop);
        }
    }

    /** �ⲿ�߳�֪ͨ�ص�����loop�ص��йر��õ���uv��� */
    static void async_cb(uv_async_t* handle){
        CRtpStream *dec = (CRtpStream*)handle->data;
        int ret = uv_udp_recv_stop(&dec->m_uvSkt);
        if(ret < 0) {
            Log::error("stop rtp recv port:%d err: %s", dec->m_nPort, uv_strerror(ret));
        }
        uv_close((uv_handle_t*)&dec->m_uvSkt, udp_uv_close_cb);

        ret = uv_timer_stop(&dec->m_uvTimer);
        if(ret < 0) {
            Log::error("stop timer error: %s",uv_strerror(ret));
        }
        uv_close((uv_handle_t*)&dec->m_uvTimer, udp_uv_close_cb);

        uv_close((uv_handle_t*)&dec->m_uvAsync, udp_uv_close_cb);
    }

    /** event loop thread */
    static void run_loop_thread(void* arg) {
        CRtpStream *dec = (CRtpStream*)arg;
        while (dec->m_bRun) {
            uv_run(&dec->m_uvLoop, UV_RUN_DEFAULT);
            Sleep(20);
        }
        uv_loop_close(&dec->m_uvLoop);
        delete dec;
    }


    CRtpStream::CRtpStream(void* usr, uint32_t port, string remoteIP, uint32_t remotePort)
        : m_pUser(usr)
        , m_nPort(port)
        , m_strRemoteIP(remoteIP)
        , m_nRemotePort(remotePort)
        , m_bRun(true)
        , m_nHandleNum(0) 
        //, m_listHead(NULL)
        //, m_listTail(NULL)
        , m_nDoneSeq(0)
        , m_bDoneFirst(false)
    {
        uv_loop_init(&m_uvLoop);

        //udp����
        m_uvSkt.data = (void*)this;
        uv_udp_init(&m_uvLoop, &m_uvSkt);
        struct sockaddr_in addr;
        uv_ip4_addr("0.0.0.0", m_nPort, &addr);
        uv_udp_bind(&m_uvSkt, (struct sockaddr*)&addr, 0);
        int nRecvBuf = 10 * 1024 * 1024;       // ���������ó�10M��Ĭ��ֵ̫С�ᶪ��
        setsockopt(m_uvSkt.socket, SOL_SOCKET, SO_RCVBUF, (char*)&nRecvBuf, sizeof(nRecvBuf));
        int nOverTime = 30*1000;  // ��ʱʱ�����ó�30s
        setsockopt(m_uvSkt.socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nOverTime, sizeof(nOverTime));
        setsockopt(m_uvSkt.socket, SOL_SOCKET, SO_SNDTIMEO, (char*)&nOverTime, sizeof(nOverTime));
        uv_udp_recv_start(&m_uvSkt, echo_alloc, after_read);
        m_nHandleNum++;

        //����udp���ճ�ʱ�ж�
        m_uvTimer.data = (void*)this;
        uv_timer_init(&m_uvLoop, &m_uvTimer);
        uv_timer_start(&m_uvTimer, timer_cb, 30000, 30000);
        m_nHandleNum++;

        //�첽����
        m_uvAsync.data = (void*)this;
        uv_async_init(&m_uvLoop, &m_uvAsync, async_cb);
        m_nHandleNum++;

        //udp ����loop�߳�
        uv_thread_t tid;
        uv_thread_create(&tid, run_loop_thread, this);
    };

    void CRtpStream::AddPacket(char* data, uint32_t len) {
        CRtpPacket *pack = new CRtpPacket();
        if(!pack->Parse(data, len))  {
            Log::error("parse header failed");
            delete pack;
            return;
        }

        Sequence thisSeq(pack->seq);

        // �������������
        Sequence doneSeq(m_nDoneSeq);
        if(m_bDoneFirst && thisSeq < doneSeq) {
            Log::error("rtp sort error: seq is old seq:%d,m_nDoneSeq:%d",pack->seq, m_nDoneSeq);
            delete pack;
            return;
        }

        //���кŴ��ڣ��ظ���������
        if(m_PacketList.count(thisSeq) > 0) {
            Log::error("rtp sort error: have same seq :%d",pack->seq);
            delete pack;
            return;
        }

        m_PacketList.insert(make_pair(thisSeq, pack));

        // ��֡
        auto it_pos          = m_PacketList.begin();
        auto it_end          = m_PacketList.end();
        auto it_first        = it_pos;   //�ҵ���rtp֡��һ������psͷ
        auto it_last         = it_pos;    //rtp֡�����һ����

        pack = NULL;
        bool findFirst = false;
        bool fullframe = false;
        for (; it_pos != it_end; ++it_pos) {
            pack = it_pos->second;
            if(it_pos == it_first && pack->m_nType == 0 && !pack->m_bIsPsHeader) {
                it_first = it_pos;
                findFirst = true;
            } else {
                if(pack->m_nType == 0){
                    if(findFirst && it_last->first.seq + 1 != it_pos->first.seq) {
                        findFirst = false;
                    }
                    if(pack->m_bIsPsHeader) {
                        if(findFirst) {
                            fullframe = true;
                            it_last = it_pos;
                            break; //����ѭ��ȥ��֡
                        } else {
                            it_first = it_pos;
                            findFirst = true;
                        }
                    }
                } else {
                    // PS
                    if(findFirst && it_last->first.seq + 1 != it_pos->first.seq) {
                        findFirst = false;
                    }
                    if(findFirst) {
                        fullframe = true;
                        it_last = it_pos;
                        break; //����ѭ��ȥ��֡
                    }
                }
            }
            it_last = it_pos;
        }

        //������ɵ�֡��������֡
        if(fullframe) {
            uint32_t nPsLen = 0;
            for (it_pos = it_first; it_pos != it_last; ++it_pos) {
                nPsLen += it_pos->second->m_nPlayloadLen;
            }
            char *pPsBuff = (char*)calloc(1, nPsLen);
            CRtpPacket *ps = new CRtpPacket();
            ps->m_nType = 1;
            ps->m_pData = pPsBuff;
            ps->m_nLen = nPsLen;
            ps->seqBegin = pack->seq = it_first->second->seq;
            ps->seqEnd = it_last->second->seq-1;
            uint32_t nLen = 0;
            for (it_pos = it_first; it_pos != it_last; ++it_pos) {
                memcpy(pPsBuff+nLen, it_pos->second->m_pData+it_pos->second->m_nHeaderLen, it_pos->second->m_nPlayloadLen);
                nLen += it_pos->second->m_nPlayloadLen;
                delete it_pos->second;
                it_pos = m_PacketList.erase(it_pos);
            }
            Sequence psSeq(ps->seq);
            m_PacketList.insert(make_pair(psSeq, ps));
        }
        
        //���������rtp����������ɵ�PS
        it_pos = m_PacketList.begin();
        it_end = m_PacketList.end();
        for (; it_pos != it_end; ) {
            if(it_pos->second->m_nType == 0) {
                if(it_end->second->ts - it_pos->second->ts > 3600) {
                    delete it_pos->second;
                    it_pos = m_PacketList.erase(it_pos);
                } else {
                    break;
                }
            } else {
                // �˴�����PS
                Server::CLiveWorker* lw = (Server::CLiveWorker*)m_pUser;
                lw->push_ps_data(it_pos->second->m_pData, it_pos->second->m_nLen);
                it_pos = m_PacketList.erase(it_pos);
            }
        }
    }

    //////////////////////////////////////////////////////////////////////////

    static void ParseSdp(string sdp, string &rip, uint32_t &rport) {
        //��sdp��������ƵԴip�Ͷ˿�
        bnf_t* sdp_bnf = create_bnf(sdp.c_str(), sdp.size());
        char *sdp_line = NULL;
        char remoteIP[25]={0};
        int remotePort = 0;
        while (bnf_line(sdp_bnf, &sdp_line)) {
            if(sdp_line[0]=='c'){
                sscanf(sdp_line, "c=IN IP4 %[^/\r\n]", remoteIP);
                rip = remoteIP;
            } else if(sdp_line[0]=='m') {
                sscanf(sdp_line, "m=video %d ", &remotePort);
                rport = remotePort;
            }

            //if(sdp_line[0]=='a' && !strncmp(sdp_line, "a=rtpmap:", 9)){
            //    //a=rtpmap:96 PS/90000
            //    //a=rtpmap:96 H264/90000
            //    char tmp[256]={0};
            //    int num = 0;
            //    char type[20]={0};
            //    int bitrate = 0;
            //    sscanf(sdp_line, "a=rtpmap:%d %[^/]/%d", &num, type, &bitrate);
            //    if(!strcmp(type, "PS") || !strcmp(type, "MP2P")){
            //        m_stream_type = RTP_STREAM_PS;
            //    }else{
            //        m_stream_type = RTP_STREAM_H264;
            //    }
            //}
        }
        destory_bnf(sdp_bnf);
    }

    bool Init() {

        return true;
    }

    void Cleanup() {

    }

    void* Play(void* user, std::string sdp, uint32_t port) {
        string remoteIP;
        uint32_t remotePort;
        ParseSdp(sdp, remoteIP, remotePort);
        CRtpStream *ret = new CRtpStream(user, port, remoteIP, remotePort);
        return ret;
    }

    void Stop(void* ret) {
        CRtpStream *dec = (CRtpStream*)ret;
        uv_async_send(&dec->m_uvAsync);
    }
};