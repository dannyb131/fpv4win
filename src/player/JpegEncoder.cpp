#include "JpegEncoder.h"

#include <QImage>
#include <QString>
#include <vector>

bool JpegEncoder::encodeJpeg(const string &outFilePath, const shared_ptr<AVFrame> &frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
        return false;
    }

    const int stride = frame->width * 3;
    std::vector<uint8_t> rgb(static_cast<size_t>(stride) * frame->height);
    uint8_t *destinationData[] = { rgb.data(), nullptr, nullptr, nullptr };
    int destinationStride[] = { stride, 0, 0, 0 };

    SwsContext *converter = sws_getContext(
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!converter) {
        return false;
    }

    const int convertedRows = sws_scale(
        converter, frame->data, frame->linesize, 0, frame->height,
        destinationData, destinationStride);
    sws_freeContext(converter);
    if (convertedRows != frame->height) {
        return false;
    }

    const QImage image(rgb.data(), frame->width, frame->height, stride, QImage::Format_RGB888);
    return image.save(QString::fromStdString(outFilePath), "JPEG", 92);
}
