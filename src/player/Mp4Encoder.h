//
// Created by liangzhuohua on 2022/3/1.
//

#ifndef CTRLCENTER_MP4ENCODER_H
#define CTRLCENTER_MP4ENCODER_H
#include "ffmpegInclude.h"
#include <memory>
#include <string>
#include <cstdint>

using namespace std;
class Mp4Encoder {
public:
    explicit Mp4Encoder(const string &saveFilePath);
    ~Mp4Encoder();
    // 开启
    bool start();
    // 关闭
    bool stop();
    // 增加轨道
    bool addTrack(AVStream *stream, const AVCodecContext *codecContext = nullptr);
    // Use an Annex-B keyframe to populate MP4 codec configuration before the header is written.
    bool primeVideoParameters(const AVPacket *packet);
    // 写packet
    void writePacket(const shared_ptr<AVPacket> &pkt, bool isVideo);
    const string &lastError() const { return _lastError; }
    // 音视频index
    int videoIndex = -1;
    int audioIndex = -1;
    // 文件保存路径
    string _saveFilePath;

private:
    // 是否已经初始化
    bool _isOpen = false;
    // 编码上下文
    shared_ptr<AVFormatContext> _formatCtx;
    // 原始视频流时间基
    AVRational _originVideoTimeBase {};
    // 原始音频流时间基
    AVRational _originAudioTimeBase {};
    // 已经写入关键帧
    bool writtenKeyFrame = false;
    bool _wrotePacket = false;
    AVCodecID _videoCodecId = AV_CODEC_ID_NONE;
    int64_t _videoFirstTimestamp = AV_NOPTS_VALUE;
    int64_t _audioFirstTimestamp = AV_NOPTS_VALUE;
    int64_t _videoLastDts = AV_NOPTS_VALUE;
    int64_t _audioLastDts = AV_NOPTS_VALUE;
    string _lastError;

    bool fail(int errorCode, const string &context);
};

#endif // CTRLCENTER_MP4ENCODER_H
