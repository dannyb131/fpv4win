#include "player/JpegEncoder.h"

#include <QCoreApplication>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    auto frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *value) { av_frame_free(&value); });
    if (!frame) {
        return 1;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 128;
    frame->height = 72;
    if (av_frame_get_buffer(frame.get(), 32) < 0 || av_frame_make_writable(frame.get()) < 0) {
        return 2;
    }

    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            frame->data[0][y * frame->linesize[0] + x] = static_cast<uint8_t>(16 + (219 * x / frame->width));
        }
    }
    for (int y = 0; y < frame->height / 2; ++y) {
        std::fill_n(frame->data[1] + y * frame->linesize[1], frame->width / 2, 90);
        std::fill_n(frame->data[2] + y * frame->linesize[2], frame->width / 2, 180);
    }

    const auto outputPath = std::filesystem::temp_directory_path() / "fpv4win-jpeg-encoder-test.jpg";
    if (!JpegEncoder::encodeJpeg(outputPath.string(), frame)) {
        return 3;
    }

    std::ifstream input(outputPath, std::ios::binary);
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(outputPath);
    if (bytes.size() < 100 || bytes[0] != 0xff || bytes[1] != 0xd8
        || bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9) {
        std::cerr << "JPEG output did not contain a complete JPEG bitstream\n";
        return 4;
    }
    return 0;
}
