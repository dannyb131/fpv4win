
#include "QQuickRealTimePlayer.h"
#include "JpegEncoder.h"
#include "QmlNativeAPI.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QStandardPaths>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstring>
#include <future>
#include <libavutil/pixdesc.h>
#include <sstream>

namespace {
bool containsRandomAccessNal(const AVPacket *packet, AVCodecID codec) {
    if (!packet || !packet->data || packet->size < 5) {
        return false;
    }
    for (int i = 0; i + 4 < packet->size; ++i) {
        int nalOffset = -1;
        if (packet->data[i] == 0 && packet->data[i + 1] == 0 && packet->data[i + 2] == 1) {
            nalOffset = i + 3;
        } else if (i + 5 < packet->size && packet->data[i] == 0 && packet->data[i + 1] == 0
                   && packet->data[i + 2] == 0 && packet->data[i + 3] == 1) {
            nalOffset = i + 4;
        }
        if (nalOffset < 0) {
            continue;
        }
        if (codec == AV_CODEC_ID_HEVC) {
            const int type = (packet->data[nalOffset] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) {
                return true;
            }
        } else if (codec == AV_CODEC_ID_H264 && (packet->data[nalOffset] & 0x1f) == 5) {
            return true;
        }
    }
    return false;
}

shared_ptr<AVPacket> makeBootstrapPacket(const AVPacket *packet, const AVCodecParameters *parameters) {
    if (!packet || !parameters || !containsRandomAccessNal(packet, parameters->codec_id)) {
        return {};
    }
    const int extraSize = std::max(0, parameters->extradata_size);
    auto result = shared_ptr<AVPacket>(av_packet_alloc(), [](AVPacket *value) { av_packet_free(&value); });
    if (!result || av_new_packet(result.get(), extraSize + packet->size) < 0) {
        return {};
    }
    if (extraSize > 0) {
        std::memcpy(result->data, parameters->extradata, extraSize);
    }
    std::memcpy(result->data + extraSize, packet->data, packet->size);
    av_packet_copy_props(result.get(), packet);
    result->stream_index = packet->stream_index;
    result->flags |= AV_PKT_FLAG_KEY;
    return result;
}
}
// GIF默认帧率
#define DEFAULT_GIF_FRAMERATE 10

//************TaoItemRender************//
class TItemRender : public QQuickFramebufferObject::Renderer {
public:
    TItemRender();

    void render() override;
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;
    void synchronize(QQuickFramebufferObject *) override;

private:
    RealTimeRenderer m_render;
    QQuickWindow *m_window = nullptr;
};

TItemRender::TItemRender() {
    m_render.init();
}

void TItemRender::render() {
    m_render.paint();
    m_window->resetOpenGLState();
}

QOpenGLFramebufferObject *TItemRender::createFramebufferObject(const QSize &size) {
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setSamples(4);
    m_render.resize(size.width(), size.height());
    return new QOpenGLFramebufferObject(size, format);
}

void TItemRender::synchronize(QQuickFramebufferObject *item) {

    auto *pItem = qobject_cast<QQuickRealTimePlayer *>(item);
    if (pItem) {
        if (!m_window) {
            m_window = pItem->window();
        }
        if (pItem->infoDirty()) {
            m_render.updateTextureInfo(pItem->videoWidth(), pItem->videoHeght(), pItem->videoFormat());
            pItem->makeInfoDirty(false);
        }
        if (pItem->playStop) {
            m_render.clear();
            return;
        }
        bool got = false;
        shared_ptr<AVFrame> frame = pItem->getFrame(got);
        if (got && frame->linesize[0]) {
            m_render.updateTextureData(frame);
        }
    }
}

//************QQuickRealTimePlayer************//
QQuickRealTimePlayer::QQuickRealTimePlayer(QQuickItem *parent)
    : QQuickFramebufferObject(parent) {
    SDL_Init(SDL_INIT_AUDIO);
    // 按每秒60帧的帧率更新界面
    startTimer(1000 / 100);
}

void QQuickRealTimePlayer::timerEvent(QTimerEvent *event) {
    Q_UNUSED(event);
    update();
}

