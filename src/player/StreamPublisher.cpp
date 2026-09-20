#include "StreamPublisher.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
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
    if (!_httpServerMode && onStatus) {
        onStatus("Streaming output opened: " + _url);
    }
    _publishThread = std::thread(&StreamPublisher::publishLoop, this);
    return true;
}

bool StreamPublisher::openOutput(bool listenForClient) {
    int ret = 0;
    if (!(_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        if (listenForClient && _httpServerMode) {
            if (!openHttpClient()) {
                return false;
            }
        } else {
        AVDictionary *ioOptions = nullptr;
        ret = avio_open2(&_formatCtx->pb, _url.c_str(), AVIO_FLAG_WRITE, nullptr, &ioOptions);
        av_dict_free(&ioOptions);
        if (ret < 0) {
            return fail(ret, listenForClient ? "Unable to start or accept an HTTP stream client"
                                             : "Unable to connect to the streaming server");
        }
        }
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "muxdelay", "0", 0);
    av_dict_set(&options, "flush_packets", "1", 0);
    av_dict_set(&options, "mpegts_flags", "resend_headers+pat_pmt_at_frames+initial_discontinuity", 0);
    av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
    _formatCtx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    ret = avformat_write_header(_formatCtx, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if (_httpServerMode) {
            closeHttpClient();
        } else if (_formatCtx->pb) {
            avio_closep(&_formatCtx->pb);
        }
        return fail(ret, listenForClient ? "Unable to start HTTP MPEG-TS output"
                                         : "The streaming server rejected the output");
    }

    _headerWritten = true;
    return true;
}

bool StreamPublisher::openHttpClient() {
#ifdef _WIN32
    if (!_winsockStarted) {
        WSADATA data {};
        const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
        if (startupResult != 0) {
            return fail(AVERROR(EIO), "Unable to initialise the HTTP streaming socket");
        }
        _winsockStarted = true;
    }

    char protocol[16] = {};
    char authorization[128] = {};
    char hostname[256] = {};
    char path[1024] = {};
    int port = -1;
    av_url_split(protocol, sizeof(protocol), authorization, sizeof(authorization), hostname, sizeof(hostname),
                 &port, path, sizeof(path), _url.c_str());
    if (port < 0) {
        port = 80;
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *addresses = nullptr;
    const std::string portText = std::to_string(port);
    const char *bindHost = (hostname[0] == '\0' || std::strcmp(hostname, "0.0.0.0") == 0) ? nullptr : hostname;
    if (getaddrinfo(bindHost, portText.c_str(), &hints, &addresses) != 0 || !addresses) {
        return fail(AVERROR(EINVAL), "Invalid HTTP streaming address");
    }

    _listenSocket = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    if (_listenSocket == INVALID_SOCKET) {
        freeaddrinfo(addresses);
        return fail(AVERROR(EIO), "Unable to create the HTTP streaming socket");
    }
    BOOL reuseAddress = TRUE;
    setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuseAddress), sizeof(reuseAddress));
    const int bindResult = bind(_listenSocket, addresses->ai_addr, static_cast<int>(addresses->ai_addrlen));
    freeaddrinfo(addresses);
    if (bindResult == SOCKET_ERROR || listen(_listenSocket, 1) == SOCKET_ERROR) {
        const int socketError = WSAGetLastError();
        closeHttpClient();
        return fail(AVERROR(EADDRINUSE), "Unable to listen for an HTTP stream client (Windows socket error "
                                           + std::to_string(socketError) + ")");
    }

    while (_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(_listenSocket, &readSet);
        timeval timeout { 0, 250000 };
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) {
            const int socketError = WSAGetLastError();
            closeHttpClient();
            return fail(AVERROR(EIO), "HTTP listener failed (Windows socket error "
                                       + std::to_string(socketError) + ")");
        }
        if (ready <= 0) {
            continue;
        }

        _clientSocket = accept(_listenSocket, nullptr, nullptr);
        if (_clientSocket == INVALID_SOCKET) {
            continue;
        }

        // Consume and identify the request. VLC and some media frameworks can
        // issue a HEAD probe before opening the real GET connection.
        std::string request;
        char requestBuffer[1024];
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
            fd_set clientReadSet;
            FD_ZERO(&clientReadSet);
            FD_SET(_clientSocket, &clientReadSet);
            timeval requestTimeout { 2, 0 };
            if (select(0, &clientReadSet, nullptr, nullptr, &requestTimeout) <= 0) {
                break;
            }
            const int received = recv(_clientSocket, requestBuffer, sizeof(requestBuffer), 0);
            if (received <= 0) {
                break;
            }
            request.append(requestBuffer, received);
        }

        const bool isHead = request.rfind("HEAD ", 0) == 0;
        const bool isGet = request.rfind("GET ", 0) == 0;
        const char *response = isGet || isHead
            ? "HTTP/1.1 200 OK\r\n"
              "Content-Type: video/MP2T\r\n"
              "Cache-Control: no-cache, no-store\r\n"
              "Connection: close\r\n"
              "Accept-Ranges: none\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "\r\n"
            : "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        const int responseLength = static_cast<int>(std::strlen(response));
        int responseOffset = 0;
        while (responseOffset < responseLength) {
            const int sent = send(_clientSocket, response + responseOffset,
                                  responseLength - responseOffset, 0);
            if (sent == SOCKET_ERROR) {
                break;
            }
            responseOffset += sent;
        }

        if (isGet && responseOffset == responseLength) {
            break;
        }

        shutdown(_clientSocket, SD_BOTH);
        closesocket(_clientSocket);
        _clientSocket = INVALID_SOCKET;
    }
    if (!_running || _clientSocket == INVALID_SOCKET) {
        closeHttpClient();
        return false;
    }

    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(32 * 1024));
    if (!ioBuffer) {
        closeHttpClient();
        return fail(AVERROR(ENOMEM), "Unable to allocate the HTTP stream buffer");
    }
    _httpIoContext = avio_alloc_context(ioBuffer, 32 * 1024, 1, this, nullptr,
                                        &StreamPublisher::writeHttpPacket, nullptr);
    if (!_httpIoContext) {
        av_free(ioBuffer);
        closeHttpClient();
        return fail(AVERROR(ENOMEM), "Unable to create the HTTP stream output");
    }
    _httpIoContext->seekable = 0;
    _formatCtx->pb = _httpIoContext;
    _formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
