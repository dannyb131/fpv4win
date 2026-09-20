#include "wifi/WFBProcessor.h"
#include "wifi/WFBTransmitter.h"

#include <sodium.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main() {
    if (sodium_init() < 0) {
        std::cerr << "libsodium initialization failed\n";
        return 1;
    }

    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> groundPublic {};
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> groundSecret {};
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> airPublic {};
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> airSecret {};
    crypto_box_keypair(groundPublic.data(), groundSecret.data());
    crypto_box_keypair(airPublic.data(), airSecret.data());

    const auto temp = std::filesystem::temp_directory_path();
    const auto suffix = std::to_string(randombytes_random());
    const auto groundKeyPath = temp / ("fpv4win-gs-" + suffix + ".key");
    const auto airKeyPath = temp / ("fpv4win-air-" + suffix + ".key");
    {
        std::ofstream groundKey(groundKeyPath, std::ios::binary);
        groundKey.write(reinterpret_cast<const char *>(groundSecret.data()), groundSecret.size());
        groundKey.write(reinterpret_cast<const char *>(airPublic.data()), airPublic.size());
        std::ofstream airKey(airKeyPath, std::ios::binary);
        airKey.write(reinterpret_cast<const char *>(airSecret.data()), airSecret.size());
        airKey.write(reinterpret_cast<const char *>(groundPublic.data()), groundPublic.size());
    }

    const std::vector<uint8_t> expected {
        0xfd, 0x09, 0x00, 0x00, 0x2a, 0xff, 0xbe, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xaa, 0x55
    };
    std::vector<uint8_t> received;
    bool droppedPrimaryFragment = false;
    constexpr uint32_t channelId = (7669206u << 8) + 144u;
    uint8_t antenna[RX_ANT_MAX] {};
    int8_t rssi[RX_ANT_MAX] {};

    int result = 0;
    try {
        Aggregator receiver(
            airKeyPath.string(), 0, channelId,
            [&](uint8_t *payload, uint16_t size) { received.assign(payload, payload + size); });
        WFBTransmitter transmitter(
            groundKeyPath.string(), 0, channelId, 1, 2,
            [&](const uint8_t *frame, size_t size) {
                constexpr size_t headers = sizeof(radiotap_header) + sizeof(ieee80211_header);
                if (size <= headers) {
                    return false;
                }
                const uint8_t *wfbPacket = frame + headers;
                const size_t wfbSize = size - headers;
                if (wfbPacket[0] == WFB_PACKET_DATA && wfbSize >= sizeof(wblock_hdr_t)) {
                    const auto *block = reinterpret_cast<const wblock_hdr_t *>(wfbPacket);
                    const uint8_t fragment = static_cast<uint8_t>(be64toh(block->data_nonce) & 0xff);
                    if (fragment == 0) {
                        droppedPrimaryFragment = true;
                        return true;
                    }
                }
                receiver.process_packet(wfbPacket, wfbSize, 0, antenna, rssi);
                return true;
            });
        if (!transmitter.send(expected.data(), expected.size()) || !droppedPrimaryFragment || received != expected) {
            std::cerr << "WFB telemetry round trip did not reproduce the original payload\n";
            result = 1;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }

    std::error_code ignored;
    std::filesystem::remove(groundKeyPath, ignored);
    std::filesystem::remove(airKeyPath, ignored);
    return result;
}
