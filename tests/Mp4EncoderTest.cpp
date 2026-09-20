#include "player/Mp4Encoder.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

int main() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) {
        return 1;
    }
    auto codecContext = std::shared_ptr<AVCodecContext>(
        avcodec_alloc_context3(codec), [](AVCodecContext *value) { avcodec_free_context(&value); });
    codecContext->width = 128;
    codecContext->height = 72;
    codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    codecContext->time_base = AVRational { 1, 30 };
    codecContext->framerate = AVRational { 30, 1 };
    codecContext->gop_size = 1;
    codecContext->max_b_frames = 0;
    if (avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
        return 2;
    }

    auto sourceContext = std::shared_ptr<AVFormatContext>(
        avformat_alloc_context(), [](AVFormatContext *value) { avformat_free_context(value); });
    AVStream *sourceStream = avformat_new_stream(sourceContext.get(), nullptr);
    sourceStream->time_base = codecContext->time_base;
    if (avcodec_parameters_from_context(sourceStream->codecpar, codecContext.get()) < 0) {
        return 3;
    }

    std::vector<std::shared_ptr<AVPacket>> packets;
    for (int frameNumber = 0; frameNumber < 4; ++frameNumber) {
        auto frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *value) { av_frame_free(&value); });
        frame->format = codecContext->pix_fmt;
        frame->width = codecContext->width;
        frame->height = codecContext->height;
        frame->pts = frameNumber;
        if (av_frame_get_buffer(frame.get(), 32) < 0 || av_frame_make_writable(frame.get()) < 0) {
            return 4;
        }
        for (int y = 0; y < frame->height; ++y) {
            std::fill_n(frame->data[0] + y * frame->linesize[0], frame->width,
                        static_cast<uint8_t>(32 + frameNumber * 40));
        }
        for (int y = 0; y < frame->height / 2; ++y) {
            std::fill_n(frame->data[1] + y * frame->linesize[1], frame->width / 2, 100);
            std::fill_n(frame->data[2] + y * frame->linesize[2], frame->width / 2, 150);
        }
        if (avcodec_send_frame(codecContext.get(), frame.get()) < 0) {
            return 5;
        }
        while (true) {
            auto packet = std::shared_ptr<AVPacket>(
                av_packet_alloc(), [](AVPacket *value) { av_packet_free(&value); });
            const int result = avcodec_receive_packet(codecContext.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                return 6;
            }
            packet->stream_index = sourceStream->index;
            packets.push_back(std::move(packet));
        }
    }
    if (packets.empty()) {
        return 7;
    }

    const auto outputPath = std::filesystem::temp_directory_path() / "fpv4win-mp4-encoder-test.mp4";
    Mp4Encoder recorder(outputPath.string());
    if (!recorder.addTrack(sourceStream, codecContext.get())
        || !recorder.primeVideoParameters(packets.front().get())
        || !recorder.start()) {
        return 8;
    }
    for (const auto &packet : packets) {
        recorder.writePacket(packet, true);
    }
    if (!recorder.stop()) {
        return 9;
    }

    AVFormatContext *probe = nullptr;
    if (avformat_open_input(&probe, outputPath.string().c_str(), nullptr, nullptr) < 0) {
        return 10;
    }
    const int streamInfoResult = avformat_find_stream_info(probe, nullptr);
    const int videoStream = av_find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    avformat_close_input(&probe);
    std::filesystem::remove(outputPath);
    return streamInfoResult >= 0 && videoStream >= 0 ? 0 : 11;
}
