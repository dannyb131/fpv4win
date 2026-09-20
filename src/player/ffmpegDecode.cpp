#include "ffmpegDecode.h"
#include <QDateTime>
#include <QThread>
#include <iostream>
#include <vector>

#define MAX_AUDIO_PACKET (2 * 1024 * 1024)

bool FFmpegDecoder::OpenInput(string &inputFile) {
    CloseInput();

    // Prefer Windows D3D11 hardware decoding. OpenVideo() falls back to the
    // normal software decoder if the codec or GPU cannot provide it.
    hwDecoderType = AV_HWDEVICE_TYPE_D3D11VA;
    hwPixFmt = AV_PIX_FMT_NONE;
    isHwDecoderEnable = true;

    AVDictionary *param = nullptr;

    av_dict_set(&param, "preset", "ultrafast", 0);
    av_dict_set(&param, "tune", "zerolatency", 0);
    av_dict_set(&param, "buffer_size", "425984", 0);
    av_dict_set(&param, "fflags", "nobuffer", 0);
    av_dict_set(&param, "flags", "low_delay", 0);
    av_dict_set(&param, "rtsp_transport", "tcp", 0);
    av_dict_set(&param, "protocol_whitelist", "file,udp,tcp,rtp,rtmp,rtsp,http", 0);

    // 打开输入
    if (avformat_open_input(&pFormatCtx, inputFile.c_str(), nullptr, &param) != 0) {
        CloseInput();
        return false;
    }
    // 超时机制
    static const int timeout = 10;
    auto startTime = std::make_shared<uint64_t>();
    *startTime = QDateTime::currentSecsSinceEpoch();
    pFormatCtx->interrupt_callback.callback = [](void *ctx) -> int {
        uint64_t now = QDateTime::currentSecsSinceEpoch();
        return now - *(uint64_t *)ctx > timeout;
    };
    pFormatCtx->interrupt_callback.opaque = startTime.get();

    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
        CloseInput();
        return false;
    }

    // 分析超时，退出，可能格式不正确
    if (QDateTime::currentSecsSinceEpoch() - *startTime > timeout) {
        CloseInput();
        return false;
    }
    pFormatCtx->interrupt_callback.callback = nullptr;
    pFormatCtx->interrupt_callback.opaque = nullptr;

    // 打开视频/音频输入
    hasVideoStream = OpenVideo();
    hasAudioStream = OpenAudio();

    isOpen = true;

    // 转换时间基
    if (videoStreamIndex != -1) {
        videoFramePerSecond = av_q2d(pFormatCtx->streams[videoStreamIndex]->r_frame_rate);
        videoBaseTime = av_q2d(pFormatCtx->streams[videoStreamIndex]->time_base);
    }
    if (audioStreamIndex != -1) {
        audioBaseTime = av_q2d(pFormatCtx->streams[audioStreamIndex]->time_base);
    }

    // 创建音频解码缓存
    if (hasAudioStream) {
        audioFifoBuffer = shared_ptr<AVFifo>(
            av_fifo_alloc2(0, GetAudioFrameSamples() * GetAudioChannelCount() * 10, AV_FIFO_FLAG_AUTO_GROW));
    }
    return true;
}

bool FFmpegDecoder::CloseInput() {
    isOpen = false;

    lock_guard<mutex> lck(_releaseLock);

    // 关闭流
    CloseVideo();
    CloseAudio();
    if (pFormatCtx) {
        avformat_close_input(&pFormatCtx);
        pFormatCtx = nullptr;
    }

    return true;
}

void freeFrame(AVFrame *f) {
    av_frame_free(&f);
}
void freePkt(AVPacket *f) {
    av_packet_free(&f);
}
void freeSwrCtx(SwrContext *s) {
    swr_free(&s);
}