#else
    return fail(AVERROR(ENOSYS), "Built-in HTTP streaming is currently supported on Windows only");
#endif
}

int StreamPublisher::writeHttpPacket(void *opaque, const uint8_t *buffer, int bufferSize) {
#ifdef _WIN32
    auto *publisher = static_cast<StreamPublisher *>(opaque);
    int totalSent = 0;
    while (totalSent < bufferSize && publisher->_running) {
        const int sent = send(publisher->_clientSocket,
                              reinterpret_cast<const char *>(buffer + totalSent), bufferSize - totalSent, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return AVERROR(ECONNRESET);
        }
        totalSent += sent;
    }
    return totalSent == bufferSize ? totalSent : AVERROR_EXIT;
#else
    return AVERROR(ENOSYS);
#endif
}

void StreamPublisher::closeHttpClient() {
#ifdef _WIN32
    _formatCtx->pb = nullptr;
    if (_httpIoContext) {
        av_freep(&_httpIoContext->buffer);
        avio_context_free(&_httpIoContext);
    }
    if (_clientSocket != INVALID_SOCKET) {
        shutdown(_clientSocket, SD_BOTH);
        closesocket(_clientSocket);
        _clientSocket = INVALID_SOCKET;
    }
    if (_listenSocket != INVALID_SOCKET) {
        closesocket(_listenSocket);
        _listenSocket = INVALID_SOCKET;
    }
#endif
}

bool StreamPublisher::enqueuePacket(const std::shared_ptr<AVPacket> &packet) {
    if (!_running || !packet) {
        return false;
    }

    auto copy = clonePacket(packet.get());
    if (!copy) {
        return false;
    }

    if (!_loggedFirstQueuedPacket.exchange(true) && onStatus) {
        onStatus("Streaming queued first encoded packet: " + std::to_string(packet->size)
                 + " bytes, stream " + std::to_string(packet->stream_index)
                 + ", pts " + std::to_string(packet->pts)
                 + ", dts " + std::to_string(packet->dts)
                 + ", flags " + std::to_string(packet->flags));
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
        if (onStatus) {
            onStatus("HTTP video client accepted; starting MPEG-TS output");
        }

        // Packets can build up while avio_open2() is waiting for an HTTP
        // client.  Sending that backlog adds several seconds of latency and,
        // more importantly, the RTP demuxer does not always preserve the
        // AV_PKT_FLAG_KEY flag for H.265.  Starting with fresh packets lets
        // the MPEG-TS client join the live stream immediately and naturally
        // begin decoding at the camera's next random-access frame.
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            std::queue<QueuedPacket> empty;
            _queue.swap(empty);
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
        const int64_t inputDts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (inputDts != AV_NOPTS_VALUE) {
            int64_t delta = 0;
            if (track->lastInputTimestamp != AV_NOPTS_VALUE) {
                delta = inputDts - track->lastInputTimestamp;
                const int64_t maximumGap = av_rescale_q(1, AVRational { 1, 1 }, track->inputTimeBase);
                if (delta <= 0 || delta > maximumGap) {
                    delta = packet->duration > 0
                        ? packet->duration
                        : std::max<int64_t>(1, av_rescale_q(1, AVRational { 1, 30 }, track->inputTimeBase));
                }
                track->outputTimestamp += delta;
            }
            const int64_t compositionOffset = packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE
                ? packet->pts - packet->dts
                : 0;
            packet->dts = track->outputTimestamp;
            packet->pts = track->outputTimestamp + compositionOffset;
            track->lastInputTimestamp = inputDts;
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
        if (!_loggedFirstWrittenPacket.exchange(true) && onStatus) {
            onStatus("Streaming wrote first MPEG-TS packet successfully");
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
    if (_httpServerMode) {
        closeHttpClient();
    } else if (_formatCtx && _formatCtx->pb && !(_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&_formatCtx->pb);
    }
#ifdef _WIN32
    if (_winsockStarted) {
        WSACleanup();
        _winsockStarted = false;
    }
#endif
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
