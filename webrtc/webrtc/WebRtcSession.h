/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */


#ifndef ZLMEDIAKIT_WEBRTCSESSION_H
#define ZLMEDIAKIT_WEBRTCSESSION_H

#include "Socket.h"
#include "Util/TimeTicker.h"

class WebRtcTransportImp;
class WebRtcSession : public toolkit::Session,
    public std::enable_shared_from_this<WebRtcSession> {
    std::string _identifier;
    bool _find_transport = true;
    toolkit::Ticker _ticker;
    struct sockaddr* _peer_addr;
    std::shared_ptr<WebRtcTransportImp> _transport;
public:
    static std::string getUserName(void* buf, int len);
    WebRtcSession(hio_t* io);
    std::string getIdentifier() const {
        return _identifier;
    }

    void onRecv(hv::Buffer* buf);
    void onManager();
    void onError(const toolkit::SockException &err);
};

#endif //ZLMEDIAKIT_WEBRTCSESSION_H