shared_ptr<AVFrame> FFmpegDecoder::GetNextFrame() {
    // 加锁，避免在此方法执行过程中解码器释放，导致崩溃
    lock_guard<mutex> lck(_releaseLock);
    shared_ptr<AVFrame> res;
    if (videoStreamIndex == -1 && audioStreamIndex == -1) {
        return res;
    }
    if (!isOpen) {
        return res;
    }

    // 读输入流
    while (true) {
        if (!pFormatCtx) {
            throw runtime_error("分配解析器出错");
        }
        shared_ptr<AVPacket> packet = shared_ptr<AVPacket>(av_packet_alloc(), &freePkt);
        int ret = av_read_frame(pFormatCtx, packet.get());
        if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("解析视频出错 " + string(errStr));
        }
        // 计算码率
        {
            bytesSecond += packet->size;
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
            if (now - lastCountBitrateTime >= 1000) {
                // 计算码率定时器
                bitrate = bytesSecond * 8 * 1000 / (now - lastCountBitrateTime);
                bytesSecond = 0;
                if (onBitrate) {
                    onBitrate(bitrate);
                }
                lastCountBitrateTime = now;
            }
        }
        if (packet->stream_index == videoStreamIndex) {
            // 回调nalu
            if (_gotPktCallback) {
                _gotPktCallback(packet);
            }
            // 处理视频数据
            shared_ptr<AVFrame> pVideoYuv = shared_ptr<AVFrame>(av_frame_alloc(), &freeFrame);
            // 解码视频祯
            bool isDecodeComplite = DecodeVideo(packet.get(), pVideoYuv);
            if (isDecodeComplite) {
                res = pVideoYuv;
            }
            // 回调frame
            if (_gotFrameCallback) {
                _gotFrameCallback(pVideoYuv);
            }
            break;
        } else if (packet->stream_index == audioStreamIndex) {
            // 回调nalu
            if (_gotPktCallback) {
                _gotPktCallback(packet);
            }
            // 处理音频数据
            if (packet->dts != AV_NOPTS_VALUE) {
                int audioFrameSize = MAX_AUDIO_PACKET;
                shared_ptr<uint8_t> pFrameAudio = shared_ptr<uint8_t>(new uint8_t[audioFrameSize]);
                // 解码音频祯
                int nDecodedSize = DecodeAudio(audioStreamIndex, packet.get(), pFrameAudio.get(), audioFrameSize);
                // 解码成功，解码数据写入音频缓存
                if (nDecodedSize > 0) {
                    writeAudioBuff(pFrameAudio.get(), nDecodedSize);
                }
            }
            if (!HasVideo()) {
                return res;
            }
        }
    }
    return res;
}

bool FFmpegDecoder::hwDecoderInit(AVCodecContext *ctx, const enum AVHWDeviceType type) {
    if (av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
        return false;
    }
    ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);

    return true;
}

enum AVPixelFormat FFmpegDecoder::selectHardwareFormat(
    AVCodecContext *ctx, const enum AVPixelFormat *formats) {
    auto *decoder = static_cast<FFmpegDecoder *>(ctx->opaque);
    if (!decoder) {
        return formats[0];
    }

    for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == decoder->hwPixFmt) {
            return *format;
        }
    }

    return AV_PIX_FMT_NONE;
}

bool FFmpegDecoder::OpenVideo() {
    bool res = false;

    if (pFormatCtx) {
        videoStreamIndex = -1;

        for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = i;
                const AVCodec *codec = avcodec_find_decoder(pFormatCtx->streams[i]->codecpar->codec_id);

                // 如果有存在视频，检测硬件解码器
                if (codec && isHwDecoderEnable) {
                    for (int configIndex = 0;; configIndex++) {
                        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, configIndex);
                        if (!config) {
                            isHwDecoderEnable = false;
                            break;
                        }
                        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                            && config->device_type == hwDecoderType) {
                            hwPixFmt = config->pix_fmt;
                            break;
                        }
                    }
                }

                if (codec) {
                    pVideoCodecCtx = avcodec_alloc_context3(codec);
                    if (pVideoCodecCtx) {
                        if (isHwDecoderEnable) {
                            isHwDecoderEnable = hwDecoderInit(pVideoCodecCtx, hwDecoderType);
                            if (isHwDecoderEnable) {
                                pVideoCodecCtx->opaque = this;
                                pVideoCodecCtx->get_format = &FFmpegDecoder::selectHardwareFormat;
                            }
                        }

                        if (avcodec_parameters_to_context(pVideoCodecCtx, pFormatCtx->streams[i]->codecpar) >= 0) {
                            pVideoCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                            res = !(avcodec_open2(pVideoCodecCtx, codec, nullptr) < 0);
                            if (!res && isHwDecoderEnable) {
                                // A D3D11 device may exist while the GPU does
                                // not support this particular codec/profile.
                                // Retry the same stream with software decoding.
                                avcodec_free_context(&pVideoCodecCtx);
                                av_buffer_unref(&hwDeviceCtx);
                                isHwDecoderEnable = false;
                                hwPixFmt = AV_PIX_FMT_NONE;
                                pVideoCodecCtx = avcodec_alloc_context3(codec);
                                if (pVideoCodecCtx
                                    && avcodec_parameters_to_context(
                                           pVideoCodecCtx, pFormatCtx->streams[i]->codecpar)
                                           >= 0) {
                                    pVideoCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                                    res = avcodec_open2(pVideoCodecCtx, codec, nullptr) >= 0;
                                }
                            }
                            if (res) {
                                width = pVideoCodecCtx->width;
                                height = pVideoCodecCtx->height;
                            }
                        }
                    }
                }

                break;
            }
        }

        if (!res) {
            CloseVideo();
        }
    }

    return res;
}

