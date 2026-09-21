//
// Created by Talus on 2024/6/10.
//

#include "WFBReceiver.h"
#include "QmlNativeAPI.h"
#ifdef emit
#pragma push_macro("emit")
#undef emit
#define FPV4WIN_RESTORE_QT_EMIT_CPP
#endif
#include "RxFrame.h"
#include "WFBProcessor.h"
#include "WiFiDriver.h"
#include "logger.h"
#ifdef FPV4WIN_RESTORE_QT_EMIT_CPP
#pragma pop_macro("emit")
#undef FPV4WIN_RESTORE_QT_EMIT_CPP
#endif

#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>

#include "Rtp.h"

std::vector<std::string> WFBReceiver::GetDongleList() {
    std::vector<std::string> list;

    libusb_context *findctx;
    // Initialize libusb
    libusb_init(&findctx);

    // Get list of USB devices
    libusb_device **devs;
    ssize_t count = libusb_get_device_list(findctx, &devs);
    if (count < 0) {
        return list;
    }

    // Iterate over devices
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            // Check if the device is using libusb driver
            if (desc.bDeviceClass == LIBUSB_CLASS_PER_INTERFACE) {
                std::stringstream ss;
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idVendor << ":";
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idProduct;
                list.push_back(ss.str());
            }
        }
    }
    std::sort(list.begin(), list.end(), [](std::string &a, std::string &b) {
        static std::vector<std::string> specialStrings = { "0b05:17d2", "0bda:8812", "0bda:881a" };
        auto itA = std::find(specialStrings.begin(), specialStrings.end(), a);
        auto itB = std::find(specialStrings.begin(), specialStrings.end(), b);
        if (itA != specialStrings.end() && itB == specialStrings.end()) {
            return true;
        }
        if (itB != specialStrings.end() && itA == specialStrings.end()) {
            return false;
        }
        return a < b;
    });

    // Free the list of devices
    libusb_free_device_list(devs, 1);

    // Deinitialize libusb
    libusb_exit(findctx);
    return list;
}
bool WFBReceiver::Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &kPath) {

    QmlNativeAPI::Instance().wifiFrameCount_ = 0;
    QmlNativeAPI::Instance().wfbFrameCount_ = 0;
    QmlNativeAPI::Instance().rtpPktCount_ = 0;
    QmlNativeAPI::Instance().telemetryRxCount_ = 0;
    QmlNativeAPI::Instance().telemetryTxCount_ = 0;
    QmlNativeAPI::Instance().UpdateCount();
    mavlinkInspectBuffer.clear();
    observedMavlinkMessageIds.clear();
    mavlinkHeartbeatSeen = false;
    missingHeartbeatWarningLogged = false;
    inspectedTelemetryDatagrams = 0;

    keyPath = kPath;
    if (usbThread) {
        return false;
    }

    constexpr uint32_t linkId = 7669206; // sha1 hash of link_domain="default"
    constexpr uint32_t videoChannelId = (linkId << 8) + 0;
    constexpr uint32_t telemetryDownChannelId = (linkId << 8) + 16;
    try {
        videoAggregator = std::make_unique<Aggregator>(
            keyPath, 0, videoChannelId,
            [](uint8_t *payload, uint16_t packetSize) { WFBReceiver::Instance().handleRtp(payload, packetSize); },
            [](const std::string &level, const std::string &message) {
                QmlNativeAPI::Instance().PutLog(level, message);
            });
        telemetryAggregator = std::make_unique<Aggregator>(
            keyPath, 0, telemetryDownChannelId,
            [](uint8_t *payload, uint16_t packetSize) {
                WFBReceiver::Instance().handleTelemetry(payload, packetSize);
            },
            [](const std::string &level, const std::string &message) {
                QmlNativeAPI::Instance().PutLog(level, "Telemetry: " + message);
            });
    } catch (const std::exception &error) {
        videoAggregator.reset();
        telemetryAggregator.reset();
        QmlNativeAPI::Instance().PutLog(
            "error", "Receiver could not load the encryption key: " + std::string(error.what()));
        return false;
    } catch (...) {
        videoAggregator.reset();
        telemetryAggregator.reset();
        QmlNativeAPI::Instance().PutLog(
            "error", "Receiver could not load the encryption key");
        return false;
    }
    int rc;

    // get vid pid
    std::istringstream iss(vidPid);
    unsigned int wifiDeviceVid, wifiDevicePid;
    char c;
    iss >> std::hex >> wifiDeviceVid >> c >> wifiDevicePid;

    auto logger = std::make_shared<Logger>();

    rc = libusb_init(&ctx);
    if (rc < 0) {
        return false;
    }
    dev_handle = libusb_open_device_with_vid_pid(ctx, wifiDeviceVid, wifiDevicePid);
    if (dev_handle == nullptr) {
        logger->error("Cannot find device {:04x}:{:04x}", wifiDeviceVid, wifiDevicePid);
        libusb_exit(ctx);
        return false;
    }

    /*Check if kenel driver attached*/
    if (libusb_kernel_driver_active(dev_handle, 0)) {
        rc = libusb_detach_kernel_driver(dev_handle, 0); // detach driver
    }
    rc = libusb_claim_interface(dev_handle, 0);

    if (rc < 0) {
        return false;
    }

    usbThread = std::make_shared<std::thread>([=]() {
        WiFiDriver wifi_driver { logger };
        try {
            rtlDevice = wifi_driver.CreateRadio(dev_handle, ctx);
            if (!rtlDevice) {
                throw std::runtime_error("The selected USB adapter is not supported by devourer");
            }
            constexpr uint32_t telemetryUpChannelId = (7669206u << 8) + 144u;
            telemetryTransmitter = std::make_unique<WFBTransmitter>(
                keyPath, 0, telemetryUpChannelId, 1, 2,
                [this](const uint8_t *frame, size_t size) {
                    std::lock_guard<std::mutex> lock(transmitterMutex);
                    return rtlDevice && rtlDevice->send_packet(frame, size);
                });

            telemetryRunning = true;
            telemetryThread = std::make_shared<std::thread>([this]() { telemetryLoop(); });
            QmlNativeAPI::Instance().PutLog(
                "info", "MAVLink bridge ready: WFB stream 16 -> UDP 127.0.0.1:14550; replies -> WFB stream 144");
            rtlDevice->Init(
                [](const Packet &p) {
                    WFBReceiver::Instance().handle80211Frame(p);
                    QmlNativeAPI::Instance().UpdateCount();
                },
                SelectedChannel {
                    .Channel = channel,
                    .ChannelOffset = 0,
                    .ChannelWidth = static_cast<ChannelWidth_t>(channelWidth),
                });
        } catch (const std::runtime_error &e) {
            logger->error(e.what());
            QmlNativeAPI::Instance().PutLog("error", e.what());
        } catch (...) {
        }
        telemetryRunning = false;
        if (telemetryThread && telemetryThread->joinable()) {
            telemetryThread->join();
        }
        telemetryThread.reset();
        telemetryTransmitter.reset();
        rtlDevice.reset();
        auto rc = libusb_release_interface(dev_handle, 0);
        if (rc < 0) {
            // error
        }
        logger->info("==========stoped==========");
        libusb_close(dev_handle);
        libusb_exit(ctx);
        dev_handle = nullptr;
        ctx = nullptr;
        Stop();
        usbThread.reset();
    });
    usbThread->detach();

    return true;
}
void WFBReceiver::handle80211Frame(const Packet &packet) {

    QmlNativeAPI::Instance().wifiFrameCount_++;
    RxFrame frame(packet.Data);
    if (!frame.IsValidWfbFrame()) {
        return;
    }
    QmlNativeAPI::Instance().wfbFrameCount_++;

    static int8_t rssi[4] = { 1, 1, 1, 1 };
    static uint8_t antenna[4] = { 1, 1, 1, 1 };

    constexpr uint32_t linkId = 7669206;
    constexpr uint32_t videoChannelId = (linkId << 8) + 0;
    constexpr uint32_t telemetryChannelId = (linkId << 8) + 16;
    static const uint32_t videoChannelBe = htobe32(videoChannelId);
    static const uint32_t telemetryChannelBe = htobe32(telemetryChannelId);
    const auto *videoChannelBytes = reinterpret_cast<const uint8_t *>(&videoChannelBe);
    const auto *telemetryChannelBytes = reinterpret_cast<const uint8_t *>(&telemetryChannelBe);

    std::lock_guard<std::mutex> lock(aggregatorMutex);
    static std::set<uint32_t> loggedChannelIds;
    uint32_t observedChannelIdBe = 0;
    std::memcpy(&observedChannelIdBe, packet.Data.data() + 12, sizeof(observedChannelIdBe));
    const uint32_t observedChannelId = be32toh(observedChannelIdBe);
    if (loggedChannelIds.size() < 8 && loggedChannelIds.insert(observedChannelId).second) {
        QmlNativeAPI::Instance().PutLog(
            "info", "Observed WFB channel ID " + std::to_string(observedChannelId) + "; expected video ID "
                + std::to_string(videoChannelId));
    }
    const size_t fcsSize = packet.RxAtrib.fcs_present ? 4 : 0;
    if (packet.Data.size() < sizeof(ieee80211_header) + fcsSize) {
        return;
    }
    const size_t wfbSize = packet.Data.size() - sizeof(ieee80211_header) - fcsSize;
    if (frame.MatchesChannelID(videoChannelBytes)) {
        static bool loggedVideoChannel = false;
        if (!loggedVideoChannel) {
            loggedVideoChannel = true;
            QmlNativeAPI::Instance().PutLog(
                "info", "Matched WFB video channel ID " + std::to_string(videoChannelId));
        }
        videoAggregator->process_packet(
            packet.Data.data() + sizeof(ieee80211_header), wfbSize, 0, antenna, rssi);
    } else if (frame.MatchesChannelID(telemetryChannelBytes)) {
        static bool loggedTelemetryChannel = false;
        if (!loggedTelemetryChannel) {
            loggedTelemetryChannel = true;
            QmlNativeAPI::Instance().PutLog(
                "info", "Matched WFB MAVLink downlink channel ID " + std::to_string(telemetryChannelId));
        }
        telemetryAggregator->process_packet(
            packet.Data.data() + sizeof(ieee80211_header), wfbSize, 0, antenna, rssi);
    }
}

