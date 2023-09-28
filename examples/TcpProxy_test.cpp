/*
 * TcpProxy_test.cpp
 *
 * @build   make evpp
 * @server  bin/TcpProxy_test 1234
 *
 */

#include <iostream>
#include "hstring.h"
#include "TcpServer.h"
#include <string>
using namespace hv;

#define TEST_TLS        0
struct Session {
    uint32_t pid = 0;
    bool serv = false;
    std::string name;
};

std::string listServer(TcpServer& srv, int filter) {
    std::ostringstream stm;
    srv.foreachChannel([&stm, filter](const SocketChannelPtr& ch) {
        auto session = ch->getContextPtr<Session>();
        if (session && session->serv) {
            if (filter && session->pid)
                return;
            stm << ch->id() << "-" << session->pid
                << " " << session->name 
                << " " << (session->serv ? "server" : "client")
                << "\n";
        }
    });
    return stm.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s port\n", argv[0]);
        return -10;
    }
    int port = atoi(argv[1]);

    hlog_set_level(LOG_LEVEL_DEBUG);

    TcpServer srv;
    int listenfd = srv.createsocket(port);
    if (listenfd < 0) {
        return -20;
    }
    printf("server listen on port %d, listenfd=%d ...\n", port, listenfd);
    srv.onConnection = [&srv](const SocketChannelPtr& channel) {
        std::string peeraddr = channel->peeraddr();
        if (channel->isConnected()) {
            channel->newContextPtr<Session>();
            hlogi("%s connected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
        } else {
            hlogi("%s disconnected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
            auto session = channel->getContextPtr<Session>();
            SocketChannelPtr peer = srv.getChannelById(session->pid);
            if (!peer) return;
            if (session->serv) {
                peer->close();
                //srv.removeChannel(peer);
            }
            else {
                peer->getContextPtr<Session>()->pid = 0;
            }
        }
    };
    srv.onMessage = [&srv](const SocketChannelPtr& channel, Buffer* buf) {
        auto session = channel->getContextPtr<Session>();
        if (!session->pid) {
            printf("%s> %.*s\n", channel->peeraddr().c_str(), (int)buf->size(), (char*)buf->data());
            // 处理CMD请求
            auto lst = hv::split((const char*)buf->data(), ' ');
            if (lst.size() < 2) {
                return;
            }
            std::string cmd = lst[0];
            if (cmd == "serv") { // 注册代理
                session->serv = true;
                session->name = lst[1];
                hlogi("regist serv %s", lst[1].c_str());
                channel->setUnpack(nullptr);
            }
            else if(cmd == "conn") { // 连接代理
                auto name = lst[1];
                SocketChannelPtr server;
                srv.foreachChannel([name, &server](const SocketChannelPtr& channel){
                    auto session = channel->getContextPtr<Session>();
                    if (session && session->serv && session->name == name) {
                        server = channel;
                    }
                });
                if (server) {
                    hlogi("connect serv %s", lst[1].c_str());
                    auto peer = server->getContextPtr<Session>();
                    peer->pid = channel->id();
                    session->pid = server->id();
                    channel->setUnpack(nullptr);
                }
                else {
                    std::string error = "error sever " + name + " not found";
                    hlogi("%s", error.c_str());
                    channel->write(error);
                }
            }
            else if(cmd == "list") { // 列出代理状态
                channel->write(listServer(srv, std::stoi(lst[1])));
            }
        }
        else {
            SocketChannelPtr peer = srv.getChannelById(session->pid);
            if (peer)
                peer->write(buf);
            else
                hlogw("without peer %d ignore %d message", session->pid, (int)buf->size());
        }
        
    };
    srv.setThreadNum(4);
    srv.setLoadBalance(LB_LeastConnections);
    
    // 每次接收一以0结尾的字符串
    unpack_setting_t setting;
    memset(&setting, 0, sizeof(setting));
    setting.mode = UNPACK_BY_DELIMITER;
    setting.delimiter_bytes = 1;
    setting.delimiter[0] = '\n';
    srv.setUnpack(&setting);

#if TEST_TLS
    hssl_ctx_opt_t ssl_opt;
    memset(&ssl_opt, 0, sizeof(hssl_ctx_opt_t));
    ssl_opt.crt_file = "cert/server.crt";
    ssl_opt.key_file = "cert/server.key";
    ssl_opt.verify_peer = 0;
    srv.withTLS(&ssl_opt);
#endif

    srv.start();

    std::string str;
    while (std::getline(std::cin, str)) {
        if (str == "close") {
            srv.closesocket();
        } else if (str == "start") {
            srv.start();
        } else if (str == "stop") {
            srv.stop();
            break;
        } else if (str == "list") {
            printf("%s\n", listServer(srv, 0).c_str());
        } else {
            srv.broadcast(str.data(), str.size());
        }
    }

    return 0;
}
