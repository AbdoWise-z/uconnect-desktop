#include "Codec.hpp"

#include <QElapsedTimer>
#include <QLibrary>
#include <algorithm>
#include <map>
#include "openh264/codec_api.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace ucd::screen {
namespace {
QString avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}
bool supports(const AVCodec* codec, AVPixelFormat format) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    const void* values = nullptr; int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &values, &count) < 0) return false;
    const auto* formats = static_cast<const AVPixelFormat*>(values);
    for (int i = 0; i < count; ++i) if (formats[i] == format) return true;
#else
    if (codec->pix_fmts) for (const auto* f = codec->pix_fmts; *f != AV_PIX_FMT_NONE; ++f) if (*f == format) return true;
#endif
    return false;
}
}

bool bitrateWorthChanging(int applied, int wanted, qint64 sinceMs) {
    if (wanted == applied || applied <= 0) return wanted != applied;
    if (wanted < applied * 0.85) return sinceMs >= 1000;
    if (wanted > applied * 1.25) return sinceMs >= 5000;
    return sinceMs >= 15000 && std::abs(wanted - applied) > applied / 20;
}

bool lgplCodecBuild() {
    const QByteArray configuration(avcodec_configuration());
    return !configuration.contains("--enable-gpl") && !configuration.contains("--enable-nonfree");
}

struct Encoder::Impl {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scale = nullptr;
    QLibrary cisco;
    ISVCEncoder* software = nullptr;
    using Destroy = void (*)(ISVCEncoder*);
    Destroy destroy = nullptr;
    QString name, error, library;
    QSize size;
    int fps = 0, bitrate = 0, openedBitrate = 0;
    QElapsedTimer bitrateChanged;
    qint64 pts = 0;
    std::map<qint64, quint32> captureTimes;
    ~Impl() {
        if (software) { software->Uninitialize(); destroy(software); }
        av_frame_free(&frame); avcodec_free_context(&context); sws_freeContext(scale);
    }

    bool openCisco(const QString& path, int width, int height, const CodecOptions& options) {
        if (path.isEmpty()) return false;
        cisco.setFileName(path);
        if (!cisco.load()) { error = cisco.errorString(); return false; }
        const auto create = reinterpret_cast<int (*)(ISVCEncoder**)>(cisco.resolve("WelsCreateSVCEncoder"));
        destroy = reinterpret_cast<Destroy>(cisco.resolve("WelsDestroySVCEncoder"));
        const auto version = reinterpret_cast<void (*)(OpenH264Version*)>(cisco.resolve("WelsGetCodecVersionEx"));
        OpenH264Version v{};
        if (!create || !destroy || !version) { error = "Invalid OpenH264 library"; return false; }
        version(&v);
        if (v.uMajor != 2 || v.uMinor != 6) { error = "OpenH264 2.6 is required"; return false; }
        if (create(&software) || !software) { error = "Cannot create OpenH264 encoder"; return false; }
        SEncParamExt params{};
        software->GetDefaultParams(&params);
        params.iUsageType = SCREEN_CONTENT_REAL_TIME;
        params.iPicWidth = width; params.iPicHeight = height;
        params.iTargetBitrate = options.bitrate; params.iRCMode = RC_BITRATE_MODE;
        params.fMaxFrameRate = options.fps; params.uiIntraPeriod = options.fps * 4;
        params.iSpatialLayerNum = 1; params.iTemporalLayerNum = 1;
        params.bEnableFrameSkip = true; params.iMultipleThreadIdc = 1;
        params.bEnableAdaptiveQuant = false; params.bEnableBackgroundDetection = false;
        auto& layer = params.sSpatialLayers[0];
        layer.iVideoWidth = width; layer.iVideoHeight = height;
        layer.fFrameRate = options.fps; layer.iSpatialBitrate = options.bitrate;
        layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
        if (software->InitializeExt(&params)) {
            error = "Cannot initialize OpenH264"; destroy(software); software = nullptr; return false;
        }
        name = "Cisco OpenH264"; library = path;
        return true;
    }

