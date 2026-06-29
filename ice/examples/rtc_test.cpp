// WebRtcTransport test: two endpoints (offerer + answerer) negotiate
// ICE + DTLS + SRTP on loopback and exchange encrypted RTP packets.
//
// Usage:
//   rtc_test              # run loopback test
//   rtc_test --log        # also dump logs to stdout
//
// The two endpoints share the same process; SDP and remote candidates are
// passed via in-memory strings, mimicking what a real signaling channel
// would do.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>

#include "rtc/RtcTransportImp.hpp"
#include "hlog.h"
#include "hstring.h"

using namespace ice;

namespace {

std::vector<uint8_t> makeRtpPacket(const std::string& payload,
                                   uint8_t payloadType,
                                   uint16_t sequenceNumber,
                                   uint32_t timestamp,
                                   uint32_t ssrc) {
    std::vector<uint8_t> packet(12 + payload.size());

    packet[0] = 0x80;  // V=2, P=0, X=0, CC=0
    packet[1] = payloadType & 0x7F;
    packet[2] = static_cast<uint8_t>(sequenceNumber >> 8);
    packet[3] = static_cast<uint8_t>(sequenceNumber & 0xFF);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    packet[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    packet[7] = static_cast<uint8_t>(timestamp & 0xFF);
    packet[8] = static_cast<uint8_t>(ssrc >> 24);
    packet[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    packet[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    packet[11] = static_cast<uint8_t>(ssrc & 0xFF);

    memcpy(packet.data() + 12, payload.data(), payload.size());
    return packet;
}

std::string rtpPayloadView(const uint8_t* data, size_t len) {
    if (len < 12 || (data[0] >> 6) != 2) {
        return std::string();
    }

    size_t csrcCount = data[0] & 0x0F;
    size_t headerLen = 12 + (csrcCount * 4);
    if (len < headerLen) {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(data + headerLen), len - headerLen);
}

}  // namespace

int main(int argc, char* argv[]) {
    logger_enable_color(hlog, true);
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--log") == 0) {
            hlog_set_handler(stdout_logger);
        }
    }

    std::atomic<int> offererRtp{0};
    std::atomic<int> answererRtp{0};
    std::atomic<bool> offererReady{false};
    std::atomic<bool> answererReady{false};
    std::atomic<int>  rtpPayloadLen{0};
    std::string       rtpPayloadCopy;

    printf("=== WebRtcTransport loopback test ===\n");

    // ---------- Create two endpoints ----------

    IceAgent agent;
    agent.start();
    auto offerer = std::make_shared<WebRtcTransportImp>(nullptr, &agent);
    auto answerer = std::make_shared<WebRtcTransportImp>(nullptr, &agent);
    answerer->setRole(WebRtcTransport::Role::CLIENT);
    offerer->onStateChange = [&offererReady](WebRtcState s) {
        printf("[offerer]   state -> %s\n", webrtcStateString(s));
        if (s == WebRtcState::Connected) {
            offererReady.store(true);
        }
    };
    answerer->onStateChange = [&answererReady](WebRtcState s) {
        printf("[answerer]  state -> %s\n", webrtcStateString(s));
        if (s == WebRtcState::Connected) {
            answererReady.store(true);
        }
    };

    offerer->onLocalCandidate  = [answerer](const std::string& sdp, const std::string& mid) {
        answerer->addRemoteCandidate(sdp, mid);
    };
    answerer->onLocalCandidate = [offerer](const std::string& sdp, const std::string& mid) {
        offerer->addRemoteCandidate(sdp, mid);
    };

    // ---------- SDP exchange (mimics signaling channel) ----------
    printf("\n[step 1] offerer creates SDP offer\n");
    std::string offerSdp = offerer->createOffer();
    std::string answerSdp = answerer->getAnswerSdp(offerSdp);

    printf("[step 3] offerer sets remote description (answer)\n");
    if (!offerer->setAnswerSdp(answerSdp)) {
        fprintf(stderr, "offerer.setRemoteDescription() failed\n");
        return -1;
    }

    // ---------- Start ICE + DTLS ----------
    printf("\n[step 4] start both endpoints\n");
    offerer->start();
    answerer->start();

    // ---------- Wait for DTLS handshake to complete ----------
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (offererReady.load() && answererReady.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!offererReady.load() || !answererReady.load()) {
        fprintf(stderr, "\n[FAIL] endpoints did not reach Connected within timeout\n");
        fprintf(stderr, "  offerer=%s  answerer=%s\n",
                webrtcStateString(offerer->state()),
                webrtcStateString(answerer->state()));
        return -1;
    }
    printf("\n[OK] both endpoints Connected\n");

    // ---------- Exchange RTP ----------
    printf("\n[step 5] exchange encrypted RTP\n");
    const std::string msg = "Hello WebRTC from libhv!";
    auto offererPacket = makeRtpPacket(msg, 111, 1, 160, 0x10203040);
    auto answererPacket = makeRtpPacket(msg, 111, 2, 320, 0x50607080);
    offerer->sendRtp(offererPacket.data(), offererPacket.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    answerer->sendRtp(answererPacket.data(), answererPacket.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---------- Result ----------
    int recvA = offererRtp.load();
    int recvB = answererRtp.load();
    printf("\n=== Result ===\n");
    printf("  offerer  received %d RTP packets\n", recvA);
    printf("  answerer received %d RTP packets\n", recvB);

    int ret = 0;
    if (recvA == 0 || recvB == 0) {
        fprintf(stderr, "[FAIL] RTP round-trip failed\n");
        ret = -1;
    } else if (rtpPayloadLen.load() != (int)msg.size() ||
               rtpPayloadCopy != msg) {
        fprintf(stderr, "[FAIL] RTP payload mismatch: got %d bytes (%.*s)\n",
                rtpPayloadLen.load(),
                (int)rtpPayloadCopy.size(), rtpPayloadCopy.c_str());
        ret = -1;
    } else {
        printf("[OK] RTP round-trip verified\n");
    }

    offerer->close();
    answerer->close();
    agent.stop();
    return ret;
}
