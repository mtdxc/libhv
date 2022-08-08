#ifndef ZLMEDIAKIT_SRT_SESSION_H
#define ZLMEDIAKIT_SRT_SESSION_H

#include "Socket.h"
#include "Util/TimeTicker.h"
#include "Buffer.hpp"
namespace SRT {

using namespace toolkit;
class SrtTransport;

class SrtSession : public toolkit::Session, 
    public std::enable_shared_from_this<SrtSession> {
public:
    typedef std::shared_ptr<SrtSession> Ptr;

    SrtSession(hio_t* io);
    ~SrtSession() override;

    void onRecv(uint8_t *data, size_t size);
    void onError(const SockException &err);
    void onManager();
    static EventPoller::Ptr queryPoller(uint8_t *data, size_t size);
private:
    bool _find_transport = true;
    Ticker _ticker;
    struct sockaddr_storage* _peer_addr;
    std::shared_ptr<SrtTransport> _transport;
};

} // namespace SRT
#endif // ZLMEDIAKIT_SRT_SESSION_H