    bool open(QSize input, const CodecOptions& options) {
        if (!lgplCodecBuild()) { error = "FFmpeg must be an LGPL shared build"; return false; }
        const int width = std::max(2, std::min(input.width(), options.width) & ~1);
        const int height = std::max(2, int(qint64(input.height()) * width / input.width()) & ~1);
        if (width > 3840 || height > 4320) { error = "Capture size exceeds the codec limit"; return false; }
        const auto names = options.encoder.isEmpty() ? Encoder::candidates() : QStringList{options.encoder};
        for (const auto& candidate : names) {
            if (candidate == "cisco") { if (openCisco(options.ciscoLibrary, width, height, options)) break; continue; }
            // Never select the source-built libopenh264 in the FFmpeg archive.
            if (candidate == "libopenh264") continue;
            const auto* codec = avcodec_find_encoder_by_name(candidate.toUtf8().constData());
            if (!codec) continue;
            context = avcodec_alloc_context3(codec);
            context->width = width; context->height = height;
            context->time_base = {1, options.fps}; context->framerate = {options.fps, 1};
            context->bit_rate = options.bitrate; context->rc_max_rate = options.bitrate;
            context->rc_buffer_size = options.bitrate / 8;
            context->gop_size = options.fps * 4; context->max_b_frames = 0;
            context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            // As swscale converts by default.
            context->colorspace = AVCOL_SPC_SMPTE170M; context->color_range = AVCOL_RANGE_MPEG;
            context->pix_fmt = supports(codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
            AVDictionary* settings = nullptr;
            if (candidate == "h264_nvenc") {
                av_dict_set(&settings, "preset", "p1", 0); av_dict_set(&settings, "tune", "ull", 0);
                av_dict_set(&settings, "zerolatency", "1", 0); av_dict_set(&settings, "forced-idr", "1", 0);
                av_dict_set(&settings, "delay", "0", 0);
            } else if (candidate == "h264_qsv") {
                av_dict_set(&settings, "async_depth", "1", 0); av_dict_set(&settings, "preset", "veryfast", 0);
            } else if (candidate == "h264_amf") {
                av_dict_set(&settings, "usage", "ultralowlatency", 0);
            } else if (candidate == "h264_videotoolbox") {
                av_dict_set(&settings, "realtime", "1", 0);
            } else if (candidate == "h264_mf") {
                av_dict_set(&settings, "hw_encoding", "1", 0);
            }
            const int result = avcodec_open2(context, codec, &settings); av_dict_free(&settings);
            if (result >= 0) { name = candidate; break; }
            error = candidate + ": " + avError(result); avcodec_free_context(&context);
        }
        if (!context && !software) {
            error = "No usable H.264 hardware encoder. Install and enable the Cisco OpenH264 fallback. " + error;
            return false;
        }
        size = {width, height}; fps = options.fps; bitrate = openedBitrate = options.bitrate; bitrateChanged.start();
        frame = av_frame_alloc(); frame->width = width; frame->height = height;
        frame->format = context ? context->pix_fmt : AV_PIX_FMT_YUV420P;
        const int result = av_frame_get_buffer(frame, 32);
        if (result < 0) { error = avError(result); return false; }
        error.clear();
        return true;
    }
};

Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}
Encoder::~Encoder() = default;
void Encoder::reset() { impl_ = std::make_unique<Impl>(); }
QString Encoder::name() const { return impl_->name; }
QString Encoder::error() const { return impl_->error; }
QStringList Encoder::candidates() {
#ifdef Q_OS_WIN
    return {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "cisco"};
#elif defined(Q_OS_MACOS)
    return {"h264_videotoolbox", "cisco"};
#else
    return {"h264_nvenc", "h264_qsv", "h264_vaapi", "cisco"};
#endif
}

