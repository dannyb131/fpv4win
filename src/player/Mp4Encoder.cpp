#include "Mp4Encoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
struct NalUnit {
    const uint8_t *data = nullptr;
    size_t size = 0;
};

std::vector<NalUnit> annexBNalUnits(const AVPacket *packet) {
    std::vector<NalUnit> units;
    if (!packet || !packet->data || packet->size < 4) {
        return units;
    }

    auto startCodeLength = [packet](size_t offset) -> size_t {
        if (offset + 3 <= static_cast<size_t>(packet->size)
            && packet->data[offset] == 0 && packet->data[offset + 1] == 0 && packet->data[offset + 2] == 1) {
            return 3;
        }
        if (offset + 4 <= static_cast<size_t>(packet->size)
            && packet->data[offset] == 0 && packet->data[offset + 1] == 0
            && packet->data[offset + 2] == 0 && packet->data[offset + 3] == 1) {
            return 4;
        }
        return 0;
    };

    size_t offset = 0;
    while (offset < static_cast<size_t>(packet->size)) {
        size_t codeLength = 0;
        while (offset < static_cast<size_t>(packet->size)
               && (codeLength = startCodeLength(offset)) == 0) {
            ++offset;
        }
        if (!codeLength) {
            break;
        }
        const size_t nalStart = offset + codeLength;
        size_t nalEnd = nalStart;
        while (nalEnd < static_cast<size_t>(packet->size) && startCodeLength(nalEnd) == 0) {
            ++nalEnd;
        }
        if (nalEnd > nalStart) {
            units.push_back({ packet->data + nalStart, nalEnd - nalStart });
        }
        offset = nalEnd;
    }
    return units;
}

bool isParameterSet(const NalUnit &unit, AVCodecID codec) {
    if (!unit.data || unit.size == 0) {
        return false;
    }
    if (codec == AV_CODEC_ID_HEVC) {
        const int type = (unit.data[0] >> 1) & 0x3f;
        return type == 32 || type == 33 || type == 34;
    }
    if (codec == AV_CODEC_ID_H264) {
        const int type = unit.data[0] & 0x1f;
        return type == 7 || type == 8;
    }
    return false;
}

bool isRandomAccessPacket(const AVPacket *packet, AVCodecID codec) {
    if (!packet) {
        return false;
    }
    if (packet->flags & AV_PKT_FLAG_KEY) {
        return true;
    }
    for (const NalUnit &unit : annexBNalUnits(packet)) {
        if (codec == AV_CODEC_ID_HEVC) {
            const int type = (unit.data[0] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) {
                return true;
            }
        } else if (codec == AV_CODEC_ID_H264 && (unit.data[0] & 0x1f) == 5) {
            return true;
        }
    }
    return false;
}
}

Mp4Encoder::Mp4Encoder(const string &saveFilePath)
    : _saveFilePath(saveFilePath) {
    AVFormatContext *context = nullptr;
    const int result = avformat_alloc_output_context2(&context, nullptr, "mp4", _saveFilePath.c_str());
    if (result >= 0 && context) {
        _formatCtx = shared_ptr<AVFormatContext>(context, &avformat_free_context);
    } else {
        fail(result < 0 ? result : AVERROR_UNKNOWN, "Unable to create MP4 output");
    }
}

Mp4Encoder::~Mp4Encoder() {
    if (_isOpen) {
        stop();
    }
}

bool Mp4Encoder::addTrack(AVStream *stream, const AVCodecContext *codecContext) {
    if (!_formatCtx || !stream) {
        _lastError = "The source track is not available.";
        return false;
    }

    AVStream *output = avformat_new_stream(_formatCtx.get(), nullptr);
    if (!output) {
        _lastError = "Unable to allocate an MP4 track.";
        return false;
    }

    int result = codecContext
        ? avcodec_parameters_from_context(output->codecpar, codecContext)
        : avcodec_parameters_copy(output->codecpar, stream->codecpar);
    if (result < 0) {
        return fail(result, "Unable to copy MP4 track parameters");
    }
    output->codecpar->codec_tag = 0;
    output->time_base = stream->time_base;

    if (output->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audioIndex = output->index;
        _originAudioTimeBase = stream->time_base;
    } else if (output->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        videoIndex = output->index;
        _originVideoTimeBase = stream->time_base;
        _videoCodecId = output->codecpar->codec_id;
    }
    return true;
}