static unsigned long long sendFd = INVALID_SOCKET;
static volatile bool playing = false;


namespace {
constexpr uint8_t RTP_PAYLOAD_TYPE_H264 = 96;
constexpr uint8_t RTP_PAYLOAD_TYPE_H265 = 97;
}

void WFBReceiver::handleRtp(uint8_t *payload, uint16_t packet_size) {
    QmlNativeAPI::Instance().rtpPktCount_++;
    QmlNativeAPI::Instance().UpdateCount();
    if (!rtlDevice) {
        return;
    }
    if (packet_size < 12) {
        return;
    }

    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(QmlNativeAPI::Instance().playerPort);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    auto *header = (RtpHeader *)payload;

    if (!playing) {
        playing = true;
        if (QmlNativeAPI::Instance().playerCodec == "AUTO") {
            // OpenIPC assigns fixed RTP payload types to its video codecs. Looking
            // at a single NAL header is unreliable because valid H.264 single-NAL
            // packets are not limited to the aggregation/fragmentation types.
            if (header->pt == RTP_PAYLOAD_TYPE_H264) {
                QmlNativeAPI::Instance().playerCodec = "H264";
            } else if (header->pt == RTP_PAYLOAD_TYPE_H265) {
                QmlNativeAPI::Instance().playerCodec = "H265";
            } else {
                QmlNativeAPI::Instance().PutLog(
                    "error", "Unknown RTP video payload type " + std::to_string(header->pt));
                playing = false;
                return;
            }
            QmlNativeAPI::Instance().PutLog(
                "debug", "RTP payload type " + std::to_string(header->pt) + " selected codec "
                    + QmlNativeAPI::Instance().playerCodec.toStdString());
        }
        QmlNativeAPI::Instance().NotifyRtpStream(header->pt, ntohl(header->ssrc));
    }

    // send video to player
    sendto(
        sendFd, reinterpret_cast<const char *>(payload), packet_size, 0, (sockaddr *)&serverAddr, sizeof(serverAddr));
}