std::optional<EncodedFrame> Encoder::encode(const QImage& input, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe) {
    if (input.isNull() || input.width() < 2 || input.height() < 2) return std::nullopt;
    const auto target = QSize(std::max(2, std::min(input.width(), options.width) & ~1),
        std::max(2, int(qint64(input.height()) * (std::min(input.width(), options.width) & ~1) / input.width()) & ~1));
    if (impl_->size != target || impl_->fps != options.fps || (impl_->software && impl_->library != options.ciscoLibrary)) reset();
    if (!impl_->frame && !impl_->open(input.size(), options)) return std::nullopt;
    auto& p = *impl_;
    if (p.bitrate != options.bitrate && (p.software || bitrateWorthChanging(p.bitrate, options.bitrate, p.bitrateChanged.elapsed()))) {
        if (p.software) {
            SBitrateInfo rate{SPATIAL_LAYER_ALL, options.bitrate};
            p.software->SetOption(ENCODER_OPTION_BITRATE, &rate);
        } else if (options.bitrate > p.openedBitrate * 2 || options.bitrate < p.openedBitrate / 2) {
            // A large change reopens the encoder, which every implementation
            // honours. Smaller ones are applied live (NVENC and QSV pick them
            // up; others keep their rate until a large change).
            reset(); if (!impl_->open(input.size(), options)) return std::nullopt;
            return encode(input, options, session, number, captureMs, true);
        } else {
            p.context->bit_rate = p.context->rc_max_rate = options.bitrate;
            p.context->rc_buffer_size = options.bitrate / 8;
        }
        p.bitrate = options.bitrate; p.bitrateChanged.start();
    }
    const QImage image = input.convertToFormat(QImage::Format_RGBA8888);
    if (av_frame_make_writable(p.frame) < 0) return std::nullopt;
    p.scale = sws_getCachedContext(p.scale, image.width(), image.height(), AV_PIX_FMT_RGBA,
        p.size.width(), p.size.height(), AVPixelFormat(p.frame->format), SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!p.scale) { p.error = "Cannot create color converter"; return std::nullopt; }
    const uint8_t* source[]{image.constBits()}; const int stride[]{int(image.bytesPerLine())};
    sws_scale(p.scale, source, stride, 0, image.height(), p.frame->data, p.frame->linesize);
    EncodedFrame result{session, number, captureMs, false, {}};
    if (p.software) {
        if (keyframe || p.pts == 0) p.software->ForceIntraFrame(true);
        SSourcePicture picture{};
        picture.iColorFormat = videoFormatI420; picture.iPicWidth = p.size.width(); picture.iPicHeight = p.size.height();
        picture.uiTimeStamp = p.pts++ * 1000 / options.fps;
        for (int i = 0; i < 3; ++i) { picture.pData[i] = p.frame->data[i]; picture.iStride[i] = p.frame->linesize[i]; }
        SFrameBSInfo bitstream{};
        if (p.software->EncodeFrame(&picture, &bitstream)) { p.error = "OpenH264 encode failed"; return std::nullopt; }
        result.keyframe = bitstream.eFrameType == videoFrameTypeIDR;
        for (int layer = 0; layer < bitstream.iLayerNum; ++layer) {
            const auto& data = bitstream.sLayerInfo[layer]; int length = 0;
            for (int nal = 0; nal < data.iNalCount; ++nal) length += data.pNalLengthInByte[nal];
            result.bytes.append(reinterpret_cast<const char*>(data.pBsBuf), length);
        }
    } else {
        p.frame->pts = p.pts++;
        p.captureTimes[p.frame->pts] = captureMs;
        if (p.captureTimes.size() > 16) { p.error = "Hardware encoder buffered too many frames"; return std::nullopt; }
        p.frame->pict_type = keyframe || p.frame->pts == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        const int sent = avcodec_send_frame(p.context, p.frame);
        if (sent < 0) { p.error = avError(sent); return std::nullopt; }
        auto* packet = av_packet_alloc();
        while (avcodec_receive_packet(p.context, packet) >= 0) {
            if (const auto at = p.captureTimes.find(packet->pts); at != p.captureTimes.end()) {
                result.captureMs = at->second; p.captureTimes.erase(at);
            }
            result.keyframe |= bool(packet->flags & AV_PKT_FLAG_KEY);
            result.bytes.append(reinterpret_cast<const char*>(packet->data), packet->size); av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    if (result.bytes.isEmpty() || result.bytes.size() > MaxFrameBytes) return std::nullopt;
    return result;
}

struct Decoder::Impl {
    AVCodecContext* context = nullptr;
    AVBufferRef* device = nullptr;
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
    SwsContext* scale = nullptr;
    QString error;
    ~Impl() { avcodec_free_context(&context); av_buffer_unref(&device); sws_freeContext(scale); }
    bool decodeInto(const EncodedFrame& encoded, bool transfer, const std::function<void(AVFrame*)>& use);
    static AVPixelFormat selectFormat(AVCodecContext* codec, const AVPixelFormat* formats) {
        const auto* self = static_cast<Impl*>(codec->opaque);
        for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f) if (*f == self->hardwareFormat) return *f;
        for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f) if (!av_pix_fmt_desc_get(*f) || !(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
        return AV_PIX_FMT_NONE;
    }
};
Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
void Decoder::reset() { impl_ = std::make_unique<Impl>(); }
QString Decoder::error() const { return impl_->error; }
// Sends one access unit and hands each picture that comes out to `use`, in
// system memory when `transfer` is set (otherwise as decoded, possibly a GPU
// surface). False on failure.
bool Decoder::Impl::decodeInto(const EncodedFrame& encoded, bool transfer, const std::function<void(AVFrame*)>& use) {
    auto& p = *this;
    if (!lgplCodecBuild()) { p.error = "FFmpeg must be an LGPL shared build"; return false; }
    if (!p.context) {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { p.error = "H.264 decoder unavailable"; return false; }
        p.context = avcodec_alloc_context3(codec); p.context->thread_count = 1;
        p.context->opaque = &p; p.context->get_format = Impl::selectFormat;
#ifdef Q_OS_WIN
        const auto type = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(Q_OS_MACOS)
        const auto type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        const auto type = AV_HWDEVICE_TYPE_VAAPI;
#endif
        for (int i = 0; const auto* hardware = avcodec_get_hw_config(codec, i); ++i) {
            if (hardware->device_type == type && (hardware->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                av_hwdevice_ctx_create(&p.device, type, nullptr, nullptr, 0) >= 0) {
                p.hardwareFormat = hardware->pix_fmt; p.context->hw_device_ctx = av_buffer_ref(p.device); break;
            }
        }
        p.context->max_pixels = 3840 * 4320;
        p.context->err_recognition = AV_EF_EXPLODE;
        p.context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(p.context, codec, nullptr) < 0) { p.error = "Cannot open H.264 decoder"; return false; }
    }
    if (encoded.bytes.isEmpty() || encoded.bytes.size() > MaxFrameBytes) return false;
    if (encoded.keyframe) avcodec_flush_buffers(p.context);
    auto* packet = av_packet_alloc();
    av_new_packet(packet, encoded.bytes.size()); memcpy(packet->data, encoded.bytes.constData(), encoded.bytes.size());
    int status = avcodec_send_packet(p.context, packet); av_packet_free(&packet);
    if (status < 0) { p.error = avError(status); return false; }
    auto* frame = av_frame_alloc();
    while ((status = avcodec_receive_frame(p.context, frame)) >= 0) {
        if (transfer && frame->format == p.hardwareFormat && p.hardwareFormat != AV_PIX_FMT_NONE) {
            auto* mapped = av_frame_alloc();
            if (av_hwframe_transfer_data(mapped, frame, 0) < 0) { av_frame_free(&mapped); break; }
            av_frame_copy_props(mapped, frame);
            av_frame_unref(frame); av_frame_move_ref(frame, mapped); av_frame_free(&mapped);
        }
        if (frame->width < 2 || frame->height < 2 || frame->width > 3840 || frame->height > 4320) break;
        use(frame);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF && status < 0) { p.error = avError(status); return false; }
    return true;
}

QImage Decoder::decode(const EncodedFrame& encoded) {
    auto& p = *impl_;
    QImage image;
    p.decodeInto(encoded, true, [&](AVFrame* frame) {
        image = QImage(frame->width, frame->height, QImage::Format_RGBA8888);
        p.scale = sws_getCachedContext(p.scale, frame->width, frame->height, AVPixelFormat(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!p.scale) { image = {}; return; }
        uint8_t* target[]{image.bits()}; const int stride[]{int(image.bytesPerLine())};
        sws_scale(p.scale, frame->data, frame->linesize, 0, frame->height, target, stride);
    });
    return image;
}

QVideoFrame Decoder::decodeFrame(const EncodedFrame& encoded, bool picture, bool* decoded) {
    auto& p = *impl_;
    QVideoFrame result; bool any = false;
    p.decodeInto(encoded, picture, [&](AVFrame* frame) {
        any = true;
        if (!picture) return;
        const auto format = AVPixelFormat(frame->format);
        const bool full = format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
        const auto layout = format == AV_PIX_FMT_NV12 ? QVideoFrameFormat::Format_NV12
            : format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P ? QVideoFrameFormat::Format_YUV420P
            : QVideoFrameFormat::Format_RGBA8888;
        QVideoFrameFormat videoFormat(QSize(frame->width, frame->height), layout);
        // Unlabelled streams were encoded with swscale's default, BT.601.
        videoFormat.setColorSpace(frame->colorspace == AVCOL_SPC_BT709 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601);
        videoFormat.setColorRange(full ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
        QVideoFrame out(videoFormat);
        if (!out.map(QVideoFrame::WriteOnly)) return;
        if (layout == QVideoFrameFormat::Format_RGBA8888) {
            p.scale = sws_getCachedContext(p.scale, frame->width, frame->height, format,
                frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!p.scale) { out.unmap(); return; }
            uint8_t* target[]{out.bits(0)}; const int stride[]{out.bytesPerLine(0)};
            sws_scale(p.scale, frame->data, frame->linesize, 0, frame->height, target, stride);
        } else {
            for (int plane = 0; plane < out.planeCount(); ++plane) {
                const int rows = plane ? (frame->height + 1) / 2 : frame->height;
                const int bytes = layout == QVideoFrameFormat::Format_NV12 ? (frame->width + 1) / 2 * 2
                                : plane ? (frame->width + 1) / 2 : frame->width;
                const int length = std::min({bytes, out.bytesPerLine(plane), frame->linesize[plane]});
                for (int y = 0; y < rows; ++y)
                    memcpy(out.bits(plane) + qsizetype(y) * out.bytesPerLine(plane), frame->data[plane] + qsizetype(y) * frame->linesize[plane], length);
            }
        }
        out.unmap();
        result = out;
    });
    if (decoded) *decoded = any;
    return result;
}

}  // namespace ucd::screen
