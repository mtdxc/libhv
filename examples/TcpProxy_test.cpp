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
    SocketChannelPtr peer;
    bool serv = false;
    std::string name;
};

std::string listSession(TcpServer& srv, int server, int filter) {
    std::ostringstream stm;
    srv.foreachChannel([&stm, server, filter](const SocketChannelPtr& ch) {
        auto session = ch->getContextPtr<Session>();
        if (!session) return;
        if (session->serv != server) return;
        if (filter && session->peer) return;
        stm << (session->serv ? "server" : "client") << " " << ch->peeraddr() 
            << " " << ch->id() << "-" << (session->peer ? session->peer->id() : 0) 
            << " " << session->name 
            << "\r\n";
    });
    return stm.str();
}

std::string listClient(TcpServer& srv, int filter) {
    return listSession(srv, false, filter);
}
std::string listServer(TcpServer& srv, int filter) {
    return listSession(srv, true, filter);
}

int writeChannel(const SocketChannelPtr& channel, const std::string& msg) {
    return channel->write(msg.c_str(), msg.length() + 1);
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
    srv.onConnection = [](const SocketChannelPtr& channel) {
        std::string peeraddr = channel->peeraddr();
        if (channel->isConnected()) {
            channel->newContextPtr<Session>();
            hlogi("%s connected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
        } else {
            hlogi("%s disconnected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
            auto session = channel->getContextPtr<Session>();
            SocketChannelPtr peer = session->peer;
            if (!peer) return;
            if (session->serv) {
                hlogi("close client %d for server %s disconnect", peer->id(), session->name.c_str());
                peer->close();
                //srv.removeChannel(peer);
            }
            else {
                auto server = peer->getContextPtr<Session>();
                if (server->peer == channel) {
                    hlogi("reset server %s peer due to client %d disconnet", server->name.c_str(), channel->id());
                    server->peer = nullptr;
                }
            }
        }
    };
    srv.onMessage = [&srv](const SocketChannelPtr& channel, Buffer* buf) {
        auto session = channel->getContextPtr<Session>();
        if (session->peer) {
            session->peer->write(buf);
            return;
        }

        printf("%s> %.*s\n", channel->peeraddr().c_str(), (int)buf->size(), (char*)buf->data());
        // 处理CMD请求
        auto lst = hv::split(hv::trim((const char*)buf->data()), ' ');
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
                if (peer->peer) 
                    peer->peer->close();
                peer->peer = channel;
                session->peer = server;
                channel->setUnpack(nullptr);
            }
            else {
                std::string error = "error sever " + name + " not found";
                hlogi("%s", error.c_str());
                writeChannel(channel, error);
            }
        }
        else if(cmd == "list") { // 列出代理状态
            std::string resp = listServer(srv, std::stoi(lst[1]));
            writeChannel(channel, resp);
        }        
    };
    //srv.setThreadNum(4);
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
        auto lst = hv::split(hv::trim(str), ' ');
        if (lst.size())
            str = lst[0];
        if (str == "close") {
            srv.closesocket();
        } else if (str == "start") {
            srv.start();
        } else if (str == "stop") {
            srv.stop();
            break;
        } 
        else if (str == "listS" || str == "ls") {
            printf("%s\n", listSession(srv, true, 0).c_str());
        } 
        else if (str == "listC" || str == "lc") {
            printf("%s\n", listSession(srv, false, 0).c_str());
        }
        else if (str == "kill") {   
            for (size_t i = 1; i < lst.size(); i++)
            {
                auto channel = srv.getChannelById(std::stoi(lst[i]));
                if (channel)
                    channel->close();
            }
            
        }
        else {
            srv.broadcast(str.data(), str.size());
        }
    }

    return 0;
}
