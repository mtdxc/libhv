#ifndef WEBRTC_TRANSPORT_H_
#define WEBRTC_TRANSPORT_H_

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "EventLoop.h"
#include "EventLoopThread.h"

#include "DtlsTransport.hpp"
#include "SrtpSession.hpp"
#include "agent/ice_agent.h"
#include "session/ice_session.h"

namespace ice {

// WebRTC Transport state
enum class WebRtcState {
    New,
    Connecting,
    Connected,
    Failed,
    Closed
};

const char* webrtcStateString(WebRtcState state);

// Codec information for SDP
struct CodecInfo {
    int payloadType;
    std::string name;
    int clockRate;
    int channels = 0;           // 0 = not applicable (video)
    std::string fmtp;           // Optional fmtp line content
    std::vector<std::string> rtcpFeedback;  // e.g. "nack", "nack pli", "goog-remb"
};

// Media description for SDP generation
struct MediaDesc {
    std::string type;           // "audio" or "video"
    std::string mid;            // Media ID for BUNDLE
    std::string direction = "sendrecv";  // sendrecv, sendonly, recvonly, inactive
    std::vector<CodecInfo> codecs;
};

// Configuration for WebRtcTransport
struct WebRtcOptions {
    // ICE configuration
    IceConfig iceConfig;

    // Media to include in SDP offer/answer
    std::vector<MediaDesc> medias;

    // ICE mode
    IceMode iceMode = IceMode::Full;
};

// WebRtcTransport: Combines ICE + DTLS + SRTP
// - DtlsTransport for key exchange
// - SrtpSession for RTP/RTCP encryption/decryption
// - IceSession for data transport
// - SDP offer/answer interface
// - RTP/RTCP send/receive interface
class WebRtcTransport : public RTC::DtlsTransport::Listener {
public:
    using Ptr = std::shared_ptr<WebRtcTransport>;

    explicit WebRtcTransport(const WebRtcOptions& options);
    ~WebRtcTransport();

    // ===================== SDP Interface =====================

    // Create SDP offer (a=setup:actpass, ICE gathering started)
    std::string createOffer();

    // Create SDP answer (must call setRemoteDescription with offer first)
    // Answers with setup:active if remote offer had actpass
    std::string createAnswer();
    const char* getIdentifier() const {
         return ice_session_ ? ice_session_->id() : "";
    }
    // Process remote SDP (offer or answer)
    // Extracts ICE credentials, candidates, DTLS fingerprint, and setup role
    bool setRemoteDescription(const std::string& sdp);

    // Add a remote ICE candidate (trickle ICE)
    // candidate: SDP candidate string (after "a=candidate:")
    // mid: media ID (unused in single-stream, pass "")
    void addRemoteCandidate(const std::string& candidate, const std::string& mid = "");

    // Signal end of remote ICE candidates
    void setRemoteCandidatesDone();

    // ===================== Control =====================

    // Start ICE connectivity checks and DTLS handshake.
    // Call after setRemoteDescription().
    void start();

    // Close the transport
    void close();

    // Current state
    WebRtcState state() const { return state_; }

    // ===================== RTP/RTCP Interface =====================

    // Send RTP packet (encrypts with SRTP, sends via ICE)
    bool sendRtp(const uint8_t* data, size_t len);

    // Send RTCP packet (encrypts with SRTCP, sends via ICE)
    bool sendRtcp(const uint8_t* data, size_t len);

    // ===================== Accessors =====================

    IceSessionPtr iceSession() const { return ice_session_; }
    IceAgent* iceAgent() const { return ice_agent_.get(); }
    RTC::DtlsTransport::Ptr dtlsTransport() const { return dtls_transport_; }
    RTC::SrtpSession* srtpSendSession() const { return srtp_send_.get(); }
    RTC::SrtpSession* srtpRecvSession() const { return srtp_recv_.get(); }

