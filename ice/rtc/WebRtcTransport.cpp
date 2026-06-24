#include "WebRtcTransport.hpp"
#include "EventLoopThread.h"
#include "sdp/ice_sdp.h"
#include "hlog.h"
#include "htime.h"
#include <cstring>
#include <sstream>
#include <algorithm>
using namespace std;
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
WebRtcTransport::WebRtcTransport(const IceConfig* options, IceAgent* agent) : ice_agent_(agent) {
    if (!ice_agent_) {
        // Create and start ICE agent
        ice_agent_ = new IceAgent();
        if (options) {
            ice_agent_->setConfig(*options);
        }
        ice_agent_->start();
        owner_agent_ = true;
    } else {
        ice_agent_->start();
    }
    createIceSession();
}

WebRtcTransport::~WebRtcTransport() {
    hlogi("~WebRtcTransport");
    if (owner_agent_) {
        delete ice_agent_;
    }
}

// ============================================================================
// SDP Interface
// ============================================================================

std::string getFingerprint(const std::string &algorithm_str, const std::shared_ptr<RTC::DtlsTransport> &transport) {
    auto algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(algorithm_str);
    for (auto &finger_prints : transport->GetLocalFingerprints()) {
        if (finger_prints.algorithm == algorithm) {
            return finger_prints.value;
        }
    }
    throw std::invalid_argument(std::string("不支持的加密算法:") + algorithm_str);
}

void WebRtcTransport::onRtcConfigure(RtcConfigure &configure) const {
    SdpAttrFingerprint fingerprint;
    fingerprint.algorithm = _offer_sdp ? _offer_sdp->media[0].fingerprint.algorithm : "sha-256";
    fingerprint.hash = getFingerprint(fingerprint.algorithm, dtls_transport_);
    configure.setDefaultSetting(ice_session_->localUfrag(), ice_session_->localPwd(), RtpDirection::sendrecv, fingerprint);

    // add local candidate
    if (ice_session_ && !ice_session_->localCandidates().empty()) {
        for(auto c : ice_session_->localCandidates()) {
            auto candidate = std::make_shared<SdpAttrCandidate>();
            candidate->foundation = c.foundation;
            candidate->component = 1;
            candidate->transport = TransportProtocolStr(c.protocol);
            candidate->priority = c.priority;
            char ipstr[64];
            candidate->address = sockaddr_ip(&c.addr, ipstr, sizeof(ipstr));
            candidate->port = sockaddr_port(&c.addr);
            candidate->type = c.typeString(c.type);
            if (strcasecmp(candidate->transport.c_str(), "tcp") == 0) {
                candidate->type += " tcptype passive";
            }
            /*
            if (candidate->type != "host" && !c.relatedAddr) {
                candidate->arr.emplace_back("raddr", base_host);
                candidate->arr.emplace_back("rport", std::to_string(base_port));
            }
            */
            configure.addCandidate(*candidate);
        }
    }
}


std::string WebRtcTransport::createOffer() {
    try {
        start();
        RtcConfigure configure;
        onRtcConfigure(configure);
        _offer_sdp = configure.createOffer();
        auto ret = _offer_sdp->toString();
        hlogi("WebRtcTransport %s createOffer=%s", getIdentifier(), ret.c_str());
        return ret;
    } catch (exception &ex) {
        close(ex.what());
        throw;
    }
}


std::string WebRtcTransport::createAnswer() {
    if (!_offer_sdp) {
        throw std::runtime_error("createAnswer called before setRemoteDescription with offer");
    }
    start();
    // sdp configure
    RtcConfigure configure;
    onRtcConfigure(configure);
    // create answer
    _answer_sdp = configure.createAnswer(*_offer_sdp);
    onCheckSdp(SdpType::answer, *_answer_sdp);
    //setSdpBitrate(*_answer_sdp);
    _answer_sdp->checkValid();
    auto ret = _answer_sdp->toString();
    hlogi("WebRtcTransport %s createAnswer=%s", getIdentifier(), ret.c_str());
    return ret;
}

