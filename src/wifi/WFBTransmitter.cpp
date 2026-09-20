#include "WFBTransmitter.h"

#include <QDateTime>
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
uint64_t hostToBigEndian64(uint64_t value) {
#ifdef _WIN32
    return _byteswap_uint64(value);
#else
    return htobe64(value);
#endif
}

uint16_t hostToBigEndian16(uint16_t value) {
#ifdef _WIN32
    return _byteswap_ushort(value);
#else
    return htobe16(value);
#endif
}
}

WFBTransmitter::WFBTransmitter(
    const std::string &keyPath, uint64_t epoch, uint32_t channelId, int fecK, int fecN,
    InjectCallback inject)
    : inject_(std::move(inject))
    , fecK_(fecK)
    , fecN_(fecN)
    , epoch_(epoch)
    , channelId_(channelId) {
    if (fecK_ < 1 || fecN_ < fecK_ || fecN_ > 255) {
        throw std::runtime_error("Invalid telemetry uplink FEC settings");
    }

    FILE *file = fopen(keyPath.c_str(), "rb");
    if (!file) {
        throw std::runtime_error("Unable to open WFB key for telemetry transmission");
    }
    const bool keyOk = fread(txSecretKey_, sizeof(txSecretKey_), 1, file) == 1
        && fread(rxPublicKey_, sizeof(rxPublicKey_), 1, file) == 1;
    fclose(file);
    if (!keyOk) {
        throw std::runtime_error("Unable to read WFB telemetry transmission key");
    }

    fec_ = fec_new(fecK_, fecN_);
    blocks_.resize(fecN_, std::vector<uint8_t>(MAX_FEC_PAYLOAD));
    blockPointers_.reserve(fecN_);
    for (auto &block : blocks_) {
        blockPointers_.push_back(block.data());
    }
    initialiseSession();
}

WFBTransmitter::~WFBTransmitter() {
    if (fec_) {
        fec_free(fec_);
    }
}

void WFBTransmitter::initialiseSession() {
    randombytes_buf(sessionKey_, sizeof(sessionKey_));
    sessionPacket_.resize(sizeof(wsession_hdr_t) + sizeof(wsession_data_t) + crypto_box_MACBYTES);
    auto *header = reinterpret_cast<wsession_hdr_t *>(sessionPacket_.data());
    header->packet_type = WFB_PACKET_KEY;
    randombytes_buf(header->session_nonce, sizeof(header->session_nonce));

    wsession_data_t data {};
    data.epoch = hostToBigEndian64(epoch_);
    data.channel_id = htobe32(channelId_);
    data.fec_type = WFB_FEC_VDM_RS;
    data.k = static_cast<uint8_t>(fecK_);
    data.n = static_cast<uint8_t>(fecN_);
    std::memcpy(data.session_key, sessionKey_, sizeof(sessionKey_));

    if (crypto_box_easy(
            sessionPacket_.data() + sizeof(wsession_hdr_t), reinterpret_cast<const uint8_t *>(&data), sizeof(data),
            header->session_nonce, rxPublicKey_, txSecretKey_)
        != 0) {
        throw std::runtime_error("Unable to encrypt WFB telemetry session");
    }
    blockIndex_ = 0;
    fragmentIndex_ = 0;
    maxPacketSize_ = 0;
    lastSessionAnnouncementMs_ = 0;
}