shared_ptr<AVFrame> QQuickRealTimePlayer::getFrame(bool &got) {
    got = false;
    shared_ptr<AVFrame> frame;
    {
        lock_guard<mutex> lck(mtx);
        // 帧缓冲区已被清空,跳过渲染
        if (videoFrameQueue.empty()) {
            return {};
        }
        // 从帧缓冲区取出帧
        frame = videoFrameQueue.front();
        got = true;
        // 缓冲区出队被渲染的帧
        videoFrameQueue.pop();
        // Keep the last rendered frame under the same lock used by captureJpeg().
        _lastFrame = frame;
    }
    return frame;
}

void QQuickRealTimePlayer::onVideoInfoReady(int width, int height, int format) {
    if (m_videoWidth != width) {
        m_videoWidth = width;
        makeInfoDirty(true);
    }
    if (m_videoHeight != height) {
        m_videoHeight = height;
        makeInfoDirty(true);
    }
    if (m_videoFormat != format) {
        m_videoFormat = format;
        makeInfoDirty(true);
    }
}

QQuickFramebufferObject::Renderer *QQuickRealTimePlayer::createRenderer() const {
    return new TItemRender;
}

void QQuickRealTimePlayer::play(const QString &playUrl) {
    playStop = false;
    if (analysisThread.joinable()) {
        analysisThread.join();
    }
    // 启动分析线程
    analysisThread = std::thread([this, playUrl]() {
        auto decoder_ = make_shared<FFmpegDecoder>();
        url = playUrl.toStdString();
        // 打开并分析输入
        bool ok = decoder_->OpenInput(url);
        if (!ok) {
            QmlNativeAPI::Instance().PutLog("error", "FFmpeg could not open the RTP video stream");
            emit onError("视频加载出错", -2);
            return;
        }
        decoder = decoder_;
        {
            lock_guard<mutex> lock(outputMutex);
            _streamBootstrapPacket.reset();
        }
        if (decoder->HasVideo()) {
            QmlNativeAPI::Instance().PutLog(
                "info", "FFmpeg opened " + std::string(avcodec_get_name(decoder->pVideoCodecCtx->codec_id))
                    + " video using " + (decoder->isHwDecoderEnable ? "D3D11 hardware" : "software")
                    + " decoding");
        } else {
            QmlNativeAPI::Instance().PutLog("error", "FFmpeg opened the input but found no video stream");
        }
        decoder->_gotPktCallback = [this](const shared_ptr<AVPacket> &packet) {
            lock_guard<mutex> lock(outputMutex);
            if (!_streamBootstrapPacket && packet->stream_index == decoder->videoStreamIndex) {
                AVStream *stream = decoder->pFormatCtx->streams[decoder->videoStreamIndex];
                _streamBootstrapPacket = makeBootstrapPacket(packet.get(), stream->codecpar);
                if (_streamBootstrapPacket) {
                    QmlNativeAPI::Instance().PutLog(
                        "info", "Cached H.265 stream bootstrap: codec headers plus random-access frame ("
                            + std::to_string(_streamBootstrapPacket->size) + " bytes)");
                }
            }
            if (_mp4Encoder) {
                _mp4Encoder->writePacket(packet, packet->stream_index == decoder->videoStreamIndex);
            }
            if (_streamPublisher && _streamPublisher->isRunning()) {
                _streamPublisher->enqueuePacket(packet);
            }
        };
        // 启动解码线程
        decodeThread = std::thread([this]() {
            bool loggedFirstFrame = false;
            while (!playStop) {
                try {
                    // 循环解码
                    auto frame = decoder->GetNextFrame();
                    if (!frame) {
                        continue;
                    }
                    if (!loggedFirstFrame) {
                        const char *pixelFormat = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
                        QmlNativeAPI::Instance().PutLog(
                            "info", "Decoded first video frame " + std::to_string(frame->width) + "x"
                                + std::to_string(frame->height) + " format "
                                + (pixelFormat ? pixelFormat : "unknown"));
                        loggedFirstFrame = true;
                    }
                    if (frame->width != m_videoWidth || frame->height != m_videoHeight
                        || frame->format != m_videoFormat) {
                        onVideoInfoReady(frame->width, frame->height, frame->format);
                    }
                    {
                        // 解码获取到视频帧,放入帧缓冲队列
                        lock_guard<mutex> lck(mtx);
                        // Live FPV should discard stale decoded frames instead
                        // of accumulating a latency-producing playback queue.
                        while (videoFrameQueue.size() >= 2) {
                            videoFrameQueue.pop();
                        }
                        videoFrameQueue.push(frame);
                    }
                } catch (const exception &e) {
                    QmlNativeAPI::Instance().PutLog("error", "FFmpeg decode failed: " + std::string(e.what()));
                    emit onError(e.what(), -2);
                    // 出错，停止
                    break;
                }
            }
            playStop = true;
            // 解码已经停止，触发信号
            emit onPlayStopped();
        });
        decodeThread.detach();

        if (!isMuted && decoder->HasAudio()) {
            // 开启音频
            enableAudio();
        }
        // 是否存在音频
        emit onHasAudio(decoder->HasAudio());

        if (decoder->HasVideo()) {
            onVideoInfoReady(decoder->GetWidth(), decoder->GetHeight(), decoder->GetVideoFrameFormat());
        }

        // 码率计算回调
        decoder->onBitrate = [this](uint64_t bitrate) { emit onBitrate(static_cast<long>(bitrate)); };
    });
    analysisThread.detach();
}