bool WebRtcTransport::setRemoteDescription(SdpType type, const std::string& sdp) {
    try {
        auto sdpSession = std::make_shared<RtcSession>();
        sdpSession->loadFrom(sdp);
        sdpSession->checkValid();
        switch (type)
        {
        case SdpType::offer:
            _offer_sdp = sdpSession;
            hlogi("WebRtcTransport %s setRemoteDescription offer=\n%s", getIdentifier(), sdp.c_str());
            break;
        case SdpType::answer:
            _answer_sdp = sdpSession;
            hlogi("WebRtcTransport %s setRemoteDescription answer=\n%s", getIdentifier(), sdp.c_str());
            break;
        default:
            break;
        }
        onCheckSdp(type, *sdpSession);
        // 设置远端dtls签名
        auto& media = sdpSession->media[0];
        RTC::DtlsTransport::Fingerprint remote_fingerprint;
        remote_fingerprint.algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(media.fingerprint.algorithm);
        remote_fingerprint.value = media.fingerprint.hash;
        dtls_transport_->SetRemoteFingerprint(remote_fingerprint);
        ice_session_->setRemoteCredentials(media.ice_ufrag, media.ice_pwd);
        if (type == SdpType::answer && ice_session_->mode() == IceMode::Full) {
            for (auto& m : sdpSession->media) {
                for (auto& item : m.candidate) {
                    IceCandidate cand;
                    if (cand.fromSdp(item.toString())) 
                        ice_session_->addRemoteCandidate(cand);
                }
            }
        }
        return true;
    } catch (exception &ex) {
        close(ex.what());
        throw;
    }
    return false;
}

std::string WebRtcTransport::getAnswerSdp(const string &offer) {
    setRemoteDescription(SdpType::offer, offer);
    return createAnswer();
}

bool WebRtcTransport::setAnswerSdp(const std::string &answer) {
    return setRemoteDescription(SdpType::answer, answer);
}