void WFBReceiver::handleTelemetry(uint8_t *payload, uint16_t packetSize) {
    if (telemetryFd == INVALID_SOCKET || !payload || packetSize == 0) {
        return;
    }
    sockaddr_in missionPlanner {};
    missionPlanner.sin_family = AF_INET;
    missionPlanner.sin_port = htons(14550);
    missionPlanner.sin_addr.s_addr = inet_addr("127.0.0.1");
    sendto(
        static_cast<SOCKET>(telemetryFd), reinterpret_cast<const char *>(payload), packetSize, 0,
        reinterpret_cast<sockaddr *>(&missionPlanner), sizeof(missionPlanner));
    inspectMavlink(payload, packetSize);
    const auto count = ++QmlNativeAPI::Instance().telemetryRxCount_;
    QmlNativeAPI::Instance().UpdateCount();
    if (count == 1) {
        QmlNativeAPI::Instance().PutLog("info", "Forwarding MAVLink telemetry to Mission Planner UDP port 14550");
    }
}

void WFBReceiver::inspectMavlink(const uint8_t *payload, size_t size) {
    if (!payload || size == 0) {
        return;
    }
    ++inspectedTelemetryDatagrams;
    mavlinkInspectBuffer.insert(mavlinkInspectBuffer.end(), payload, payload + size);

    while (!mavlinkInspectBuffer.empty()) {
        auto magic = std::find_if(
            mavlinkInspectBuffer.begin(), mavlinkInspectBuffer.end(),
            [](uint8_t byte) { return byte == 0xfe || byte == 0xfd; });
        if (magic != mavlinkInspectBuffer.begin()) {
            mavlinkInspectBuffer.erase(mavlinkInspectBuffer.begin(), magic);
        }
        if (mavlinkInspectBuffer.size() < 8) {
            break;
        }

        const bool mavlink2 = mavlinkInspectBuffer[0] == 0xfd;
        const size_t headerSize = mavlink2 ? 10 : 6;
        if (mavlinkInspectBuffer.size() < headerSize) {
            break;
        }
        const bool signedPacket = mavlink2 && (mavlinkInspectBuffer[2] & 0x01) != 0;
        const size_t frameSize = headerSize + mavlinkInspectBuffer[1] + 2 + (signedPacket ? 13 : 0);
        if (mavlinkInspectBuffer.size() < frameSize) {
            break;
        }

        const uint32_t messageId = mavlink2
            ? static_cast<uint32_t>(mavlinkInspectBuffer[7])
                | (static_cast<uint32_t>(mavlinkInspectBuffer[8]) << 8)
                | (static_cast<uint32_t>(mavlinkInspectBuffer[9]) << 16)
            : mavlinkInspectBuffer[5];
        const uint8_t systemId = mavlink2 ? mavlinkInspectBuffer[5] : mavlinkInspectBuffer[3];
        const uint8_t componentId = mavlink2 ? mavlinkInspectBuffer[6] : mavlinkInspectBuffer[4];

        if (observedMavlinkMessageIds.size() < 16 && observedMavlinkMessageIds.insert(messageId).second) {
            QmlNativeAPI::Instance().PutLog(
                "info", "MAVLink message " + std::to_string(messageId) + " from system "
                    + std::to_string(systemId) + ", component " + std::to_string(componentId));
        }
        if (messageId == 0 && !mavlinkHeartbeatSeen) {
            mavlinkHeartbeatSeen = true;
            QmlNativeAPI::Instance().PutLog(
                "info", "Valid-looking MAVLink heartbeat detected; Mission Planner should now connect");
        }
        mavlinkInspectBuffer.erase(mavlinkInspectBuffer.begin(), mavlinkInspectBuffer.begin() + frameSize);
    }

    if (mavlinkInspectBuffer.size() > 8192) {
        mavlinkInspectBuffer.erase(mavlinkInspectBuffer.begin(), mavlinkInspectBuffer.end() - 1024);
    }
    if (!mavlinkHeartbeatSeen && !missingHeartbeatWarningLogged && inspectedTelemetryDatagrams >= 5) {
        missingHeartbeatWarningLogged = true;
        QmlNativeAPI::Instance().PutLog(
            "warn", "Telemetry is arriving from the air unit, but no flight-controller MAVLink heartbeat was found. "
                    "Check the camera UART selection, RX/TX wiring, flight-controller telemetry port, and 115200 baud.");
    }
}

