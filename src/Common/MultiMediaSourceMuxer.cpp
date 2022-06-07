﻿/*
* Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
*
* Use of this source code is governed by MIT license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#include <math.h>
#include "Common/config.h"
#include "MultiMediaSourceMuxer.h"
#include "Extension/AAC.h"
#include "Extension/Opus.h"
#include "Extension/G711.h"
using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::MultiMediaSourceMuxer);
}

namespace mediakit {

ProtocolOption::ProtocolOption() {
    GET_CONFIG(bool, s_to_hls, General::kPublishToHls);
    GET_CONFIG(bool, s_to_mp4, General::kPublishToMP4);
    GET_CONFIG(bool, s_enabel_audio, General::kEnableAudio);
    GET_CONFIG(bool, s_add_mute_audio, General::kAddMuteAudio);
    GET_CONFIG(bool, s_audio_transcode, General::kTranscodeAudio);
    GET_CONFIG(uint32_t, s_continue_push_ms, General::kContinuePushMS);

    enable_hls = s_to_hls;
    enable_mp4 = s_to_mp4;
    enable_audio = s_enabel_audio;
    add_mute_audio = s_add_mute_audio;
    audio_transcode = s_audio_transcode;
    continue_push_ms = s_continue_push_ms;
}

static std::shared_ptr<MediaSinkInterface> makeRecorder(MediaSource &sender, const vector<Track::Ptr> &tracks, Recorder::type type, const string &custom_path, size_t max_second){
    auto recorder = Recorder::createRecorder(type, sender.getVhost(), sender.getApp(), sender.getId(), custom_path, max_second);
    for (auto &track : tracks) {
        recorder->addTrack(track);
    }
    return recorder;
}

static string getTrackInfoStr(const TrackSource *track_src){
    _StrPrinter codec_info;
    auto tracks = track_src->getTracks(true);
    for (auto &track : tracks) {
        auto codec_type = track->getTrackType();
        codec_info << track->getCodecName();
        switch (codec_type) {
            case TrackAudio : {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                codec_info << "["
                           << audio_track->getAudioSampleRate() << "/"
                           << audio_track->getAudioChannel() << "/"
                           << audio_track->getAudioSampleBit() << "] ";
                break;
            }
            case TrackVideo : {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                codec_info << "["
                           << video_track->getVideoWidth() << "/"
                           << video_track->getVideoHeight() << "/"
                           << round(video_track->getVideoFps()) << "] ";
                break;
            }
            default:
                break;
        }
    }
    return std::move(codec_info);
}

MultiMediaSourceMuxer::MultiMediaSourceMuxer(const string &vhost, const string &app, const string &stream, float dur_sec, const ProtocolOption &option) {
    _get_origin_url = [this, vhost, app, stream]() {
        auto ret = getOriginUrl(*MediaSource::NullMediaSource);
        if (!ret.empty()) {
            return ret;
        }
        return vhost + "/" + app + "/" + stream;
    };

    if (option.enable_rtmp) {
        _rtmp = std::make_shared<RtmpMediaSourceMuxer>(vhost, app, stream, std::make_shared<TitleMeta>(dur_sec));
    }
    if (option.enable_rtsp) {
        _rtsp = std::make_shared<RtspMediaSourceMuxer>(vhost, app, stream, RTSP_SCHEMA, std::make_shared<TitleSdp>(dur_sec));
    }
    if (option.enable_rtc) {
#if defined(ENABLE_WEBRTC)
        _rtc = std::make_shared<RtspMediaSourceMuxer>(vhost, app, stream, RTC_SCHEMA, std::make_shared<TitleSdp>(dur_sec));
#endif
    }
    if (option.enable_hls) {
        _hls = dynamic_pointer_cast<HlsRecorder>(Recorder::createRecorder(Recorder::type_hls, vhost, app, stream, option.hls_save_path));
    }
    if (option.enable_mp4) {
        _mp4 = Recorder::createRecorder(Recorder::type_mp4, vhost, app, stream, option.mp4_save_path, option.mp4_max_second);
    }
    if (option.enable_ts) {
        _ts = std::make_shared<TSMediaSourceMuxer>(vhost, app, stream);
    }
    if (option.audio_transcode) {
#if defined(ENABLE_FFMPEG)
        _audio_transcode = option.audio_transcode;
        InfoL << "enable audio_transcode";
#else
        InfoL << "without ffmpeg disable audio_transcode";
#endif
    }
#if defined(ENABLE_MP4)
    if (option.enable_fmp4) {
        _fmp4 = std::make_shared<FMP4MediaSourceMuxer>(vhost, app, stream);
    }
#endif

    //音频相关设置
    enableAudio(option.enable_audio);
    enableMuteAudio(option.add_mute_audio);
}

void MultiMediaSourceMuxer::setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);

    auto self = shared_from_this();
    //拦截事件
    if (_rtmp) {
        _rtmp->setListener(self);
    }
    if (_rtsp) {
        _rtsp->setListener(self);
    }
    if (_ts) {
        _ts->setListener(self);
    }
    if (_rtc) {
        _rtc->setListener(self);
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->setListener(self);
    }
#endif
    auto hls = _hls;
    if (hls) {
        hls->setListener(self);
    }
}

void MultiMediaSourceMuxer::setTrackListener(const std::weak_ptr<Listener> &listener) {
    _track_listener = listener;
}

int MultiMediaSourceMuxer::totalReaderCount() const {
    int ret = 0;
    if (_rtsp) ret += _rtsp->readerCount();
    if (_rtmp) ret += _rtmp->readerCount();
    if (_ts) ret += _ts->readerCount();
    if (_rtc) ret += _rtc->readerCount();
#if defined(ENABLE_MP4)
    if (_fmp4) ret += _fmp4->readerCount();
#endif
    if (_hls) ret += _hls->readerCount();

#if defined(ENABLE_RTPPROXY)
    return ret + (int)_rtp_sender.size();
#else
    return ret;
#endif
}

void MultiMediaSourceMuxer::setTimeStamp(uint32_t stamp) {
    if (_rtmp) {
        _rtmp->setTimeStamp(stamp);
    }
    if (_rtsp) {
        _rtsp->setTimeStamp(stamp);
    }
}

int MultiMediaSourceMuxer::totalReaderCount(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return totalReaderCount();
    }
    return listener->totalReaderCount(sender);
}

//此函数可能跨线程调用
bool MultiMediaSourceMuxer::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path, size_t max_second) {
    switch (type) {
        case Recorder::type_hls : {
            if (start && !_hls) {
                //开始录制
                auto hls = dynamic_pointer_cast<HlsRecorder>(makeRecorder(sender, getTracks(), type, custom_path, max_second));
                if (hls) {
                    //设置HlsMediaSource的事件监听器
                    hls->setListener(shared_from_this());
                }
                _hls = hls;
            } else if (!start && _hls) {
                //停止录制
                _hls = nullptr;
            }
            return true;
        }
        case Recorder::type_mp4 : {
            if (start && !_mp4) {
                //开始录制
                _mp4 = makeRecorder(sender, getTracks(), type, custom_path, max_second);
            } else if (!start && _mp4) {
                //停止录制
                _mp4 = nullptr;
            }
            return true;
        }
        default : return false;
    }
}

//此函数可能跨线程调用
bool MultiMediaSourceMuxer::isRecording(MediaSource &sender, Recorder::type type) {
    switch (type){
        case Recorder::type_hls :
            return !!_hls;
        case Recorder::type_mp4 :
            return !!_mp4;
        default:
            return false;
    }
}

void MultiMediaSourceMuxer::startSendRtp(MediaSource &, const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) {
#if defined(ENABLE_RTPPROXY)
    auto rtp_sender = std::make_shared<RtpSender>();
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    rtp_sender->startSend(args, [args, weak_self, rtp_sender, cb](uint16_t local_port, const SockException &ex) {
        cb(local_port, ex);
        auto strong_self = weak_self.lock();
        if (!strong_self || ex) {
            return;
        }
        for (auto &track : strong_self->getTracks(false)) {
            rtp_sender->addTrack(track);
        }
        rtp_sender->addTrackCompleted();
        lock_guard<mutex> lck(strong_self->_rtp_sender_mtx);
        strong_self->_rtp_sender[args.ssrc] = rtp_sender;
    });
#else
    cb(0, SockException(Err_other, "该功能未启用，编译时请打开ENABLE_RTPPROXY宏"));
#endif//ENABLE_RTPPROXY
}

bool MultiMediaSourceMuxer::stopSendRtp(MediaSource &sender, const string &ssrc) {
#if defined(ENABLE_RTPPROXY)
    std::unique_ptr<onceToken> token;
    if (&sender != MediaSource::NullMediaSource) {
        token.reset(new onceToken(nullptr, [&]() {
            //关闭rtp推流，可能触发无人观看事件
            MediaSourceEventInterceptor::onReaderChanged(sender, totalReaderCount());
        }));
    }
    if (ssrc.empty()) {
        //关闭全部
        lock_guard<mutex> lck(_rtp_sender_mtx);
        auto size = _rtp_sender.size();
        _rtp_sender.clear();
        return size;
    }
    //关闭特定的
    lock_guard<mutex> lck(_rtp_sender_mtx);
    return _rtp_sender.erase(ssrc);
#else
    return false;
#endif//ENABLE_RTPPROXY
}

vector<Track::Ptr> MultiMediaSourceMuxer::getMediaTracks(MediaSource &sender, bool trackReady) const {
    return getTracks(trackReady);
}

bool MultiMediaSourceMuxer::onTrackReady(const Track::Ptr &track) {
    if (CodecL16 == track->getCodecId()) {
        WarnL << "L16音频格式目前只支持RTSP协议推流拉流!!!";
        return false;
    }

    bool ret = false;
    auto rtmp = _rtmp;
    auto rtc = _rtc;
#if defined(ENABLE_FFMPEG)
    if (_audio_transcode) {
        if (track->getCodecId() == CodecAAC) {
            if (rtmp) {
                rtmp->addTrack(track);
                rtmp = nullptr;
            }
            _audio_dec = nullptr;
            _audio_enc = nullptr;
            if (rtc) {
                Track::Ptr newTrack(new OpusTrack());
                GET_CONFIG(int, bitrate, General::kOpusBitrate);
                newTrack->setBitRate(bitrate);
                rtc->addTrack(newTrack);
                rtc = nullptr;

                // aac to opus
                _audio_dec.reset(new FFmpegDecoder(track));
                _audio_enc.reset(new FFmpegEncoder(newTrack));
                _audio_dec->setOnDecode([this](const FFmpegFrame::Ptr & frame) {
                    _audio_enc->inputFrame(frame, false);
                });
                _audio_enc->setOnEncode([this](const Frame::Ptr& frame) {
                    // fill data to _rtc
                    if (_rtc && _rtc->isEnabled())
                        _rtc->inputFrame(frame);
                });
            }
        } else if (track->getTrackType() == TrackAudio) {
            if (rtc) {
                rtc->addTrack(track);
                rtc = nullptr;
            }
            _audio_dec = nullptr;
            _audio_enc = nullptr;
            if (rtmp) {
                Track::Ptr newTrack(new AACTrack(44100, 1));
                GET_CONFIG(int, bitrate, General::kAacBitrate);
                newTrack->setBitRate(bitrate);
                rtmp->addTrack(newTrack);
                rtmp = nullptr;

                _audio_dec.reset(new FFmpegDecoder(track));
                _audio_enc.reset(new FFmpegEncoder(newTrack));
                _audio_dec->setOnDecode([this](const FFmpegFrame::Ptr & frame) {
                    _audio_enc->inputFrame(frame, false);
                });
                _audio_enc->setOnEncode([this](const Frame::Ptr& frame) {
                    // fill aac frame to rtmp
                    if (_rtmp && _rtmp->isEnabled())
                        _rtmp->inputFrame(frame);
                });
            }
        }
    }
#endif
    if (rtc && rtc->addTrack(track))
        ret = true;
    if (rtmp && rtmp->addTrack(track))
        ret = true;
    if (_rtsp && _rtsp->addTrack(track))
        ret = true;
    if (_ts && _ts->addTrack(track))
        ret = true;
#if defined(ENABLE_MP4)
    if (_fmp4) {
        ret = _fmp4->addTrack(track) ? true : ret;
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        ret = hls->addTrack(track) ? true : ret;
    }
    auto mp4 = _mp4;
    if (mp4) {
        ret = mp4->addTrack(track) ? true : ret;
    }
    return ret;
}

void MultiMediaSourceMuxer::onAllTrackReady() {
    setMediaListener(getDelegate());

    if (_rtmp) {
        _rtmp->onAllTrackReady();
    }
    if (_rtsp) {
        _rtsp->onAllTrackReady();
    }
    if (_rtc) {
      _rtc->onAllTrackReady();
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->onAllTrackReady();
    }
#endif
    auto listener = _track_listener.lock();
    if (listener) {
        listener->onAllTrackReady();
    }
    InfoL << "stream: " << _get_origin_url() << " , codec info: " << getTrackInfoStr(this);
}

void MultiMediaSourceMuxer::resetTracks() {
    MediaSink::resetTracks();

    if (_rtmp) {
        _rtmp->resetTracks();
    }
    if (_rtsp) {
        _rtsp->resetTracks();
    }
    if (_ts) {
        _ts->resetTracks();
    }
    if (_rtc) {
        _rtc->resetTracks();
    }
#if defined(ENABLE_FFMPEG)
    _audio_dec = nullptr;
    _audio_dec = nullptr;
#endif
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->resetTracks();
    }
#endif

#if defined(ENABLE_RTPPROXY)
    lock_guard<mutex> lck(_rtp_sender_mtx);
    for (auto &pr : _rtp_sender) {
        pr.second->resetTracks();
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        hls->resetTracks();
    }

    auto mp4 = _mp4;
    if (mp4) {
        mp4->resetTracks();
    }
}

//该类实现frame级别的时间戳覆盖
class FrameModifyStamp : public Frame{
public:
    typedef std::shared_ptr<FrameModifyStamp> Ptr;
    FrameModifyStamp(const Frame::Ptr &frame, Stamp &stamp){
        _frame = frame;
        //覆盖时间戳
        stamp.revise(frame->dts(), frame->pts(), _dts, _pts, true);
    }
    ~FrameModifyStamp() override {}

    uint32_t dts() const override{
        return (uint32_t)_dts;
    }

    uint32_t pts() const override{
        return (uint32_t)_pts;
    }

    size_t prefixSize() const override {
        return _frame->prefixSize();
    }

    bool keyFrame() const override {
        return _frame->keyFrame();
    }

    bool configFrame() const override {
        return _frame->configFrame();
    }

    bool cacheAble() const override {
        return _frame->cacheAble();
    }

    char *data() const override {
        return _frame->data();
    }

    size_t size() const override {
        return _frame->size();
    }

    CodecId getCodecId() const override {
        return _frame->getCodecId();
    }
private:
    int64_t _dts;
    int64_t _pts;
    Frame::Ptr _frame;
};

bool MultiMediaSourceMuxer::onTrackFrame(const Frame::Ptr &frame_in) {
    GET_CONFIG(bool, modify_stamp, General::kModifyStamp);
    auto frame = frame_in;
    if (modify_stamp) {
        //开启了时间戳覆盖
        frame = std::make_shared<FrameModifyStamp>(frame, _stamp[frame->getTrackType()]);
    }

    bool ret = false;
    RtspMediaSourceMuxer::Ptr rtc;
    RtmpMediaSourceMuxer::Ptr rtmp;
    if (_rtc && _rtc->isEnabled())
        rtc = _rtc;
    if (_rtmp && _rtmp->isEnabled())
        rtmp = _rtmp;
#if defined(ENABLE_FFMPEG)
    if (_audio_transcode) {
        if (frame->getCodecId() == CodecAAC) {
            if (rtc) {
                if (_audio_dec && rtc->readerCount())
                    _audio_dec->inputFrame(frame, true, false, false);
                rtc = nullptr;
            }
        } else if (frame->getTrackType() == TrackAudio) {
            if (rtmp) {
                if (_audio_dec && rtmp->readerCount())
                    _audio_dec->inputFrame(frame, true, false, false);
                rtmp = nullptr;
            }
        }
    }
#endif
    if (rtc && rtc->inputFrame(frame))
      ret = true;

    if (rtmp && rtmp->inputFrame(frame))
      ret = true;

    if (_rtsp && _rtsp->inputFrame(frame))
        ret =  true;

    if (_ts && _ts->inputFrame(frame))
        ret = true;

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    //此处使用智能指针拷贝来确保线程安全，比互斥锁性能更优
    auto hls = _hls;
    if (hls) {
        ret = hls->inputFrame(frame) ? true : ret;
    }
    auto mp4 = _mp4;
    if (mp4) {
        ret = mp4->inputFrame(frame) ? true : ret;
    }

#if defined(ENABLE_MP4)
    if (_fmp4) {
        ret = _fmp4->inputFrame(frame) ? true : ret;
    }
#endif

#if defined(ENABLE_RTPPROXY)
    lock_guard<mutex> lck(_rtp_sender_mtx);
    for (auto &pr : _rtp_sender) {
        ret = pr.second->inputFrame(frame) ? true : ret;
    }
#endif //ENABLE_RTPPROXY
    return ret;
}

bool MultiMediaSourceMuxer::isEnabled(){
    GET_CONFIG(uint32_t, stream_none_reader_delay_ms, General::kStreamNoneReaderDelayMS);
    if (!_is_enable || _last_check.elapsedTime() > stream_none_reader_delay_ms) {
        //无人观看时，每次检查是否真的无人观看
        //有人观看时，则延迟一定时间检查一遍是否无人观看了(节省性能)
        auto hls = _hls;
        auto flag = (_rtmp ? _rtmp->isEnabled() : false) ||
                    (_rtsp ? _rtsp->isEnabled() : false) ||
                    (_ts ? _ts->isEnabled() : false) ||
                    (_rtc && _rtc->isEnabled()) ||
#if defined(ENABLE_MP4)
                    (_fmp4 ? _fmp4->isEnabled() : false) ||
#endif
                    (hls ? hls->isEnabled() : false) || _mp4;

#if defined(ENABLE_RTPPROXY)
        _is_enable = flag || _rtp_sender.size();
#else
        _is_enable = flag;
#endif //ENABLE_RTPPROXY
        if (_is_enable) {
            //无人观看时，不刷新计时器,因为无人观看时每次都会检查一遍，所以刷新计数器无意义且浪费cpu
            _last_check.resetTime();
        }
    }
    return _is_enable;
}

}//namespace mediakit