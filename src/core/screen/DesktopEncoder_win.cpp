#include "DesktopEncoder.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <map>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace ucd::screen {
namespace {
template<class T> class Com {
    T* value_ = nullptr;
public:
    ~Com() { reset(); }
    void reset() { if (value_) value_->Release(); value_ = nullptr; }
    T* get() const { return value_; }
    T** put() { reset(); return &value_; }
    T* operator->() const { return value_; }
};
class D3DDesktop : public DesktopEncoder {
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutputDuplication> duplication_;
    Com<ID3D11VideoDevice> videoDevice_;
    Com<ID3D11VideoContext> videoContext_;
    Com<ID3D11VideoProcessorEnumerator> enumerator_;
    Com<ID3D11VideoProcessor> processor_;
    Com<ID3D11Texture2D> source_;
    AVBufferRef *deviceRef_ = nullptr, *frames_ = nullptr, *qsvDevice_ = nullptr, *qsvFrames_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    QString monitor_, name_, error_;
    QSize input_, output_;
    CodecOptions options_;
    qint64 pts_ = 0;
    int openedBitrate_ = 0;
    QElapsedTimer bitrateChanged_;
    std::map<qint64, quint32> times_;
    bool haveSource_ = false, forceKey_ = true;
    // The encoder side: what depends on the output size and frame rate.
    void closeCodec() {
        avcodec_free_context(&codec_); av_buffer_unref(&qsvFrames_); av_buffer_unref(&frames_);
        processor_.reset(); enumerator_.reset();
        forceKey_ = true; pts_ = 0; times_.clear();
    }
    void close() {
        closeCodec();
        av_buffer_unref(&qsvDevice_); av_buffer_unref(&deviceRef_);
        source_.reset(); videoContext_.reset(); videoDevice_.reset();
        duplication_.reset(); context_.reset(); device_.reset();
        haveSource_ = false; monitor_.clear();
    }
    bool open(const QString& monitor, const CodecOptions& options) {
        if (!lgplCodecBuild()) return false;
        Com<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())))) return false;
        Com<IDXGIAdapter1> adapter; Com<IDXGIOutput> output;
        bool found = false; DXGI_OUTPUT_DESC description{};
        for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) == S_OK && !found; ++a) {
            for (UINT o = 0; adapter->EnumOutputs(o, output.put()) == S_OK; ++o) {
                output->GetDesc(&description);
                if (description.AttachedToDesktop && QString::fromWCharArray(description.DeviceName) == monitor) { found = true; break; }
            }
            if (found) break;
        }
        if (!found || description.Rotation != DXGI_MODE_ROTATION_IDENTITY) return false;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 1,
            D3D11_SDK_VERSION, device_.put(), nullptr, context_.put()))) return false;
        Com<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(output1.put()))) ||
            FAILED(output1->DuplicateOutput(device_.get(), duplication_.put()))) return false;
        DXGI_OUTDUPL_DESC desktop{}; duplication_->GetDesc(&desktop);
        input_ = {int(desktop.ModeDesc.Width), int(desktop.ModeDesc.Height)};
        if (input_.width() < 2 || input_.height() < 2) return false;
        if (FAILED(device_->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(videoDevice_.put()))) ||
            FAILED(context_->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(videoContext_.put())))) return false;
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = input_.width(); texture.Height = input_.height(); texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count = 1;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&texture, nullptr, source_.put()))) return false;
        deviceRef_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA); if (!deviceRef_) return false;
        auto* hardware = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(deviceRef_->data)->hwctx);
        hardware->device = device_.get(); hardware->device->AddRef();
        if (av_hwdevice_ctx_init(deviceRef_) < 0) return false;
        monitor_ = monitor;
        return openCodec(options);
    }
    bool openCodec(const CodecOptions& options) {
        output_ = {std::max(2, std::min(input_.width(), options.width) & ~1), 0};
        output_.setHeight(std::max(2, int(qint64(input_.height()) * output_.width() / input_.width()) & ~1));
        if (output_.width() > 3840 || output_.height() > 4320) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = input_.width(); content.InputHeight = input_.height();
        content.OutputWidth = output_.width(); content.OutputHeight = output_.height();
        content.InputFrameRate = content.OutputFrameRate = {UINT(options.fps), 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&content, enumerator_.put())) ||
            FAILED(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()))) return false;
        frames_ = av_hwframe_ctx_alloc(deviceRef_); if (!frames_) return false;
        auto* pool = reinterpret_cast<AVHWFramesContext*>(frames_->data);
        pool->format = AV_PIX_FMT_D3D11; pool->sw_format = AV_PIX_FMT_NV12;
        // QSV surface dimensions must be macroblock aligned; the visible frame
        // and video-processor destination retain the requested crop size.
        pool->width = (output_.width() + 15) & ~15; pool->height = (output_.height() + 15) & ~15; pool->initial_pool_size = 0;
        static_cast<AVD3D11VAFramesContext*>(pool->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (av_hwframe_ctx_init(frames_) < 0) return false;
        const auto names = options.encoder.isEmpty() ? QStringList{"h264_nvenc", "h264_qsv", "h264_amf"} : QStringList{options.encoder};
        for (const auto& name : names) {
            if (name != "h264_nvenc" && name != "h264_qsv" && name != "h264_amf") continue;
            const auto* codec = avcodec_find_encoder_by_name(name.toUtf8().constData()); if (!codec) continue;
            AVBufferRef* codecFrames = frames_;
            if (name == "h264_qsv") {
                if (!qsvDevice_ && av_hwdevice_ctx_create_derived(&qsvDevice_, AV_HWDEVICE_TYPE_QSV, deviceRef_, 0) < 0) continue;
                if (!qsvFrames_ && av_hwframe_ctx_create_derived(&qsvFrames_, AV_PIX_FMT_QSV, qsvDevice_, frames_, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0) continue;
                codecFrames = qsvFrames_;
            }
            codec_ = avcodec_alloc_context3(codec);
            codec_->width = output_.width(); codec_->height = output_.height(); codec_->time_base = {1, options.fps};
            codec_->framerate = {options.fps, 1}; codec_->bit_rate = codec_->rc_max_rate = options.bitrate;
            codec_->rc_buffer_size = options.bitrate / 8; codec_->gop_size = options.fps * 4; codec_->max_b_frames = 0;
            codec_->flags |= AV_CODEC_FLAG_LOW_DELAY; codec_->hw_frames_ctx = av_buffer_ref(codecFrames);
            // As the video processor converts (YCbCr_Matrix 1, nominal range).
            codec_->colorspace = AVCOL_SPC_BT709; codec_->color_primaries = AVCOL_PRI_BT709;
            codec_->color_trc = AVCOL_TRC_BT709; codec_->color_range = AVCOL_RANGE_MPEG;
            codec_->pix_fmt = name == "h264_qsv" ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
            AVDictionary* tuning = nullptr;
            if (name == "h264_nvenc") {
                av_dict_set(&tuning, "preset", "p1", 0); av_dict_set(&tuning, "tune", "ull", 0);
                av_dict_set(&tuning, "delay", "0", 0); av_dict_set(&tuning, "forced-idr", "1", 0);
            } else if (name == "h264_qsv") av_dict_set(&tuning, "async_depth", "1", 0);
            else av_dict_set(&tuning, "usage", "ultralowlatency", 0);
            const int status = avcodec_open2(codec_, codec, &tuning); av_dict_free(&tuning);
            if (status >= 0) { name_ = name; break; }
            avcodec_free_context(&codec_);
        }
        if (!codec_) return false;
        options_ = options; openedBitrate_ = options.bitrate; bitrateChanged_.start(); return true;
    }
public:
    ~D3DDesktop() override { close(); }
    DesktopResult encode(const QString& monitor, const CodecOptions& options, quint32 number, quint32 captureMs, bool keyframe) override {
        forceKey_ |= keyframe;
        // Another monitor needs a new duplication; another size, frame rate or
        // encoder only a new encoder. The bitrate changes on the fly, batched
        // (see bitrateWorthChanging) since each change costs a keyframe.
        if (monitor_ != monitor) close();
        else if (codec_ && (options.width != options_.width || options.fps != options_.fps || options.encoder != options_.encoder ||
                            options.bitrate > openedBitrate_ * 2 || options.bitrate < openedBitrate_ / 2)) closeCodec();
        if ((!duplication_.get() && !open(monitor, options)) || (!codec_ && !openCodec(options))) {
            close(); return {false, {}, {}, "D3D11 capture/encode unavailable; using Qt capture."};
        }
        if (bitrateWorthChanging(options_.bitrate, options.bitrate, bitrateChanged_.elapsed())) {
            codec_->bit_rate = codec_->rc_max_rate = options.bitrate; codec_->rc_buffer_size = options.bitrate / 8;
            options_.bitrate = options.bitrate; bitrateChanged_.start();
        }
        const auto pipeline = "DXGI / D3D11 NV12 / " + name_;
        Com<IDXGIResource> resource; DXGI_OUTDUPL_FRAME_INFO info{};
        // Until the first image arrives, wait for it a little rather than
        // reporting nothing captured over and over.
        auto acquired = duplication_->AcquireNextFrame(haveSource_ ? 0 : 50, &info, resource.put());
        if (acquired == DXGI_ERROR_ACCESS_LOST) {
            // A mode change, a UAC prompt or the lock screen: duplicate afresh.
            close();
            if (!open(monitor, options)) { close(); return {false, {}, {}, "DXGI capture was interrupted; using Qt capture for now."}; }
            acquired = duplication_->AcquireNextFrame(50, &info, resource.put());
        }
        if (acquired == S_OK) {
            Com<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put())))) {
                context_->CopyResource(source_.get(), texture.get()); haveSource_ = true;
            }
            duplication_->ReleaseFrame();
        } else if (acquired != DXGI_ERROR_WAIT_TIMEOUT && acquired != DXGI_ERROR_ACCESS_LOST) {
            close(); return {false, {}, {}, "DXGI capture ended; using Qt capture."};
        }
        if (!haveSource_) return {true, {}, pipeline, {}};
        auto* frame = av_frame_alloc();
        if (av_hwframe_get_buffer(frames_, frame, 0) < 0) { av_frame_free(&frame); close(); return {}; }
        frame->width = output_.width(); frame->height = output_.height();
        Com<ID3D11VideoProcessorInputView> input; Com<ID3D11VideoProcessorOutputView> output;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{}; inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{}; outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        const auto madeInput = videoDevice_->CreateVideoProcessorInputView(source_.get(), enumerator_.get(), &inputDesc, input.put());
        const auto madeOutput = videoDevice_->CreateVideoProcessorOutputView(reinterpret_cast<ID3D11Texture2D*>(frame->data[0]), enumerator_.get(), &outputDesc, output.put());
        RECT sourceRect{0,0,input_.width(),input_.height()}, destRect{0,0,output_.width(),output_.height()};
        videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &sourceRect);
        videoContext_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &destRect);
        videoContext_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &destRect);
        videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{}, yuv{}; rgb.RGB_Range = 0; yuv.YCbCr_Matrix = 1; yuv.Nominal_Range = 1;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &rgb);
        videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &yuv);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input.get();
        if (FAILED(madeInput) || FAILED(madeOutput) || FAILED(videoContext_->VideoProcessorBlt(processor_.get(), output.get(), 0, 1, &stream))) {
            av_frame_free(&frame); close(); return {false, {}, {}, "D3D11 color conversion failed; using Qt capture."};
        }
        context_->Flush();
        if (codec_->pix_fmt == AV_PIX_FMT_QSV) {
            auto* mapped = av_frame_alloc(); mapped->format = AV_PIX_FMT_QSV; mapped->hw_frames_ctx = av_buffer_ref(qsvFrames_);
            const int status = av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
            av_frame_free(&frame); frame = mapped;
            if (status < 0) { av_frame_free(&frame); close(); return {}; }
        }
        frame->pts = pts_++; frame->pict_type = forceKey_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        times_[frame->pts] = captureMs;
        const int sent = avcodec_send_frame(codec_, frame); av_frame_free(&frame);
        if (sent < 0 || times_.size() > 16) { close(); return {false, {}, {}, "D3D11 encoder failed; using Qt capture."}; }
        EncodedFrame encoded{1,number,captureMs,false,{}}; auto* packet = av_packet_alloc();
        while (avcodec_receive_packet(codec_, packet) >= 0) {
            encoded.keyframe |= bool(packet->flags & AV_PKT_FLAG_KEY);
            if (auto at = times_.find(packet->pts); at != times_.end()) { encoded.captureMs = at->second; times_.erase(at); }
            encoded.bytes.append(reinterpret_cast<const char*>(packet->data), packet->size); av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (encoded.bytes.isEmpty()) return {true, {}, pipeline, {}};
        forceKey_ = false;
        if (encoded.bytes.size() > MaxFrameBytes) { close(); return {}; }
        return {true, encoded, pipeline, {}};
    }
};
}
std::unique_ptr<DesktopEncoder> desktopEncoder() { return std::make_unique<D3DDesktop>(); }
}