void QQuickRealTimePlayer::stop() {
    stopStream();
    playStop = true;
    if (decoder && decoder->pFormatCtx) {
        decoder->pFormatCtx->interrupt_callback.callback = [](void *) { return 1; };
    }
    if (analysisThread.joinable()) {
        analysisThread.join();
    }
    if (decodeThread.joinable()) {
        decodeThread.join();
    }
    while (!videoFrameQueue.empty()) {
        lock_guard<mutex> lck(mtx);
        // 清空缓冲
        videoFrameQueue.pop();
    }
    SDL_CloseAudio();
    if (decoder) {
        decoder->CloseInput();
    }
}

void QQuickRealTimePlayer::setMuted(bool muted) {
    if (!decoder->HasAudio()) {
        return;
    }
    if (!muted && decoder) {
        decoder->ClearAudioBuff();
        // 初始化声音
        if (!enableAudio()) {
            return;
        }
    } else {
        disableAudio();
    }
    isMuted = muted;
    emit onMutedChanged(muted);
}

QQuickRealTimePlayer::~QQuickRealTimePlayer() {
    stop();
}

QString QQuickRealTimePlayer::captureJpeg() {
    shared_ptr<AVFrame> frame;
    {
        lock_guard<mutex> lock(mtx);
        frame = _lastFrame;
    }
    if (!frame) {
        QmlNativeAPI::Instance().PutLog("error", "JPEG capture failed: no decoded frame is available yet");
        return "";
    }

    QDir outputDirectory(QDir::current().filePath("jpg"));
    if (!outputDirectory.exists() && !QDir().mkpath(outputDirectory.absolutePath())) {
        QmlNativeAPI::Instance().PutLog("error", "JPEG capture failed: unable to create the jpg folder");
        return "";
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const QString filePath = outputDirectory.filePath(QString::number(timestamp) + ".jpg");
    const bool ok = JpegEncoder::encodeJpeg(filePath.toStdString(), frame);
    if (!ok) {
        QFile::remove(filePath);
        QmlNativeAPI::Instance().PutLog(
            "error", "JPEG capture failed while encoding " + filePath.toStdString());
        return "";
    }
    QmlNativeAPI::Instance().PutLog("info", "Saved JPEG capture to " + filePath.toStdString());
    return QDir::toNativeSeparators(filePath);
}

bool QQuickRealTimePlayer::startRecord() {
    if (playStop || !decoder || !decoder->pFormatCtx || !decoder->HasVideo()) {
        QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: video is not ready");
        return false;
    }

    shared_ptr<AVPacket> bootstrapPacket;
    {
        lock_guard<mutex> lock(outputMutex);
        if (_mp4Encoder) {
            QmlNativeAPI::Instance().PutLog("error", "MP4 recording is already running");
            return false;
        }
        bootstrapPacket = _streamBootstrapPacket;
    }
    if (!bootstrapPacket) {
        QmlNativeAPI::Instance().PutLog(
            "error", "MP4 recording failed: wait for a camera keyframe, then try again");
        return false;
    }

    QDir outputDirectory(QDir::current().filePath("mp4"));
    if (!outputDirectory.exists() && !QDir().mkpath(outputDirectory.absolutePath())) {
        QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: unable to create the mp4 folder");
        return false;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const QString filePath = outputDirectory.filePath(QString::number(timestamp) + ".mp4");
    auto encoder = make_shared<Mp4Encoder>(filePath.toStdString());

    if (decoder->HasAudio()) {
        if (!encoder->addTrack(
                decoder->pFormatCtx->streams[decoder->audioStreamIndex], decoder->pAudioCodecCtx)) {
            QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: " + encoder->lastError());
            return false;
        }
    }
    if (!encoder->addTrack(
            decoder->pFormatCtx->streams[decoder->videoStreamIndex], decoder->pVideoCodecCtx)
        || !encoder->primeVideoParameters(bootstrapPacket.get())) {
        QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: " + encoder->lastError());
        return false;
    }
    if (!encoder->start()) {
        QFile::remove(filePath);
        QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: " + encoder->lastError());
        return false;
    }
    {
        lock_guard<mutex> lock(outputMutex);
        _mp4Encoder = std::move(encoder);
    }
    QmlNativeAPI::Instance().PutLog(
        "info", "MP4 recording started; waiting for the next camera keyframe: " + filePath.toStdString());
    return true;
}

QString QQuickRealTimePlayer::stopRecord() {
    lock_guard<mutex> lock(outputMutex);
    if (!_mp4Encoder) {
        return {};
    }
    QString path { _mp4Encoder->_saveFilePath.c_str() };
    const bool ok = _mp4Encoder->stop();
    const std::string error = _mp4Encoder->lastError();
    _mp4Encoder.reset();
    if (!ok) {
        QFile::remove(path);
        QmlNativeAPI::Instance().PutLog("error", "MP4 recording failed: " + error);
        return {};
    }
    QmlNativeAPI::Instance().PutLog("info", "Saved MP4 recording to " + path.toStdString());
    return QDir::toNativeSeparators(path);
}

QString QQuickRealTimePlayer::startStream(const QString &streamUrl) {
    if (!decoder || !decoder->pFormatCtx || (!decoder->HasAudio() && !decoder->HasVideo())) {
        return "Start the receiver and wait for video before streaming.";
    }

    const std::string target = streamUrl.trimmed().toStdString();
    auto publisher = make_shared<StreamPublisher>(target);
    if (decoder->HasVideo()) {
        AVStream *videoStream = decoder->pFormatCtx->streams[decoder->videoStreamIndex];
        QmlNativeAPI::Instance().PutLog(
            "info", "Starting stream output " + target + "; codec "
                + std::string(avcodec_get_name(videoStream->codecpar->codec_id)) + ", time base "
                + std::to_string(videoStream->time_base.num) + "/"
                + std::to_string(videoStream->time_base.den) + ", extradata "
                + std::to_string(videoStream->codecpar->extradata_size) + " bytes");
    }
    if (decoder->HasAudio()
        && !publisher->addTrack(decoder->pFormatCtx->streams[decoder->audioStreamIndex])) {
        return QString::fromStdString(publisher->lastError());
    }
    if (decoder->HasVideo()
        && !publisher->addTrack(decoder->pFormatCtx->streams[decoder->videoStreamIndex])) {
        return QString::fromStdString(publisher->lastError());
    }
    publisher->onStatus = [](const std::string &status) {
        QmlNativeAPI::Instance().PutLog("info", status);
    };
    publisher->onError = [this](const std::string &error) {
        QmlNativeAPI::Instance().PutLog("error", error);
        emit onStreamStopped(QString::fromStdString(error));
    };
    if (!publisher->start()) {
        return QString::fromStdString(publisher->lastError());
    }
    {
        lock_guard<mutex> lock(outputMutex);
        if (_streamPublisher) {
            _streamPublisher->stop();
        }
        if (_streamBootstrapPacket) {
            publisher->enqueuePacket(_streamBootstrapPacket);
            QmlNativeAPI::Instance().PutLog("info", "Injected cached codec headers and random-access frame");
        } else {
            QmlNativeAPI::Instance().PutLog(
                "error", "No cached random-access frame is available yet; restart the receiver before streaming");
        }
        _streamPublisher = std::move(publisher);
    }
    return {};
}

void QQuickRealTimePlayer::stopStream() {
    {
        lock_guard<mutex> lock(outputMutex);
        if (!_streamPublisher) {
            return;
        }
        _streamPublisher->stop();
        _streamPublisher.reset();
    }
    emit onStreamStopped(QString());
}

int QQuickRealTimePlayer::getVideoWidth() {
    if (!decoder) {
        return 0;
    }
    return decoder->width;
}

int QQuickRealTimePlayer::getVideoHeight() {
    if (!decoder) {
        return 0;
    }
    return decoder->height;
}

bool QQuickRealTimePlayer::enableAudio() {
    if (!decoder->HasAudio()) {
        return false;
    }
    // 音频参数
    SDL_AudioSpec audioSpec;
    audioSpec.freq = decoder->GetAudioSampleRate();
    audioSpec.format = AUDIO_S16;
    audioSpec.channels = decoder->GetAudioChannelCount();
    audioSpec.silence = 1;
    audioSpec.samples = decoder->GetAudioFrameSamples();
    audioSpec.padding = 0;
    audioSpec.size = 0;
    audioSpec.userdata = this;
    // 音频样本读取回调
    audioSpec.callback = [](void *Thiz, Uint8 *stream, int len) {
        auto *pThis = static_cast<QQuickRealTimePlayer *>(Thiz);
        SDL_memset(stream, 0, len);
        pThis->decoder->ReadAudioBuff(stream, len);
        if (pThis->isMuted) {
            SDL_memset(stream, 0, len);
        }
    };
    // 关闭音频
    SDL_CloseAudio();
    // 开启声音
    if (SDL_OpenAudio(&audioSpec, nullptr) == 0) {
        // 播放声音
        SDL_PauseAudio(0);
    } else {
        emit onError("开启音频出错，如需听声音请插入音频外设\n" + QString(SDL_GetError()), -1);
        return false;
    }
    return true;
}

void QQuickRealTimePlayer::disableAudio() {
    SDL_CloseAudio();
}

bool QQuickRealTimePlayer::startGifRecord() {
    if (playStop) {
        return false;
    }
    // 保存路径
    stringstream ss;
    ss << QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).toStdString() << "/";
    ss << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count()
       << ".gif";
    if (!(decoder && decoder->HasVideo())) {
        return false;
    }
    // 创建gif编码器
    _gifEncoder = make_shared<GifEncoder>();
    if (!_gifEncoder->open(
            decoder->width, decoder->height, decoder->GetVideoFrameFormat(), DEFAULT_GIF_FRAMERATE, ss.str())) {
        return false;
    }
    // 设置获得解码帧回调
    decoder->_gotFrameCallback = [this](const shared_ptr<AVFrame> &frame) {
        if (!_gifEncoder) {
            return;
        }
        if (!_gifEncoder->isOpened()) {
            return;
        }
        // 根据GIF帧率跳帧
        uint64_t now
            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count();
        if (_gifEncoder->getLastEncodeTime() + 1000 / _gifEncoder->getFrameRate() > now) {
            return;
        }
        // 编码
        _gifEncoder->encodeFrame(frame);
    };

    return true;
}

void QQuickRealTimePlayer::stopGifRecord() {
    decoder->_gotFrameCallback = nullptr;
    if (!_gifEncoder) {
        return;
    }
    _gifEncoder->close();
}