void WebRtcTransport::createIceSession() {
    if (!ice_session_) {
        ice_session_ = ice_agent_->createSession();

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
    _self = shared_from_this();
    if (!ice_session_) {
        hloge("WebRtcTransport::start() - ICE session not created. Call createOffer/createAnswer first.");
        return;
    }
    hlogi("WebRtcTransport %s start", getIdentifier());
    setState(WebRtcState::Connecting);
    ice_session_->setMode(_role == Role::CLIENT ? IceMode::Full : IceMode::Lite);
    // Start ICE connectivity checks
    ice_session_->gatherCandidates(_role == Role::CLIENT);
    // If remote_setup_ is empty (we're the offerer), DTLS will start
    // in onIceStateChanged(Connected) after ICE completes.

    if (timeout_sec_ > 0) {
        _ticker.resetTime();
        std::weak_ptr<WebRtcTransport> weak_self = shared_from_this();
        loop()->setInterval(timeout_sec_ * 500, [weak_self](hv::TimerID id) {
            auto self = weak_self.lock();
            if (!self || self->state() == WebRtcState::Closed) {
                hv::killTimer(id);
                return;
            }
            if (self->_ticker.elapsedTime() > (uint64_t)self->timeout_sec_ * 1000) {
                self->close("rtp/rtcp timeout");
                hv::killTimer(id);
            }
        });
    }
}

void WebRtcTransport::close(const char* resson) {
    if (state_ == WebRtcState::Closed) {
        return;
    }
    hlogi("WebRtcTransport %s close %s", getIdentifier(), resson ? resson : "");
    onClose();
    setState(WebRtcState::Closed);
    srtp_send_.reset();
    srtp_recv_.reset();
    dtls_transport_.reset();

    if (ice_session_ && ice_agent_) {
        ice_agent_->destroySession(ice_session_);
        ice_session_.reset();
    }
    _self = nullptr;
}

// ============================================================================
// RTP/RTCP Send Interface
// ============================================================================

bool WebRtcTransport::sendRtp(const void* data, size_t len, void *ctx) {
    if (!srtp_send_ || !ice_session_) return false;
    if (ice_session_->state() != IceState::Connected &&
        ice_session_->state() != IceState::Completed) {
        return false;
    }

    // Copy to writable buffer with extra space for SRTP auth tag
    std::vector<uint8_t> buf((const uint8_t*)data, (const uint8_t*)data + len);
    buf.resize(len + kMaxSrtpOverhead);

    int pktLen = static_cast<int>(len);
    onBeforeEncryptRtp((char*)buf.data(), pktLen, ctx);
    if (!srtp_send_->EncryptRtp(buf.data(), &pktLen)) {
        hlogw("WebRtcTransport %s SRTP encrypt failed", getIdentifier());
        return false;
    }

    return ice_session_->send(buf.data(), pktLen) > 0;
}

bool WebRtcTransport::sendRtcp(const void* data, size_t len, void* ctx) {
    if (!srtp_send_ || !ice_session_) return false;
    if (ice_session_->state() != IceState::Connected &&
        ice_session_->state() != IceState::Completed) {
        return false;
    }

    // SRTCP can add up to 28 bytes (auth tag + index)
    std::vector<uint8_t> buf((const uint8_t*)data, (const uint8_t*)data + len);
    buf.resize(len + kMaxSrtpOverhead + 4);

    int pktLen = static_cast<int>(len);
    onBeforeEncryptRtcp((char*)buf.data(), pktLen, ctx);
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
    _ticker.resetTime();

    // Copy to writable buffer (decrypt modifies in-place)
    std::vector<uint8_t> buf(data, data + len);
    int pktLen = static_cast<int>(len);

    if (isRtcpPacket(data, len)) {
        if (srtp_recv_->DecryptSrtcp(buf.data(), &pktLen)) {
            onRtcp((const char*)buf.data(), pktLen);
        } else {
            hlogw("WebRtcTransport %s SRTCP decrypt failed", getIdentifier());
        }
    } else {
        if (srtp_recv_->DecryptSrtp(buf.data(), &pktLen)) {
            onRtp((const char*)buf.data(), pktLen, _ticker.createdTime());
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
            if ((getRole() == Role::PEER && _answer_sdp->media[0].role == DtlsRole::passive)
                || (getRole() == Role::CLIENT && _answer_sdp->media[0].role == DtlsRole::active)) {
                dtls_transport_->Run(RTC::DtlsTransport::Role::SERVER);
            } else {
                dtls_transport_->Run(RTC::DtlsTransport::Role::CLIENT);
            }
            break;
        }
        case IceState::Connected: 
            break;
        case IceState::Failed:
            close("ice session failed");
            break;
        case IceState::Closed:
            close("ice session closed");
            break;
        default:
            break;
    }
}

void WebRtcTransport::onIceLocalCandidate(const IceCandidate& candidate) {
    if (onLocalCandidate) {
        std::string sdpLine = candidate.toSdp();
        // Use first media's mid, or empty string
        std::string mid = _offer_sdp ? _offer_sdp->media[0].mid : "0";
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

    setupSrtp(srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen,
              srtpRemoteKey, srtpRemoteKeyLen);

    setState(WebRtcState::Connected);
    onStartWebRTC();
}

void WebRtcTransport::OnDtlsTransportFailed(
    const RTC::DtlsTransport* /*dtlsTransport*/)
{
    hloge("WebRtcTransport %s DTLS failed", getIdentifier());
    close("dtls transport failed");
}

void WebRtcTransport::OnDtlsTransportClosed(
    const RTC::DtlsTransport* /*dtlsTransport*/)
{
    hlogi("WebRtcTransport %s DTLS closed", getIdentifier());
    close("dtls transport closed");
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

} // namespace ice
