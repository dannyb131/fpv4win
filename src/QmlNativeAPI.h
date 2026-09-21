//
// Created by liangzhuohua on 2022/4/20.
//

#ifndef CTRLCENTER_QMLNATIVEAPI_H
#define CTRLCENTER_QMLNATIVEAPI_H
#include "wifi/WFBReceiver.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QObject>
#include <QUdpSocket>
#include <fstream>
#include <future>
#include <mutex>
#include <util/mini.h>

using namespace toolkit;

#define CONFIG "config."
#define CONFIG_FILE "config.ini"
#define CONFIG_CHANNEL CONFIG "channel"
#define CONFIG_CHANNEL_WIDTH CONFIG "channelWidth"
#define CONFIG_CHANNEL_KEY CONFIG "key"
#define CONFIG_CHANNEL_CODEC CONFIG "codec"

/**
 * C++封装留给qml使用的api
 */
class QmlNativeAPI : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong wifiFrameCount READ wifiFrameCount NOTIFY onWifiFrameCount)
    Q_PROPERTY(qulonglong wfbFrameCount READ wfbFrameCount NOTIFY onWfbFrameCount)
    Q_PROPERTY(qulonglong rtpPktCount READ rtpPktCount NOTIFY onRtpPktCount)
    Q_PROPERTY(qulonglong telemetryRxCount READ telemetryRxCount NOTIFY onTelemetryRxCount)
    Q_PROPERTY(qulonglong telemetryTxCount READ telemetryTxCount NOTIFY onTelemetryTxCount)
public:
    static QmlNativeAPI &Instance() {
        static QmlNativeAPI api;
        return api;
    }
    explicit QmlNativeAPI(QObject *parent = nullptr)
        : QObject(parent)
        , logFilePath_(QDir(QCoreApplication::applicationDirPath()).filePath("fpv4win.log")) {
        // Start each application run with a fresh, easy-to-share log file.
        std::ofstream(logFilePath_.toStdString(), std::ios::trunc);
        // load config
        try {
            mINI::Instance().parseFile(CONFIG_FILE);
        } catch (...) {
        }
    };
    // Get config
    Q_INVOKABLE QJsonObject GetConfig() {
        QJsonObject config;
        for (const auto &item : mINI::Instance()) {
            config[QString(item.first.c_str())] = QString(item.second.c_str());
        }
        return config;
    }
    // get all dongle
    Q_INVOKABLE static QList<QString> GetDongleList() {
        QList<QString> l;
        for (auto &item : WFBReceiver::Instance().GetDongleList()) {
            l.push_back(QString(item.c_str()));
        }
        return l;
    };
    Q_INVOKABLE static bool
    Start(const QString &vidPid, int channel, int channelWidth, const QString &keyPath, const QString &codec) {
        const QFileInfo keyFile(keyPath.trimmed());
        if (!keyFile.exists() || !keyFile.isFile() || !keyFile.isReadable()) {
            QmlNativeAPI::Instance().PutLog(
                "error", "Receiver could not start: select a readable OpenIPC gs.key file");
            return false;
        }
        // save config
        mINI::Instance()[CONFIG_CHANNEL] = channel;
        mINI::Instance()[CONFIG_CHANNEL_WIDTH] = channelWidth;
        mINI::Instance()[CONFIG_CHANNEL_KEY] = keyPath.toStdString();
        mINI::Instance()[CONFIG_CHANNEL_CODEC] = codec.toStdString();
        mINI::Instance().dumpFile(CONFIG_FILE);
        // alloc port
        QmlNativeAPI::Instance().playerPort = QmlNativeAPI::Instance().GetFreePort();
        QmlNativeAPI::Instance().playerCodec = codec;
        return WFBReceiver::Instance().Start(vidPid.toStdString(), channel, channelWidth, keyPath.toStdString());
    }
    Q_INVOKABLE static bool Stop() {
        std::async([]() { WFBReceiver::Instance().Stop(); });
        return true;
    }
    Q_INVOKABLE static void BuildSdp(const QString &filePath, const QString &codec, int payloadType, int port) {
        QString dirPath = QFileInfo(filePath).absolutePath();
        QDir dir(dirPath);
        if (!dir.exists()) {
            dir.mkpath(dirPath);
        }
        std::ofstream sdpFos(filePath.toStdString());
        sdpFos << "v=0\n";
        sdpFos << "o=- 0 0 IN IP4 127.0.0.1\n";
        sdpFos << "s=No Name\n";
        sdpFos << "c=IN IP4 127.0.0.1\n";
        sdpFos << "t=0 0\n";
        sdpFos << "m=video " << port << " RTP/AVP " << payloadType << "\n";
        sdpFos << "a=rtpmap:" << payloadType << " " << codec.toStdString() << "/90000\n";
        sdpFos.flush();
        sdpFos.close();
        // log
        QmlNativeAPI::Instance().PutLog(
            "debug",
            "Build Player SDP: Codec:" + codec.toStdString() + " PT:" + std::to_string(payloadType)
                + " Port:" + std::to_string(port));
    }
    void PutLog(const std::string &level, const std::string &msg) {
        {
            std::lock_guard<std::mutex> lock(logFileMutex_);
            std::ofstream logFile(logFilePath_.toStdString(), std::ios::app);
            logFile << '[' << level << "] " << msg << '\n';
        }
        emit onLog(QString(level.c_str()), QString(msg.c_str()));
    }
    Q_INVOKABLE QString GetLogFilePath() const { return QDir::toNativeSeparators(logFilePath_); }
    void NotifyWifiStop() { emit onWifiStop(); }
    int NotifyRtpStream(int pt, uint16_t ssrc) {
        // get free port
        const QString sdpFile = "sdp/sdp.sdp";
        BuildSdp(sdpFile, playerCodec, pt, playerPort);
        emit onRtpStream(sdpFile);
        return QmlNativeAPI::Instance().playerPort;
    }
    void UpdateCount() {
        emit onWifiFrameCount(wifiFrameCount_);
        emit onWfbFrameCount(wfbFrameCount_);
        emit onRtpPktCount(rtpPktCount_);
        emit onTelemetryRxCount(telemetryRxCount_);
        emit onTelemetryTxCount(telemetryTxCount_);
    }
    qulonglong wfbFrameCount() { return wfbFrameCount_; }
    qulonglong rtpPktCount() { return rtpPktCount_; }
    qulonglong wifiFrameCount() { return wifiFrameCount_; }
    qulonglong telemetryRxCount() { return telemetryRxCount_; }
    qulonglong telemetryTxCount() { return telemetryTxCount_; }
    Q_INVOKABLE int GetPlayerPort() { return playerPort; }
    Q_INVOKABLE QString GetPlayerCodec() const { return playerCodec; }
    int GetFreePort() { return 52356; }
    qulonglong wfbFrameCount_ = 0;
    qulonglong wifiFrameCount_ = 0;
    qulonglong rtpPktCount_ = 0;
    std::atomic<qulonglong> telemetryRxCount_ { 0 };
    std::atomic<qulonglong> telemetryTxCount_ { 0 };
    int playerPort = 0;
    QString playerCodec;
private:
    QString logFilePath_;
    std::mutex logFileMutex_;

signals:
    // onlog
    void onLog(QString level, QString msg);
    void onWifiStop();
    void onWifiFrameCount(qulonglong count);
    void onWfbFrameCount(qulonglong count);
    void onRtpPktCount(qulonglong count);
    void onTelemetryRxCount(qulonglong count);
    void onTelemetryTxCount(qulonglong count);
    void onRtpStream(QString sdp);
};

#endif // CTRLCENTER_QMLNATIVEAPI_H
