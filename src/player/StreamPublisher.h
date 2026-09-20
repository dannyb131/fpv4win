#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "ffmpegInclude.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class StreamPublisher {
public:
    explicit StreamPublisher(std::string url);
    ~StreamPublisher();

    bool addTrack(AVStream *stream);
    bool start();
    void stop();
    bool enqueuePacket(const std::shared_ptr<AVPacket> &packet);

    bool isRunning() const { return _running; }
    const std::string &lastError() const { return _lastError; }

    std::function<void(const std::string &)> onError;
    std::function<void(const std::string &)> onStatus;

private:
    struct Track {
        int inputIndex = -1;
        int outputIndex = -1;
        AVRational inputTimeBase {};
        int64_t lastInputTimestamp = AV_NOPTS_VALUE;
        int64_t outputTimestamp = 0;
    };

    struct QueuedPacket {
        std::shared_ptr<AVPacket> packet;
        int inputIndex = -1;
    };

    void publishLoop();
    bool openOutput(bool listenForClient);
    bool openHttpClient();
    void closeHttpClient();
    static int writeHttpPacket(void *opaque, const uint8_t *buffer, int bufferSize);
    Track *findTrack(int inputIndex);
    bool fail(int errorCode, const std::string &context);
    static const char *formatForUrl(const std::string &url);

    std::string _url;
    std::string _lastError;
    AVFormatContext *_formatCtx = nullptr;
    std::vector<Track> _tracks;
    std::queue<QueuedPacket> _queue;
    std::mutex _queueMutex;
    std::condition_variable _queueReady;
    std::thread _publishThread;
    std::atomic_bool _running { false };
    std::atomic_bool _loggedFirstQueuedPacket { false };
    std::atomic_bool _loggedFirstWrittenPacket { false };
    bool _headerWritten = false;
    bool _hasVideo = false;
    bool _httpServerMode = false;
    AVIOContext *_httpIoContext = nullptr;
#ifdef _WIN32
    SOCKET _listenSocket = INVALID_SOCKET;
    SOCKET _clientSocket = INVALID_SOCKET;
    bool _winsockStarted = false;
#endif

    static constexpr size_t MAX_QUEUED_PACKETS = 512;
};