bool Mp4Encoder::primeVideoParameters(const AVPacket *packet) {
    if (!_formatCtx || videoIndex < 0 || !packet) {
        _lastError = "No video keyframe is available yet.";
        return false;
    }

    AVCodecParameters *parameters = _formatCtx->streams[videoIndex]->codecpar;
    std::vector<uint8_t> extraData;
    static constexpr uint8_t startCode[] = { 0, 0, 0, 1 };
    for (const NalUnit &unit : annexBNalUnits(packet)) {
        if (!isParameterSet(unit, _videoCodecId)) {
            continue;
        }
        extraData.insert(extraData.end(), std::begin(startCode), std::end(startCode));
        extraData.insert(extraData.end(), unit.data, unit.data + unit.size);
    }

    if (!extraData.empty()) {
        av_freep(&parameters->extradata);
        parameters->extradata = static_cast<uint8_t *>(
            av_mallocz(extraData.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata) {
            return fail(AVERROR(ENOMEM), "Unable to allocate MP4 codec headers");
        }
        std::memcpy(parameters->extradata, extraData.data(), extraData.size());
        parameters->extradata_size = static_cast<int>(extraData.size());
    }

    if ((_videoCodecId == AV_CODEC_ID_H264 || _videoCodecId == AV_CODEC_ID_HEVC)
        && parameters->extradata_size == 0) {
        _lastError = "The next camera keyframe has not supplied codec headers yet.";
        return false;
    }
    return true;
}

bool Mp4Encoder::start() {
    if (!_formatCtx || videoIndex < 0) {
        _lastError = "No video track is available for MP4 recording.";
        return false;
    }
    AVCodecParameters *videoParameters = _formatCtx->streams[videoIndex]->codecpar;
    if (videoParameters->width <= 0 || videoParameters->height <= 0) {
        _lastError = "The video dimensions are not available yet.";
        return false;
    }

    int result = avio_open(&_formatCtx->pb, _saveFilePath.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
        return fail(result, "Unable to create the MP4 file");
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    av_dict_set(&options, "avoid_negative_ts", "make_zero", 0);
    result = avformat_write_header(_formatCtx.get(), &options);
    av_dict_free(&options);
    if (result < 0) {
        avio_closep(&_formatCtx->pb);
        return fail(result, "Unable to write the MP4 header");
    }
    _isOpen = true;
    return true;
}

void Mp4Encoder::writePacket(const shared_ptr<AVPacket> &packet, bool isVideo) {
    if (!_isOpen || !packet) {
        return;
    }

    auto output = shared_ptr<AVPacket>(
        av_packet_clone(packet.get()), [](AVPacket *value) { av_packet_free(&value); });
    if (!output) {
        return;
    }

    if (!writtenKeyFrame) {
        if (!isVideo || !isRandomAccessPacket(output.get(), _videoCodecId)) {
            return;
        }
        output->flags |= AV_PKT_FLAG_KEY;
        writtenKeyFrame = true;
    } else if (isVideo && isRandomAccessPacket(output.get(), _videoCodecId)) {
        output->flags |= AV_PKT_FLAG_KEY;
    }

    const int outputIndex = isVideo ? videoIndex : audioIndex;
    if (outputIndex < 0) {
        return;
    }
    const AVRational inputTimeBase = isVideo ? _originVideoTimeBase : _originAudioTimeBase;
    int64_t &firstTimestamp = isVideo ? _videoFirstTimestamp : _audioFirstTimestamp;
    int64_t &lastDts = isVideo ? _videoLastDts : _audioLastDts;
    const int64_t sourceTimestamp = output->dts != AV_NOPTS_VALUE ? output->dts : output->pts;
    if (firstTimestamp == AV_NOPTS_VALUE && sourceTimestamp != AV_NOPTS_VALUE) {
        firstTimestamp = sourceTimestamp;
    }
    if (firstTimestamp != AV_NOPTS_VALUE) {
        if (output->dts != AV_NOPTS_VALUE) output->dts -= firstTimestamp;
        if (output->pts != AV_NOPTS_VALUE) output->pts -= firstTimestamp;
    }

    output->stream_index = outputIndex;
    output->pos = -1;
    av_packet_rescale_ts(output.get(), inputTimeBase, _formatCtx->streams[outputIndex]->time_base);
    if (output->dts == AV_NOPTS_VALUE) {
        output->dts = lastDts == AV_NOPTS_VALUE ? 0 : lastDts + std::max<int64_t>(output->duration, 1);
    }
    if (lastDts != AV_NOPTS_VALUE && output->dts <= lastDts) {
        output->dts = lastDts + std::max<int64_t>(output->duration, 1);
    }
    if (output->pts == AV_NOPTS_VALUE || output->pts < output->dts) {
        output->pts = output->dts;
    }
    lastDts = output->dts;

    const int result = av_interleaved_write_frame(_formatCtx.get(), output.get());
    if (result < 0) {
        fail(result, "Unable to write MP4 video data");
        return;
    }
    _wrotePacket = true;
}

bool Mp4Encoder::stop() {
    if (!_isOpen) {
        return false;
    }
    const int trailerResult = av_write_trailer(_formatCtx.get());
    const int closeResult = avio_closep(&_formatCtx->pb);
    _isOpen = false;
    if (!_wrotePacket) {
        _lastError = "Recording stopped before the camera sent a new keyframe.";
        return false;
    }
    if (trailerResult < 0) {
        return fail(trailerResult, "Unable to finish the MP4 file");
    }
    if (closeResult < 0) {
        return fail(closeResult, "Unable to close the MP4 file");
    }
    return true;
}

bool Mp4Encoder::fail(int errorCode, const string &context) {
    char error[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, error, sizeof(error));
    _lastError = context + ": " + error;
    return false;
}
