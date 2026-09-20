#include "StreamPublisher.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <utility>

namespace {
std::string lowerCase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return value;
}

std::shared_ptr<AVPacket> clonePacket(const AVPacket *packet) {
    return { av_packet_clone(packet), [](AVPacket *value) { av_packet_free(&value); } };
}
}

StreamPublisher::StreamPublisher(std::string url)
    : _url(std::move(url)) {
    avformat_network_init();

    _httpServerMode = lowerCase(_url).rfind("http://", 0) == 0;

    const char *format = formatForUrl(_url);
    if (!format) {
        _lastError = "Unsupported stream URL. Use http://, rtmp://, rtmps://, rtsp://, rtsps://, srt://, or udp://.";
        return;
    }

    int ret = avformat_alloc_output_context2(&_formatCtx, nullptr, format, _url.c_str());
    if (ret < 0 || !_formatCtx) {
        fail(ret < 0 ? ret : AVERROR_UNKNOWN, "Unable to create stream output");
    }
}

StreamPublisher::~StreamPublisher() {
    stop();
    if (_formatCtx) {
        avformat_free_context(_formatCtx);
        _formatCtx = nullptr;
    }
}

const char *StreamPublisher::formatForUrl(const std::string &url) {
    const std::string normalized = lowerCase(url);
    if (normalized.rfind("rtmp://", 0) == 0 || normalized.rfind("rtmps://", 0) == 0) {
        return "flv";
    }
    if (normalized.rfind("rtsp://", 0) == 0 || normalized.rfind("rtsps://", 0) == 0) {
        return "rtsp";
    }
    if (normalized.rfind("srt://", 0) == 0 || normalized.rfind("udp://", 0) == 0) {
        return "mpegts";
    }
    if (normalized.rfind("http://", 0) == 0) {
        return "mpegts";
    }
    return nullptr;
}

bool StreamPublisher::addTrack(AVStream *stream) {
    if (!_formatCtx || _headerWritten || !stream) {
        return false;
    }

    AVStream *output = avformat_new_stream(_formatCtx, nullptr);
    if (!output) {
        _lastError = "Unable to add an output stream.";
        return false;
    }

    int ret = avcodec_parameters_copy(output->codecpar, stream->codecpar);
    if (ret < 0) {
        return fail(ret, "Unable to copy stream parameters");
    }
    output->codecpar->codec_tag = 0;
    output->time_base = stream->time_base;

    _tracks.push_back({ stream->index, output->index, stream->time_base, AV_NOPTS_VALUE });
    _hasVideo = _hasVideo || stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    return true;
}

bool StreamPublisher::start() {
    if (!_formatCtx) {
        return false;
    }
    if (_tracks.empty()) {
        _lastError = "The input has no audio or video tracks to stream.";
        return false;
    }

    _running = true;
    if (!_httpServerMode && !openOutput(false)) {
        _running = false;
        return false;
    }
    _publishThread = std::thread(&StreamPublisher::publishLoop, this);
    return true;
}

bool StreamPublisher::openOutput(bool listenForClient) {
    int ret = 0;
    if (!(_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary *ioOptions = nullptr;
        if (listenForClient) {
            av_dict_set(&ioOptions, "listen", "1", 0);
            av_dict_set(&ioOptions, "listen_timeout", "1000", 0);
            av_dict_set(&ioOptions, "content_type", "video/MP2T", 0);
        }
        ret = avio_open2(&_formatCtx->pb, _url.c_str(), AVIO_FLAG_WRITE, nullptr, &ioOptions);
        av_dict_free(&ioOptions);
        if (ret < 0) {
            if (listenForClient && ret == AVERROR(ETIMEDOUT)) {
                return false;
            }
            return fail(ret, listenForClient ? "Unable to start or accept an HTTP stream client"
                                             : "Unable to connect to the streaming server");
        }
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "muxdelay", "0", 0);
    av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
    _formatCtx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    ret = avformat_write_header(_formatCtx, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if (_formatCtx->pb) {
            avio_closep(&_formatCtx->pb);
        }
        return fail(ret, listenForClient ? "Unable to start HTTP MPEG-TS output"
                                         : "The streaming server rejected the output");
    }

    _headerWritten = true;
    return true;
}

bool StreamPublisher::enqueuePacket(const std::shared_ptr<AVPacket> &packet) {
    if (!_running || !packet) {
        return false;
    }

    auto copy = clonePacket(packet.get());
    if (!copy) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_queue.size() >= MAX_QUEUED_PACKETS) {
            _queue.pop();
        }
        _queue.push({ std::move(copy), packet->stream_index });
    }
    _queueReady.notify_one();
    return true;
}

void StreamPublisher::publishLoop() {
    if (_httpServerMode) {
        while (_running && !_headerWritten) {
            _lastError.clear();
            if (openOutput(true)) {
                break;
            }
            if (!_lastError.empty()) {
                _running = false;
                if (onError) {
                    onError(_lastError);
                }
                return;
            }
        }
        if (!_running) {
            return;
        }
    }

    while (true) {
        QueuedPacket queued;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueReady.wait(lock, [this]() { return !_running || !_queue.empty(); });
            if (!_running && _queue.empty()) {
                break;
            }
            queued = std::move(_queue.front());
            _queue.pop();
        }

        Track *track = findTrack(queued.inputIndex);
        if (!track) {
            continue;
        }

        AVPacket *packet = queued.packet.get();
        const bool isVideo = _formatCtx->streams[track->outputIndex]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        if (_hasVideo && !_writtenKeyFrame) {
            if (!isVideo || !(packet->flags & AV_PKT_FLAG_KEY)) {
                continue;
            }
            _writtenKeyFrame = true;
        }

        const int64_t timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (track->firstTimestamp == AV_NOPTS_VALUE && timestamp != AV_NOPTS_VALUE) {
            track->firstTimestamp = timestamp;
        }
        if (track->firstTimestamp != AV_NOPTS_VALUE) {
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->pts -= track->firstTimestamp;
            }
            if (packet->dts != AV_NOPTS_VALUE) {
                packet->dts -= track->firstTimestamp;
            }
        }

        packet->stream_index = track->outputIndex;
        packet->pos = -1;
        av_packet_rescale_ts(packet, track->inputTimeBase, _formatCtx->streams[track->outputIndex]->time_base);
        const int ret = av_interleaved_write_frame(_formatCtx, packet);
        if (ret < 0) {
            fail(ret, "Streaming stopped while writing data");
            _running = false;
            if (onError) {
                onError(_lastError);
            }
            break;
        }
    }
}

void StreamPublisher::stop() {
    _running = false;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        std::queue<QueuedPacket> empty;
        _queue.swap(empty);
    }
    _queueReady.notify_all();
    if (_publishThread.joinable() && _publishThread.get_id() != std::this_thread::get_id()) {
        _publishThread.join();
    }

    if (_headerWritten && _formatCtx) {
        av_write_trailer(_formatCtx);
        _headerWritten = false;
    }
    if (_formatCtx && _formatCtx->pb && !(_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&_formatCtx->pb);
    }
}

StreamPublisher::Track *StreamPublisher::findTrack(int inputIndex) {
    auto found = std::find_if(_tracks.begin(), _tracks.end(), [inputIndex](const Track &track) {
        return track.inputIndex == inputIndex;
    });
    return found == _tracks.end() ? nullptr : &*found;
}

bool StreamPublisher::fail(int errorCode, const std::string &context) {
    char error[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, error, sizeof(error));
    _lastError = context + ": " + error;
    return false;
}