bool FFmpegDecoder::DecodeVideo(const AVPacket *av_pkt, shared_ptr<AVFrame> &pOutFrame) {
    bool res = false;

    if (pVideoCodecCtx && av_pkt && pOutFrame) {
        int ret = avcodec_send_packet(pVideoCodecCtx, av_pkt);
        if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("发送视频包出错 " + string(errStr));
        }

        if (isHwDecoderEnable) {
            // Initialize the hardware frame.
            if (!hwFrame) {
                hwFrame = shared_ptr<AVFrame>(av_frame_alloc(), &freeFrame);
            }

            ret = avcodec_receive_frame(pVideoCodecCtx, hwFrame.get());
        } else {
            ret = avcodec_receive_frame(pVideoCodecCtx, pOutFrame.get());
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // No output available right now or end of stream
            return false;
        } else if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("解码视频出错 " + string(errStr));
        } else {
            // Successfully decoded a frame
            res = true;
        }

        if (isHwDecoderEnable) {
            if (dropCurrentVideoFrame) {
                av_frame_unref(hwFrame.get());
                pOutFrame.reset();
                return false;
            }

            if (hwFrame->format == hwPixFmt) {
                // Copy data from the D3D11 surface to memory the OpenGL renderer
                // can upload. Keep the transfer frame separate because D3D11
                // normally downloads as NV12, whose two-channel OpenGL upload
                // is unreliable on some Windows/Qt graphics backends.
                shared_ptr<AVFrame> transferFrame(av_frame_alloc(), &freeFrame);
                ret = av_hwframe_transfer_data(transferFrame.get(), hwFrame.get(), 0);
                av_frame_unref(hwFrame.get());

                if (ret < 0) {
                    char errStr[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
                    throw runtime_error("Decode video frame error. " + string(errStr));
                }

                if (transferFrame->format == AV_PIX_FMT_NV12) {
                    pOutFrame->format = AV_PIX_FMT_YUV420P;
                    pOutFrame->width = transferFrame->width;
                    pOutFrame->height = transferFrame->height;
                    if (av_frame_get_buffer(pOutFrame.get(), 32) < 0) {
                        throw runtime_error("Unable to allocate hardware decode conversion frame");
                    }

                    pImgConvertCtx = sws_getCachedContext(
                        pImgConvertCtx, transferFrame->width, transferFrame->height,
                        static_cast<AVPixelFormat>(transferFrame->format), transferFrame->width,
                        transferFrame->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    if (!pImgConvertCtx
                        || sws_scale(
                               pImgConvertCtx, transferFrame->data, transferFrame->linesize, 0,
                               transferFrame->height, pOutFrame->data, pOutFrame->linesize)
                            <= 0) {
                        throw runtime_error("Unable to convert hardware NV12 frame for display");
                    }
                    av_frame_copy_props(pOutFrame.get(), transferFrame.get());
                } else {
                    av_frame_move_ref(pOutFrame.get(), transferFrame.get());
                }
            } else {
                // Some FFmpeg/driver combinations accept a hardware device but
                // still select software output. Preserve that valid frame rather
                // than treating it as a D3D11 surface.
                av_frame_move_ref(pOutFrame.get(), hwFrame.get());
            }
        }
    }

    return res;
}

bool FFmpegDecoder::OpenAudio() {
    bool res = false;

    if (pFormatCtx) {
        audioStreamIndex = -1;

        for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = i;
                const AVCodec *codec = avcodec_find_decoder(pFormatCtx->streams[i]->codecpar->codec_id);

                if (codec) {
                    pAudioCodecCtx = avcodec_alloc_context3(codec);
                    if (pAudioCodecCtx) {
                        if (avcodec_parameters_to_context(pAudioCodecCtx, pFormatCtx->streams[i]->codecpar) >= 0) {
                            res = !(avcodec_open2(pAudioCodecCtx, codec, nullptr) < 0);
                        }
                    }
                }

                break;
            }
        }

        if (!res) {
            CloseAudio();
        }
    }

    return res;
}

