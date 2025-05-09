/*
 * websocket server
 *
 * @build   make examples
 * @server  bin/websocket_server_test 9999
 * @client  bin/websocket_client_test ws://127.0.0.1:9999/
 * @python  scripts/websocket_server.py
 * @js      html/websocket_client.html
 *
 */

#include "WebSocketServer.h"
#include "EventLoop.h"
#include "htime.h"
#include "hlog.h"
using namespace hv;

/*
 * #define TEST_WSS 1
 *
 * @build   ./configure --with-openssl && make clean && make
 *
 * @server  bin/websocket_server_test 9999
 *
 * @client  bin/websocket_client_test ws://127.0.0.1:9999/
 *          bin/websocket_client_test wss://127.0.0.1:10000/
 *
 */
#define TEST_WSS 0

using namespace hv;

// 计算 MP3 帧大小
static int calculate_mp3_frame_size(const uint8_t* header, int& sampling_rate) {
    // 参考MPEG音频帧头格式
    int version = (header[1] >> 3) & 0x03;
    int layer = (header[1] >> 1) & 0x03;
    int bitrate_index = (header[2] >> 4) & 0x0F;
    int sampling_rate_index = (header[2] >> 2) & 0x03;
    int padding = (header[2] >> 1) & 0x01;
    
    if (version == 0 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 ||
        sampling_rate_index == 3) {
        return -1; // 无效帧头
    }
    
    // 简化的帧大小计算（适用于Layer III）
    static const int bitrates[][16] = {
        // MPEG Version 1
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
        // MPEG Version 2/2.5
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
    };
    
    static const int sampling_rates[][4] = {
        {44100, 48000, 32000, 0}, // MPEG 1
        {22050, 24000, 16000, 0}, // MPEG 2
        {11025, 12000, 8000, 0}   // MPEG 2.5
    };
    
    int bitrate = bitrates[version == 3 ? 0 : 1][bitrate_index] * 1000;
    sampling_rate = sampling_rates[version == 3 ? 0 : (version == 2 ? 1 : 2)][sampling_rate_index];
    
    if (bitrate == 0 || sampling_rate == 0) return -1;
    
    // 帧大小公式: ((144 * bitrate) / sampling_rate) + padding
    return ((144 * bitrate) / sampling_rate) + padding;
}

bool read_mp3_frame(FILE* fp, std::string& frame, int& samplerate) {
    unsigned char header[4];
    while (fread(header, 1, 4, fp) == 4) {
        if (header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) {
            // 找到MP3帧头
            // 这里可以解析帧头信息或处理帧数据
            
            // 计算帧大小（简化版）
            int frame_size = calculate_mp3_frame_size(header, samplerate);
            if (frame_size > 0) {
                frame.resize(frame_size);
                memcpy(&frame[0], header, 4);
                fread(&frame[4], 1, frame_size - 4, fp);
                return true;
            }
        }
        else if(header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
            // ID3v2 标签，跳过
            char tag_header[6];
            fread(tag_header, 1, 6, fp);
            int tag_size = ((tag_header[2] & 0x7F) << 21) | ((tag_header[3] & 0x7F) << 14) |
                           ((tag_header[4] & 0x7F) << 7) | (tag_header[5] & 0x7F);
            printf("read ID3v2 tag size: %d\n", tag_size);
            frame.resize(tag_size + 10);
            memcpy(&frame[0], header, 4);
            memcpy(&frame[4], tag_header, 6);
            fread(&frame[10], tag_size, 1, fp);
            continue;
        }
        // 不是帧头，回退3字节继续查找
        fseek(fp, -3, SEEK_CUR);
    }
    return false;
}
struct tagheader {
  char ID[3];          // The first 4 bytes should be ID3
  char version[2];     // $03 00
  char flags;          // $abc00000 : a:unsynchronisation if set; b:extended header exist if set; c:experimental indicator if set
  char size[4];        // (total tag size - 10) excluding the tagheader;
};

struct frameheader {
  char frameid[4];    // TIT2 MCDI TRCK ...
  char size[4];
  char flags[2];      // %abc00000  %ijk00000 | a 0:frame should be preserved 1:frame should be discard
};

#define ADTS_HEADER_SIZE 7
bool read_aac_frame(FILE* fp, std::string& frame, int& sample_rate) {
    static const int gAacSampleMap[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                    24000, 22050, 16000, 12000, 11025, 8000,
                                    7350,  0,     0,     0};
    uint8_t header[ADTS_HEADER_SIZE];
    while (fread(header, 1, ADTS_HEADER_SIZE, fp) == ADTS_HEADER_SIZE) {
        if (header[0] == 0xFF && (header[1] & 0xF0) == 0xF0) {
            // 找到ADTS帧头
            int frame_size = (header[3] & 0x01) << 11 | header[4] << 3 | header[5] >> 5;
            frame.resize(frame_size);
            memcpy(&frame[0], header, ADTS_HEADER_SIZE);
            sample_rate = gAacSampleMap[header[2] >> 2 & 0x0F];
            int channels = (header[2] & 0x01) << 2 | header[3] >> 6;
            int block_count = (header[6] & 0x03) + 1; // 每帧包含的 AAC 数据块数目，通常为 0（表示只有 1 个数据块）。
            // cur_ += 1024.0 * block_count / sample_rate; 
            if (fread(&frame[ADTS_HEADER_SIZE], frame_size - ADTS_HEADER_SIZE, 1, fp)) {
                return true;
            }
        }
        // 不是ADTS帧头，回退3字节继续查找
        fseek(fp, 1-ADTS_HEADER_SIZE, SEEK_CUR);
    }
    return false;
}

