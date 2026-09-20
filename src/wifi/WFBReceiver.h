//
// Created by Talus on 2024/6/10.
//

#ifndef WFBRECEIVER_H
#define WFBRECEIVER_H
#ifdef emit
#pragma push_macro("emit")
#undef emit
#define FPV4WIN_RESTORE_QT_EMIT
#endif
#include "IRadio.h"
#include "RxPacket.h"
#ifdef FPV4WIN_RESTORE_QT_EMIT
#pragma pop_macro("emit")
#undef FPV4WIN_RESTORE_QT_EMIT
#endif
#include "WFBProcessor.h"
#include "WFBTransmitter.h"
#include <QUdpSocket>
#include <atomic>
#include <libusb.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class WFBReceiver {
public:
    WFBReceiver();
    ~WFBReceiver();
    static WFBReceiver &Instance() {
        static WFBReceiver wfb_receiver;
        return wfb_receiver;
    }
    std::vector<std::string> GetDongleList();
    bool Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &keyPath);
    bool Stop();
    void handle80211Frame(const Packet &pkt);
    void handleRtp(uint8_t *payload, uint16_t packet_size);
    void handleTelemetry(uint8_t *payload, uint16_t packet_size);

protected:
    libusb_context *ctx;
    libusb_device_handle *dev_handle;
    std::shared_ptr<std::thread> usbThread;
    std::unique_ptr<IRadio> rtlDevice;
    std::string keyPath;
    std::unique_ptr<Aggregator> videoAggregator;
    std::unique_ptr<Aggregator> telemetryAggregator;
    std::unique_ptr<WFBTransmitter> telemetryTransmitter;
    std::mutex aggregatorMutex;
    std::mutex transmitterMutex;
    std::shared_ptr<std::thread> telemetryThread;
    std::atomic<bool> telemetryRunning { false };
    unsigned long long telemetryFd = static_cast<unsigned long long>(~0ULL);
    void telemetryLoop();
    void inspectMavlink(const uint8_t *payload, size_t size);
    std::vector<uint8_t> mavlinkInspectBuffer;
    std::unordered_set<uint32_t> observedMavlinkMessageIds;
    bool mavlinkHeartbeatSeen = false;
    bool missingHeartbeatWarningLogged = false;
    size_t inspectedTelemetryDatagrams = 0;
};

#endif // WFBRECEIVER_H
