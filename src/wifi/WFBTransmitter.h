#pragma once

#include "WFBDefine.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class WFBTransmitter {
public:
    using InjectCallback = std::function<bool(const uint8_t *, size_t)>;

    WFBTransmitter(
        const std::string &keyPath, uint64_t epoch, uint32_t channelId, int fecK, int fecN,
        InjectCallback inject);
    ~WFBTransmitter();

    bool send(const uint8_t *payload, size_t size);

private:
    void initialiseSession();
    bool announceSession();
    bool sendFragment(size_t payloadSize);
    bool injectWfb(const uint8_t *payload, size_t size);

    std::mutex mutex_;
    InjectCallback inject_;
    fec_t *fec_ = nullptr;
    int fecK_;
    int fecN_;
    uint64_t epoch_;
    uint32_t channelId_;
    uint64_t blockIndex_ = 0;
    uint8_t fragmentIndex_ = 0;
    size_t maxPacketSize_ = 0;
    uint16_t wifiSequence_ = 0;
    uint64_t lastSessionAnnouncementMs_ = 0;
    std::vector<std::vector<uint8_t>> blocks_;
    std::vector<uint8_t *> blockPointers_;
    uint8_t txSecretKey_[crypto_box_SECRETKEYBYTES] {};
    uint8_t rxPublicKey_[crypto_box_PUBLICKEYBYTES] {};
    uint8_t sessionKey_[crypto_aead_chacha20poly1305_KEYBYTES] {};
    std::vector<uint8_t> sessionPacket_;
};