void FFmpegDecoder::CloseVideo() {
    hwFrame.reset();
    if (pImgConvertCtx) {
        sws_freeContext(pImgConvertCtx);
        pImgConvertCtx = nullptr;
    }
    if (pVideoCodecCtx) {
        avcodec_free_context(&pVideoCodecCtx);
    }
    av_buffer_unref(&hwDeviceCtx);
    hwPixFmt = AV_PIX_FMT_NONE;
    videoStreamIndex = -1;
}

void FFmpegDecoder::CloseAudio() {
    if (pAudioCodecCtx) {
        avcodec_free_context(&pAudioCodecCtx);
        audioStreamIndex = 0;
    }
}

int FFmpegDecoder::DecodeAudio(int nStreamIndex, const AVPacket *avpkt, uint8_t *pOutBuffer, size_t nOutBufferSize) {
    int decodedSize = 0;

    int packetSize = avpkt->size;
    const uint8_t *pPacketData = avpkt->data;

    while (packetSize > 0) {
        int sizeToDecode = nOutBufferSize;
        uint8_t *pDest = pOutBuffer + decodedSize;
        AVFrame *audioFrame = av_frame_alloc();
        if (!audioFrame) {
            throw std::runtime_error("Failed to allocate audio frame");
            return 0;
        }

        int packetDecodedSize = avcodec_receive_frame(pAudioCodecCtx, audioFrame);

        if (packetDecodedSize >= 0) {
            if (audioFrame->format != AV_SAMPLE_FMT_S16) {
                // Convert frame to AV_SAMPLE_FMT_S16 if needed
                if (!swrCtx) {
                    SwrContext *ptr = nullptr;
                    swr_alloc_set_opts2(
                        &ptr, &pAudioCodecCtx->ch_layout, AV_SAMPLE_FMT_S16, pAudioCodecCtx->sample_rate,
                        &pAudioCodecCtx->ch_layout, static_cast<AVSampleFormat>(audioFrame->format),
                        pAudioCodecCtx->sample_rate, 0, nullptr);

                    auto ret = swr_init(ptr);
                    if (ret < 0) {
                        char errStr[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
                        throw runtime_error("解码音频出错 " + string(errStr));
                        return 0;
                    }
                    swrCtx = shared_ptr<SwrContext>(ptr, &freeSwrCtx);
                }

                // Convert audio frame to S16 format
                int samples = swr_convert(
                    swrCtx.get(), &pDest, audioFrame->nb_samples, (const uint8_t **)audioFrame->data,
                    audioFrame->nb_samples);
                sizeToDecode
                    = samples * pAudioCodecCtx->ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            } else {
                // Copy S16 audio data directly
                sizeToDecode = av_samples_get_buffer_size(
                    nullptr, pAudioCodecCtx->ch_layout.nb_channels, audioFrame->nb_samples, AV_SAMPLE_FMT_S16, 1);
                memcpy(pDest, audioFrame->data[0], sizeToDecode);
            }
        }

        av_frame_free(&audioFrame);

        if (packetDecodedSize < 0) {
            decodedSize = 0;
            break;
        }

        packetSize -= packetDecodedSize;
        pPacketData += packetDecodedSize;

        if (sizeToDecode <= 0) {
            continue;
        }

        decodedSize += sizeToDecode;
    }

    return decodedSize;
}

void FFmpegDecoder::writeAudioBuff(uint8_t *aSample, size_t aSize) {
    lock_guard<mutex> lck(abBuffMtx);
    if (av_fifo_can_write(audioFifoBuffer.get()) < aSize) {
        std::vector<uint8_t> tmp;
        tmp.resize(aSize);
        av_fifo_read(audioFifoBuffer.get(), tmp.data(), aSize);
    }
    av_fifo_write(audioFifoBuffer.get(), aSample, aSize);
}

size_t FFmpegDecoder::ReadAudioBuff(uint8_t *aSample, size_t aSize) {
    lock_guard<mutex> lck(abBuffMtx);
    if (av_fifo_elem_size(audioFifoBuffer.get()) < aSize) {
        return 0;
    }
    av_fifo_read(audioFifoBuffer.get(), aSample, aSize);
    return aSize;
}
void FFmpegDecoder::ClearAudioBuff() {
    lock_guard<mutex> lck(abBuffMtx);
    av_fifo_reset2(audioFifoBuffer.get());
}