class MyContext {
    hv::WebSocketChannel* sock_ = nullptr;
    // MP3帧头通常是4字节，以0xFF开头
    FILE* mp3File = nullptr;
    int frame_count = 2;
public:
    MyContext(hv::WebSocketChannel* s) : sock_(s) {
        timerID = INVALID_TIMER_ID;
    }
    ~MyContext() {
        closeFile();
        killTimer();
    }
    void killTimer() {
        if (timerID != INVALID_TIMER_ID) {
            ::killTimer(timerID);
            timerID = INVALID_TIMER_ID;
        }
    }
    void closeFile() {
        if (fp_) {
            fclose(fp_);
            fp_ = nullptr;
        }
        if (mp3File) {
            fclose(mp3File);
            mp3File = nullptr;
        }
    }

    void handleMessage(const std::string& msg, enum ws_opcode opcode) {
        if (opcode == WS_OPCODE_TEXT) {
            printf("onText: %.*s\n", (int)msg.size(), msg.data());
            auto args = hv::split(msg, ' ');
            if (args[0] == "start") {
                killTimer();
                if (mp3File) {
                    fclose(mp3File);
                    mp3File = nullptr;
                }
                const char* path = "resp.mp3";
                if (args.size() > 1) {
                    frame_count = atoi(args[1].c_str());
                }
                if (args.size() > 2) {
                    path = args[2].c_str();
                }

                char buff[256];
                mp3File = fopen(path, "rb");
                if (!mp3File) {
                    sprintf(buff, "unable to open file %s", path);
                    printf("%s\n", buff);
                    sock_->send(buff);
                    return;
                }
                std::string data;
                int samplerate = 0;
                if (!read_mp3_frame(mp3File, data, samplerate)) {
                    sprintf(buff, "invalid file %s", path);
                    printf("%s\n", buff);
                    sock_->send(buff);
                    return;
                }

                sprintf(buff, "start %s %d %d", path, samplerate, frame_count);
                printf("%s\n", buff);
                sock_->send(buff);

                // 开始发送mp3
                timerID = setInterval(26 * frame_count, [this](TimerID id) {
                    if (sock_->isConnected() && sock_->isWriteComplete() && mp3File) {
                        std::string resp;
                        int samplerate = 0;
                        for (int i=0;i<frame_count;i++) {
                            std::string frame;
                            if (!read_mp3_frame(mp3File, frame, samplerate)) {
                                printf("read mp3 eof rewind\n");
                                fseek(mp3File, 0, SEEK_SET);
                                break;
                            }
                            resp.append(frame);
                        }
                        sock_->send(resp, WS_OPCODE_BINARY, true);
                    }
                });
            }
            // 停止接收mp3
            else if (msg == "stop") {
                killTimer();
            }
            else if(-1!=msg.find("hello")) {
                sock_->send(msg, opcode);
            }
        }
        else{
            printf("onBin: %d\n", (int)msg.size());
            // 回显mp3
            sock_->send(msg, opcode);
            return ;
            if (msg.size() < 1) {
                closeFile();
                char buff[128];
                int n = snprintf(buff, sizeof(buff), "recv %d samples\n", binCount);
                sock_->send(buff, n, WS_OPCODE_TEXT);
                printf("%s", buff);
                binCount = 0;
                return;
            }
            // 追加文件
            if (!fp_) {
                char buff[256];
                sprintf(buff, "%d.mp3", count++);
                fp_ = fopen(buff, "wb");
            }
            fwrite(msg.data(), msg.size(), 1, fp_);
            binCount++;
        }
    }
    int count = 0;
    FILE* fp_ = nullptr;
    int binCount = 0;
    TimerID timerID = 0;
};

int main(int argc, char** argv) {
    int port = 8000;
    if (argc < 2) {
        printf("Usage: %s port\n", argv[0]);
        // return -10;
    }
    else {
        port = atoi(argv[1]);
    }
    hlog_set_handler(stdout_logger);

    HttpService http;
    http.GET("/ping", [](const HttpContextPtr& ctx) {
        return ctx->send("pong");
    });
    http.document_root = "html";
    http.index_of = "/";

    WebSocketService ws;
    // ws.setPingInterval(10000);
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        printf("onopen: GET %s\n", req->Path().c_str());
        channel->setContextPtr(std::make_shared<MyContext>(channel.get()));
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        ctx->handleMessage(msg, channel->opcode);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        channel->deleteContextPtr();
    };

    WebSocketServer server;
    server.port = port;
    server.worker_processes = 0;
    server.worker_threads = 1;
#if TEST_WSS
    server.https_port = port + 1;
    hssl_ctx_opt_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = "cert/server.crt";
    param.key_file = "cert/server.key";
    param.endpoint = HSSL_SERVER;
    if (server.newSslCtx(&param) != 0) {
        fprintf(stderr, "new SSL_CTX failed!\n");
        return -20;
    }
#endif
    server.registerHttpService(&http);
    server.registerWebSocketService(&ws);

    server.start();

    // press Enter to stop
    while (getchar() != '\n');
    return 0;
}