bool WFBTransmitter::injectWfb(const uint8_t *payload, size_t size) {
    std::vector<uint8_t> frame;
    frame.reserve(sizeof(radiotap_header) + sizeof(ieee80211_header) + size);

    std::array<uint8_t, sizeof(radiotap_header)> radio {};
    std::memcpy(radio.data(), radiotap_header, radio.size());
    radio[11] = IEEE80211_RADIOTAP_MCS_BW_20;
    radio[12] = 3; // Robust 5 GHz HT MCS3; avoids the invalid 1 Mbps CCK fallback.
    frame.insert(frame.end(), radio.begin(), radio.end());

    std::array<uint8_t, sizeof(ieee80211_header)> wifi {};
    std::memcpy(wifi.data(), ieee80211_header, wifi.size());
    const uint32_t channelBe = htobe32(channelId_);
    std::memcpy(wifi.data() + 12, &channelBe, sizeof(channelBe));
    std::memcpy(wifi.data() + 18, &channelBe, sizeof(channelBe));
    const uint16_t sequence = static_cast<uint16_t>((wifiSequence_++ & 0x0fff) << 4);
    wifi[22] = static_cast<uint8_t>(sequence & 0xff);
    wifi[23] = static_cast<uint8_t>(sequence >> 8);
    frame.insert(frame.end(), wifi.begin(), wifi.end());
    frame.insert(frame.end(), payload, payload + size);
    return inject_(frame.data(), frame.size());
}

bool WFBTransmitter::announceSession() {
    if (!injectWfb(sessionPacket_.data(), sessionPacket_.size())) {
        return false;
    }
    lastSessionAnnouncementMs_ = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    return true;
}

bool WFBTransmitter::sendFragment(size_t payloadSize) {
    std::vector<uint8_t> encrypted(sizeof(wblock_hdr_t) + payloadSize + crypto_aead_chacha20poly1305_ABYTES);
    auto *header = reinterpret_cast<wblock_hdr_t *>(encrypted.data());
    header->packet_type = WFB_PACKET_DATA;
    header->data_nonce = hostToBigEndian64(((blockIndex_ & BLOCK_IDX_MASK) << 8) + fragmentIndex_);
    unsigned long long encryptedSize = 0;
    if (crypto_aead_chacha20poly1305_encrypt(
            encrypted.data() + sizeof(wblock_hdr_t), &encryptedSize, blocks_[fragmentIndex_].data(), payloadSize,
            encrypted.data(), sizeof(wblock_hdr_t), nullptr, reinterpret_cast<uint8_t *>(&header->data_nonce),
            sessionKey_)
        != 0) {
        return false;
    }
    return injectWfb(encrypted.data(), sizeof(wblock_hdr_t) + encryptedSize);
}

bool WFBTransmitter::send(const uint8_t *payload, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!payload || size == 0 || size > MAX_PAYLOAD_SIZE) {
        return false;
    }
    const uint64_t now = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    if (lastSessionAnnouncementMs_ == 0 || now - lastSessionAnnouncementMs_ >= SESSION_KEY_ANNOUNCE_MSEC) {
        if (!announceSession()) {
            return false;
        }
    }

    auto *packet = reinterpret_cast<wpacket_hdr_t *>(blocks_[fragmentIndex_].data());
    packet->flags = 0;
    packet->packet_size = hostToBigEndian16(static_cast<uint16_t>(size));
    std::memcpy(blocks_[fragmentIndex_].data() + sizeof(wpacket_hdr_t), payload, size);
    std::memset(
        blocks_[fragmentIndex_].data() + sizeof(wpacket_hdr_t) + size, 0,
        MAX_FEC_PAYLOAD - sizeof(wpacket_hdr_t) - size);

    const size_t encodedSize = sizeof(wpacket_hdr_t) + size;
    bool ok = sendFragment(encodedSize);
    maxPacketSize_ = std::max(maxPacketSize_, encodedSize);
    ++fragmentIndex_;
    if (fragmentIndex_ < fecK_) {
        return ok;
    }

    std::vector<const gf *> sourceBlocks;
    sourceBlocks.reserve(fecK_);
    for (int i = 0; i < fecK_; ++i) {
        sourceBlocks.push_back(blockPointers_[i]);
    }
    fec_encode(fec_, sourceBlocks.data(), blockPointers_.data() + fecK_, maxPacketSize_);
    while (fragmentIndex_ < fecN_) {
        ok = sendFragment(maxPacketSize_) && ok;
        ++fragmentIndex_;
    }
    ++blockIndex_;
    fragmentIndex_ = 0;
    maxPacketSize_ = 0;
    if (blockIndex_ > MAX_BLOCK_IDX) {
        initialiseSession();
    }
    return ok;
}