    // Remote SDP info (available after setRemoteDescription)
    std::string remoteUfrag() const { return remote_ufrag_; }
    std::string remotePwd() const { return remote_pwd_; }

    // ===================== Callbacks =====================

    // State change notification
    std::function<void(WebRtcState)> onStateChange;

    // Decrypted RTP packet received
    std::function<void(const uint8_t* data, size_t len)> onRtpPacket;

    // Decrypted RTCP packet received
    std::function<void(const uint8_t* data, size_t len)> onRtcpPacket;

    // New local ICE candidate discovered (trickle ICE)
    // sdp: full "a=candidate:..." line
    std::function<void(const std::string& sdp, const std::string& mid)> onLocalCandidate;

private:
    void createIceSession();

    // Packet classification (RFC 5764 Section 5.1.4 demultiplexing)
    static bool isStunPacket(const uint8_t* data, size_t len);
    static bool isDtlsPacket(const uint8_t* data, size_t len);
    static bool isRtpOrRtcpPacket(const uint8_t* data, size_t len);
    static bool isRtcpPacket(const uint8_t* data, size_t len);

    // Data processing (demultiplex incoming packets)
    void processIceData(const uint8_t* data, size_t len);
    void processDtlsData(const uint8_t* data, size_t len);
    void processRtpOrRtcp(const uint8_t* data, size_t len);

    // SDP generation
    std::string generateSdp(bool isOffer);
    void parseRemoteSdp(const std::string& sdp);

    // ICE session callbacks
    void onIceStateChanged(IceState state);
    void onIceLocalCandidate(const IceCandidate& candidate);
    void onIceData(const void* data, size_t len);

    // DTLS listener interface
    void OnDtlsTransportConnecting(const RTC::DtlsTransport* dtlsTransport) override;
    void OnDtlsTransportConnected(
        const RTC::DtlsTransport* dtlsTransport,
        RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
        uint8_t* srtpLocalKey, size_t srtpLocalKeyLen,
        uint8_t* srtpRemoteKey, size_t srtpRemoteKeyLen,
        std::string& remoteCert) override;
    void OnDtlsTransportFailed(const RTC::DtlsTransport* dtlsTransport) override;
    void OnDtlsTransportClosed(const RTC::DtlsTransport* dtlsTransport) override;
    void OnDtlsTransportSendData(
        const RTC::DtlsTransport* dtlsTransport,
        const uint8_t* data, size_t len) override;
    void OnDtlsTransportApplicationDataReceived(
        const RTC::DtlsTransport* dtlsTransport,
        const uint8_t* data, size_t len) override;

    // Internal helpers
    void setupSrtp(RTC::SrtpSession::CryptoSuite suite,
                   uint8_t* localKey, size_t localKeyLen,
                   uint8_t* remoteKey, size_t remoteKeyLen);
    void setState(WebRtcState state);

    std::unique_ptr<IceAgent> ice_agent_;
    IceSessionPtr ice_session_;
    RTC::DtlsTransport::Ptr dtls_transport_;
    RTC::SrtpSession::Ptr srtp_send_;
    RTC::SrtpSession::Ptr srtp_recv_;

    // Configuration
    WebRtcOptions options_;

    // Negotiated parameters
    RTC::SrtpSession::CryptoSuite srtp_crypto_suite_ = RTC::SrtpSession::CryptoSuite::NONE;

    // Remote SDP info
    std::string remote_ufrag_;
    std::string remote_pwd_;
    std::string remote_fingerprint_value_;
    RTC::DtlsTransport::FingerprintAlgorithm remote_fingerprint_algo_ =
        RTC::DtlsTransport::FingerprintAlgorithm::NONE;
    std::string remote_setup_;  // "actpass", "active", "passive"

    // State
    WebRtcState state_ = WebRtcState::New;
    RTC::DtlsTransport::Role dtls_role_ = RTC::DtlsTransport::Role::NONE;
};

} // namespace ice

#endif // WEBRTC_TRANSPORT_H_
