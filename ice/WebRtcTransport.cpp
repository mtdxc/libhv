#include "WebRtcTransport.hpp"
#include "EventLoopThread.h"
#include "sdp/ice_sdp.h"
#include "hlog.h"

#include <cstring>
#include <sstream>
#include <algorithm>

namespace ice {

const char* webrtcStateString(WebRtcState state) {
    switch (state) {
        case WebRtcState::New:        return "New";
        case WebRtcState::Connecting: return "Connecting";
        case WebRtcState::Connected:  return "Connected";
        case WebRtcState::Failed:     return "Failed";
        case WebRtcState::Closed:     return "Closed";
        default:                      return "Unknown";
    }
}

// Maximum SRTP overhead (AEAD_AES_256_GCM = 16 bytes auth tag)
static constexpr int kMaxSrtpOverhead = 16;

// ============================================================================
// Construction / Destruction
// ============================================================================

WebRtcTransport::WebRtcTransport(const WebRtcOptions& options)
    : options_(options)
{
    // Create and start ICE agent
    ice_agent_ = std::unique_ptr<IceAgent>(new IceAgent());
    ice_agent_->setConfig(options_.iceConfig);
    ice_agent_->start();
}

WebRtcTransport::~WebRtcTransport() {
    close();
}

// ============================================================================
// SDP Interface
// ============================================================================

std::string WebRtcTransport::createOffer() {
    createIceSession();
    auto ret = generateSdp(true);
    hlogi("WebRtcTransport %s createOffer:\n%s", getIdentifier(), ret.c_str());
    return ret;
}

std::string WebRtcTransport::createAnswer() {
    createIceSession();
    auto ret = generateSdp(false);
    hlogi("WebRtcTransport %s createAnswer:\n%s", getIdentifier(), ret.c_str());
    return ret;
}

bool WebRtcTransport::setRemoteDescription(const std::string& sdp) {
    parseRemoteSdp(sdp);

    if (remote_ufrag_.empty() || remote_pwd_.empty()) {
        hloge("WebRtcTransport remote SDP missing ICE credentials", getIdentifier());
        return false;
    }

    // Create IceSession if not yet created (answerer path)
    createIceSession();

    hlogi("WebRtcTransport %s Setting remote SDP:\n%s", getIdentifier(), sdp.c_str());

    // Set ICE remote credentials
    ice_session_->setRemoteCredentials(remote_ufrag_, remote_pwd_);

    // Add remote ICE candidates parsed from SDP
    // Re-parse to get candidates (IceSdp gives us IceCandidate objects)
    IceSdp::ParseResult iceResult = IceSdp::parseAttributes(sdp);
    for (const auto& cand : iceResult.candidates) {
        ice_session_->addRemoteCandidate(cand);
    }

    // Set DTLS remote fingerprint
    if (!remote_fingerprint_value_.empty() &&
        remote_fingerprint_algo_ != RTC::DtlsTransport::FingerprintAlgorithm::NONE) {
        RTC::DtlsTransport::Fingerprint fp;
        fp.algorithm = remote_fingerprint_algo_;
        fp.value = remote_fingerprint_value_;
        dtls_transport_->SetRemoteFingerprint(fp);
    }

    return true;
}

void WebRtcTransport::createIceSession() {
    if (!ice_session_) {
        ice_session_ = ice_agent_->createSession(options_.iceMode);

        ice_session_->onStateChange = [this](IceState state) { onIceStateChanged(state); };
        ice_session_->onLocalCandidate = [this](const IceCandidate& candidate) { onIceLocalCandidate(candidate); };
        ice_session_->onData = [this](const void* data, size_t len) { onIceData(data, len); };
        // Create DTLS transport (uses same event loop as ICE)
        dtls_transport_ = std::make_shared<RTC::DtlsTransport>(ice_session_->loop(), this);
        dtls_transport_->setId(ice_session_->id());
    }
}

void WebRtcTransport::addRemoteCandidate(const std::string& candidate, const std::string& /*mid*/) {
    if (!ice_session_ || candidate.empty()) return;

    IceCandidate cand;
    if (cand.fromSdp(candidate)) {
        ice_session_->addRemoteCandidate(cand);
    }
}

void WebRtcTransport::setRemoteCandidatesDone() {
    if (ice_session_) {
        ice_session_->setRemoteCandidatesDone();
    }
}

// ============================================================================
// Control
// ============================================================================

void WebRtcTransport::start() {
    if (!ice_session_) {
        hloge("WebRtcTransport::start() - ICE session not created. Call createOffer/createAnswer first.");
        return;
    }
    hlogi("WebRtcTransport %s start", getIdentifier());
    setState(WebRtcState::Connecting);

    // Start ICE connectivity checks
    ice_session_->gatherCandidates(true);

    // If remote_setup_ is empty (we're the offerer), DTLS will start
    // in onIceStateChanged(Connected) after ICE completes.
}

void WebRtcTransport::close() {
    if (state_ == WebRtcState::Closed) return;

    setState(WebRtcState::Closed);
    hlogi("WebRtcTransport %s close", getIdentifier());
    srtp_send_.reset();
    srtp_recv_.reset();
    dtls_transport_.reset();

    if (ice_session_ && ice_agent_) {
        ice_agent_->destroySession(ice_session_);
        ice_session_.reset();
    }

    if (ice_agent_) {
        ice_agent_->stop();
        ice_agent_.reset();
    }
}

// ============================================================================
// RTP/RTCP Send Interface
// ============================================================================

bool WebRtcTransport::sendRtp(const uint8_t* data, size_t len) {
    if (!srtp_send_ || !ice_session_) return false;
    if (ice_session_->state() != IceState::Connected &&
        ice_session_->state() != IceState::Completed) {
        return false;
    }

    // Copy to writable buffer with extra space for SRTP auth tag
    std::vector<uint8_t> buf(data, data + len);
    buf.resize(len + kMaxSrtpOverhead);

    int pktLen = static_cast<int>(len);
    if (!srtp_send_->EncryptRtp(buf.data(), &pktLen)) {
        hlogw("WebRtcTransport %s SRTP encrypt failed", getIdentifier());
        return false;
    }

    return ice_session_->send(buf.data(), pktLen) > 0;
}

bool WebRtcTransport::sendRtcp(const uint8_t* data, size_t len) {
    if (!srtp_send_ || !ice_session_) return false;
    if (ice_session_->state() != IceState::Connected &&
        ice_session_->state() != IceState::Completed) {
        return false;
    }

    // SRTCP can add up to 28 bytes (auth tag + index)
    std::vector<uint8_t> buf(data, data + len);
    buf.resize(len + kMaxSrtpOverhead + 4);

    int pktLen = static_cast<int>(len);
    if (!srtp_send_->EncryptRtcp(buf.data(), &pktLen)) {
        hlogw("WebRtcTransport %s SRTCP encrypt failed", getIdentifier());
        return false;
    }

    return ice_session_->send(buf.data(), pktLen) > 0;
}

// ============================================================================
// Packet Classification (RFC 5764 Section 5.1.4)
// ============================================================================

bool WebRtcTransport::isStunPacket(const uint8_t* data, size_t len) {
    if (len < 20) return false;
    // RFC 5389: STUN packets carry the fixed magic cookie 0x2112A442.
    // Checking only the first byte would misclassify DTLS records (20..63).
    return data[4] == 0x21 &&
           data[5] == 0x12 &&
           data[6] == 0xA4 &&
           data[7] == 0x42;
}

bool WebRtcTransport::isDtlsPacket(const uint8_t* data, size_t len) {
    if (len < 13) return false;
    // DTLS content type: 20..63
    return (data[0] > 19 && data[0] < 64);
}

bool WebRtcTransport::isRtpOrRtcpPacket(const uint8_t* data, size_t len) {
    if (len < 2) return false;
    // RTP/RTCP: first byte 128..191 (version=2 → 10xxxxxx)
    return (data[0] > 127 && data[0] < 192);
}

bool WebRtcTransport::isRtcpPacket(const uint8_t* data, size_t len) {
    if (len < 2) return false;
    // RTCP payload types: 200..206 (SR, RR, SDES, BYE, APP, RTPFB, PSFB)
    uint8_t pt = data[1];
    return (pt >= 200 && pt <= 206);
}

// ============================================================================
// Data Processing (Demultiplex)
// ============================================================================

void WebRtcTransport::processIceData(const uint8_t* data, size_t len) {
    if (len < 1) return;

    if (isStunPacket(data, len)) {
        // STUN packet - should be handled internally by IceSession
        // (keepalives, etc.) This path is a fallback.
        return;
    }

    if (isDtlsPacket(data, len)) {
        processDtlsData(data, len);
        return;
    }

    if (isRtpOrRtcpPacket(data, len)) {
        processRtpOrRtcp(data, len);
        return;
    }

    hlogw("WebRtcTransport %s unknown packet type, first byte=%d", getIdentifier(), data[0]);
}

void WebRtcTransport::processDtlsData(const uint8_t* data, size_t len) {
    if (dtls_transport_) {
        dtls_transport_->ProcessDtlsData(data, len);
    }
}

void WebRtcTransport::processRtpOrRtcp(const uint8_t* data, size_t len) {
    if (!srtp_recv_) {
        hlogw("WebRtcTransport %s SRTP recv session not ready, dropping packet", getIdentifier());
        return;
    }

    // Copy to writable buffer (decrypt modifies in-place)
    std::vector<uint8_t> buf(data, data + len);
    int pktLen = static_cast<int>(len);

    if (isRtcpPacket(data, len)) {
        if (srtp_recv_->DecryptSrtcp(buf.data(), &pktLen)) {
            if (onRtcpPacket) {
                onRtcpPacket(buf.data(), pktLen);
            }
        } else {
            hlogw("WebRtcTransport %s SRTCP decrypt failed", getIdentifier());
        }
    } else {
        if (srtp_recv_->DecryptSrtp(buf.data(), &pktLen)) {
            if (onRtpPacket) {
                onRtpPacket(buf.data(), pktLen);
            }
        } else {
            hlogw("WebRtcTransport %s SRTP decrypt failed", getIdentifier());
        }
    }
}

// ============================================================================
// ICE Session Callbacks
// ============================================================================

void WebRtcTransport::onIceStateChanged(IceState state) {
    hlogi("WebRtcTransport %s ICE state -> %s", getIdentifier(), iceStateString(state));

    switch (state) {
        case IceState::Completed:
        {
            // Determine DTLS role and start handshake if possible
            if (remote_setup_ == "actpass") {
                // Remote offered with actpass → we answer as client (DTLS client)
                dtls_role_ = RTC::DtlsTransport::Role::CLIENT;
            }
            else if (remote_setup_ == "active") {
                // Remote is DTLS client → we are server
                dtls_role_ = RTC::DtlsTransport::Role::SERVER;
            }
            else if (remote_setup_ == "passive") {
                // Remote is DTLS server → we are client
                dtls_role_ = RTC::DtlsTransport::Role::CLIENT;
            }

            // ICE connected - start DTLS handshake if we are the offerer
            // (answerer already started DTLS in start())
            if (dtls_role_ == RTC::DtlsTransport::Role::NONE) {
                // We are the offerer: become DTLS server (wait for client)
                dtls_role_ = RTC::DtlsTransport::Role::SERVER;
            }
            dtls_transport_->Run(dtls_role_);
            break;
        }
        case IceState::Connected: 
            break;
        case IceState::Failed:
            setState(WebRtcState::Failed);
            break;
        case IceState::Closed:
            setState(WebRtcState::Closed);
            break;
        default:
            break;
    }
}

void WebRtcTransport::onIceLocalCandidate(const IceCandidate& candidate) {
    if (onLocalCandidate) {
        std::string sdpLine = candidate.toSdp();
        // Use first media's mid, or empty string
        std::string mid = options_.medias.empty() ? "0" : options_.medias[0].mid;
        onLocalCandidate(sdpLine, mid);
    }
}

void WebRtcTransport::onIceData(const void* data, size_t len) {
    processIceData(static_cast<const uint8_t*>(data), len);
}

// ============================================================================
// DTLS Listener Callbacks
// ============================================================================

void WebRtcTransport::OnDtlsTransportConnecting(
    const RTC::DtlsTransport* /*dtlsTransport*/)
{
    hlogi("WebRtcTransport %s DTLS connecting", getIdentifier());
}

void WebRtcTransport::OnDtlsTransportConnected(
    const RTC::DtlsTransport* /*dtlsTransport*/,
    RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
    uint8_t* srtpLocalKey, size_t srtpLocalKeyLen,
    uint8_t* srtpRemoteKey, size_t srtpRemoteKeyLen,
    std::string& /*remoteCert*/)
{
    hlogi("WebRtcTransport %s DTLS connected, setting up SRTP", getIdentifier());

    srtp_crypto_suite_ = srtpCryptoSuite;
    setupSrtp(srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen,
              srtpRemoteKey, srtpRemoteKeyLen);

    setState(WebRtcState::Connected);
}

void WebRtcTransport::OnDtlsTransportFailed(
    const RTC::DtlsTransport* /*dtlsTransport*/)
{
    hloge("WebRtcTransport %s DTLS failed", getIdentifier());
    setState(WebRtcState::Failed);
}

void WebRtcTransport::OnDtlsTransportClosed(
    const RTC::DtlsTransport* /*dtlsTransport*/)
{
    hlogi("WebRtcTransport %s DTLS closed", getIdentifier());
    setState(WebRtcState::Closed);
}

void WebRtcTransport::OnDtlsTransportSendData(
    const RTC::DtlsTransport* /*dtlsTransport*/,
    const uint8_t* data, size_t len)
{
    // DTLS data goes out through ICE
    if (ice_session_) {
        ice_session_->send(data, len);
    }
}

void WebRtcTransport::OnDtlsTransportApplicationDataReceived(
    const RTC::DtlsTransport* /*dtlsTransport*/,
    const uint8_t* /*data*/, size_t /*len*/)
{
    // WebRTC media transport - application data not used
}

// ============================================================================
// SRTP Setup
// ============================================================================

void WebRtcTransport::setupSrtp(
    RTC::SrtpSession::CryptoSuite suite,
    uint8_t* localKey, size_t localKeyLen,
    uint8_t* remoteKey, size_t remoteKeyLen)
{
    srtp_send_ = std::make_shared<RTC::SrtpSession>(
        RTC::SrtpSession::Type::OUTBOUND, suite, localKey, localKeyLen);
    srtp_recv_ = std::make_shared<RTC::SrtpSession>(
        RTC::SrtpSession::Type::INBOUND, suite, remoteKey, remoteKeyLen);
    char id[64];
    snprintf(id, sizeof(id), "%s-send", ice_session_ ? ice_session_->id() : "unknown");
    srtp_send_->setId(id);
    snprintf(id, sizeof(id), "%s-recv", ice_session_ ? ice_session_->id() : "unknown");
    srtp_recv_->setId(id);
    hlogi("WebRtcTransport %s SRTP sessions created (suite=%d)", getIdentifier(), static_cast<int>(suite));
}

// ============================================================================
// State Management
// ============================================================================

void WebRtcTransport::setState(WebRtcState state) {
    if (state_ == state) return;
    state_ = state;
    hlogi("WebRtcTransport %s state -> %s", getIdentifier(), webrtcStateString(state));
    if (onStateChange) {
        onStateChange(state);
    }
}

// ============================================================================
// SDP Generation
// ============================================================================

std::string WebRtcTransport::generateSdp(bool isOffer) {
    std::ostringstream sdp;

    // --- Session-level ---
    sdp << "v=0\r\n";
    sdp << "o=- " << static_cast<uint64_t>(rand()) << " "
        << static_cast<uint64_t>(rand()) << " IN IP4 0.0.0.0\r\n";
    sdp << "s=-\r\n";
    sdp << "t=0 0\r\n";

    // BUNDLE group (all mids)
    if (!options_.medias.empty()) {
        sdp << "a=group:BUNDLE";
        for (const auto& m : options_.medias) {
            sdp << " " << m.mid;
        }
        sdp << "\r\n";
    }

    sdp << "a=msid-semantic:WMS *\r\n";

    // --- ICE attributes (session-level) ---
    if (ice_session_) {
        sdp << "a=ice-ufrag:" << ice_session_->localUfrag() << "\r\n";
        sdp << "a=ice-pwd:" << ice_session_->localPwd() << "\r\n";
        sdp << "a=ice-options:trickle\r\n";

        // ICE candidates
        for (const auto& cand : ice_session_->localCandidates()) {
            sdp << "a=candidate:" << cand.toSdp() << "\r\n";
        }
    }

    // --- DTLS fingerprint (session-level) ---
    auto& fingerprints = dtls_transport_->GetLocalFingerprints();
    for (const auto& fp : fingerprints) {
        sdp << "a=fingerprint:"
            << RTC::DtlsTransport::GetFingerprintAlgorithmString(fp.algorithm)
            << " " << fp.value << "\r\n";
    }

    // setup attribute
    if (isOffer) {
        sdp << "a=setup:actpass\r\n";
    } else {
        // Answer: active if remote had actpass, passive if remote had active
        if (remote_setup_ == "actpass" || remote_setup_ == "passive") {
            sdp << "a=setup:active\r\n";
        } else if (remote_setup_ == "active") {
            sdp << "a=setup:passive\r\n";
        } else {
            sdp << "a=setup:active\r\n";  // default for answer
        }
    }

    // --- Media sections ---
    for (const auto& media : options_.medias) {
        sdp << "m=" << media.type << " 9 UDP/TLS/RTP/SAVPF";
        for (const auto& codec : media.codecs) {
            sdp << " " << codec.payloadType;
        }
        sdp << "\r\n";

        sdp << "c=IN IP4 0.0.0.0\r\n";
        sdp << "a=mid:" << media.mid << "\r\n";
        sdp << "a=rtcp-mux\r\n";

        if (!media.direction.empty()) {
            sdp << "a=" << media.direction << "\r\n";
        }

        // Codec attributes
        for (const auto& codec : media.codecs) {
            // rtpmap
            if (media.type == "audio" && codec.channels > 0) {
                sdp << "a=rtpmap:" << codec.payloadType << " "
                    << codec.name << "/" << codec.clockRate
                    << "/" << codec.channels << "\r\n";
            } else {
                sdp << "a=rtpmap:" << codec.payloadType << " "
                    << codec.name << "/" << codec.clockRate << "\r\n";
            }

            // fmtp
            if (!codec.fmtp.empty()) {
                sdp << "a=fmtp:" << codec.payloadType << " "
                    << codec.fmtp << "\r\n";
            }

            // rtcp-fb
            for (const auto& fb : codec.rtcpFeedback) {
                sdp << "a=rtcp-fb:" << codec.payloadType << " " << fb << "\r\n";
            }
        }
    }

    return sdp.str();
}

// ============================================================================
// SDP Parsing
// ============================================================================

void WebRtcTransport::parseRemoteSdp(const std::string& sdp) {
    std::istringstream iss(sdp);
    std::string line;

    while (std::getline(iss, line)) {
        // Trim trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        // --- Session-level attributes (before any m= line) ---
        if (line.find("a=ice-ufrag:") == 0 && remote_ufrag_.empty()) {
            remote_ufrag_ = line.substr(12);
        }
        else if (line.find("a=ice-pwd:") == 0 && remote_pwd_.empty()) {
            remote_pwd_ = line.substr(10);
        }
        else if (line.find("a=fingerprint:") == 0 && remote_fingerprint_value_.empty()) {
            std::string rest = line.substr(14);
            auto spacePos = rest.find(' ');
            if (spacePos != std::string::npos) {
                std::string algo = rest.substr(0, spacePos);
                remote_fingerprint_algo_ =
                    RTC::DtlsTransport::GetFingerprintAlgorithm(algo);
                // Skip any whitespace between algo and value
                size_t valStart = rest.find_first_not_of(' ', spacePos);
                if (valStart != std::string::npos) {
                    remote_fingerprint_value_ = rest.substr(valStart);
                }
            }
        }
        else if (line.find("a=setup:") == 0 && remote_setup_.empty()) {
            remote_setup_ = line.substr(8);
        }
    }
}

} // namespace ice