void WFBReceiver::telemetryLoop() {
    telemetryFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (telemetryFd == INVALID_SOCKET) {
        QmlNativeAPI::Instance().PutLog("error", "Unable to create Mission Planner UDP socket");
        telemetryRunning = false;
        return;
    }
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(14551);
    if (bind(static_cast<SOCKET>(telemetryFd), reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
        // Mission Planner can retain 14551 after reconnecting or when more
        // than one UDP connection is configured.  Binding an ephemeral port
        // is safe: telemetry sent to Mission Planner originates from this
        // same socket, so Mission Planner returns packets to the actual
        // source port automatically.
        local.sin_port = 0;
        if (bind(static_cast<SOCKET>(telemetryFd), reinterpret_cast<sockaddr *>(&local), sizeof(local))
            == SOCKET_ERROR) {
            QmlNativeAPI::Instance().PutLog(
                "error", "Unable to bind any local port for the MAVLink bridge");
            closesocket(static_cast<SOCKET>(telemetryFd));
            telemetryFd = INVALID_SOCKET;
            telemetryRunning = false;
            return;
        }
        sockaddr_in actualLocal {};
        int actualLocalSize = sizeof(actualLocal);
        getsockname(
            static_cast<SOCKET>(telemetryFd), reinterpret_cast<sockaddr *>(&actualLocal), &actualLocalSize);
        QmlNativeAPI::Instance().PutLog(
            "warn", "MAVLink reply port 14551 is already in use; using automatic port "
                + std::to_string(ntohs(actualLocal.sin_port)) + " instead");
    }
    DWORD timeoutMs = 250;
    setsockopt(
        static_cast<SOCKET>(telemetryFd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs),
        sizeof(timeoutMs));

    std::vector<uint8_t> buffer(MAX_PAYLOAD_SIZE);
    while (telemetryRunning) {
        sockaddr_in sender {};
        int senderSize = sizeof(sender);
        const int received = recvfrom(
            static_cast<SOCKET>(telemetryFd), reinterpret_cast<char *>(buffer.data()),
            static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr *>(&sender), &senderSize);
        if (received <= 0 || !telemetryRunning) {
            continue;
        }
        if (telemetryTransmitter && telemetryTransmitter->send(buffer.data(), static_cast<size_t>(received))) {
            const auto count = ++QmlNativeAPI::Instance().telemetryTxCount_;
            QmlNativeAPI::Instance().UpdateCount();
            if (count == 1) {
                QmlNativeAPI::Instance().PutLog("info", "Sent first Mission Planner MAVLink packet to the air unit");
            }
        }
    }
    closesocket(static_cast<SOCKET>(telemetryFd));
    telemetryFd = INVALID_SOCKET;
}

bool WFBReceiver::Stop() {
    playing = false;
    telemetryRunning = false;
    if (rtlDevice) {
        rtlDevice->StopRxLoop();
    }
    QmlNativeAPI::Instance().NotifyWifiStop();

    return true;
}

WFBReceiver::WFBReceiver() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return;
    }
    sendFd = socket(AF_INET, SOCK_DGRAM, 0);
}

WFBReceiver::~WFBReceiver() {
    closesocket(sendFd);
    sendFd = INVALID_SOCKET;
    WSACleanup();
    Stop();
}
