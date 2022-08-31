#pragma once
#include "Track.h"
namespace mediakit {

class AudioSdp : public Sdp {
public:
    AudioSdp(AudioTrack* track, int payload_type = 98);

    CodecId getCodecId() const override { return _codecId; }
    std::string getSdp() const override;
protected:
    std::stringstream _printer;
    CodecId _codecId;
};

class AudioTrackImp : public AudioTrack {
public:
    typedef std::shared_ptr<AudioTrackImp> Ptr;

    /**
        * ���캯��
        * @param codecId ��������
        * @param sample_rate ������(HZ)
        * @param channels ͨ����
        * @param sample_bit ����λ����һ��Ϊ16
        */
    AudioTrackImp(CodecId codecId, int sample_rate, int channels, int sample_bit) {
        _codecid = codecId;
        _sample_rate = sample_rate;
        _channels = channels;
        _sample_bit = sample_bit;
    }

    /**
        * ���ر�������
        */
    CodecId getCodecId() const override {
        return _codecid;
    }

    /**
        * �Ƿ��Ѿ���ʼ��
        */
    bool ready() override {
        return true;
    }

    /**
        * ������Ƶ������
        */
    int getAudioSampleRate() const override {
        return _sample_rate;
    }

    /**
        * ������Ƶ����λ����һ��Ϊ16��8
        */
    int getAudioSampleBit() const override {
        return _sample_bit;
    }

    /**
        * ������Ƶͨ����
        */
    int getAudioChannel() const override {
        return _channels;
    }

    Sdp::Ptr getSdp() override {
        if (!ready()) {
            WarnL << getCodecName() << " Trackδ׼����";
            return nullptr;
        }
        return std::make_shared<AudioSdp>(this);
    }

    Track::Ptr clone() override {
        return std::make_shared<AudioTrackImp>(*this);
    }
private:
    CodecId _codecid;
    int _sample_rate;
    int _channels;
    int _sample_bit;
};

/**
 * L16��Ƶͨ��
 */
class L16Track : public AudioTrackImp {
public:
    using Ptr = std::shared_ptr<L16Track>;
    L16Track(int sample_rate, int channels) : AudioTrackImp(CodecL16, sample_rate, channels, 16) {}

private:
    Track::Ptr clone() override {
        return std::make_shared<std::remove_reference<decltype(*this)>::type >(*this);
    }
};

/**
 * Opus֡��Ƶͨ��
 */
class OpusTrack : public AudioTrackImp {
public:
    typedef std::shared_ptr<OpusTrack> Ptr;
    OpusTrack() : AudioTrackImp(CodecOpus, 48000, 2, 16) {}

private:
    Track::Ptr clone() override {
        return std::make_shared<std::remove_reference<decltype(*this)>::type >(*this);
    }
};

/**
 * G711��Ƶͨ��
 */
class G711Track : public AudioTrackImp {
public:
    using Ptr = std::shared_ptr<G711Track>;
    G711Track(CodecId codecId, int sample_rate, int channels, int sample_bit) : AudioTrackImp(codecId, 8000, 1, 16) {}

private:
    Sdp::Ptr getSdp() override;

    Track::Ptr clone() override {
        return std::make_shared<std::remove_reference<decltype(*this)>::type >(*this);
    }
};
}

