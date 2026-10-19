#include "DesktopEncoder.hpp"
#include <algorithm>
#include <mutex>
#include <map>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <QElapsedTimer>
#include <QStringList>
#import <CoreMedia/CoreMedia.h>
// AVFoundation (through ScreenCaptureKit) and FFmpeg both name a type
// AVMediaType; FFmpeg's enum is renamed here, where nothing uses it.
#define AVMediaType FFmpegAVMediaType
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#undef AVMediaType

// ScreenCaptureKit owns the IOSurface pool; this delegate retains only the
// newest CVPixelBuffer. No image mapping, readback or swscale is used.
struct UCDLatestSurface {
    std::mutex mutex;
    CVPixelBufferRef buffer = nullptr;
    bool failed = false;
    ~UCDLatestSurface() { if (buffer) CVPixelBufferRelease(buffer); }
};
@interface UCDScreenOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public std::shared_ptr<UCDLatestSurface> surface;
}
@end
@implementation UCDScreenOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments.count && [attachments[0][SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
    auto buffer = CMSampleBufferGetImageBuffer(sample); if (!buffer) return;
    auto latest = surface;
    std::lock_guard lock(latest->mutex);
    CVPixelBufferRetain(buffer); if (latest->buffer) CVPixelBufferRelease(latest->buffer); latest->buffer = buffer;
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    auto latest = surface; std::lock_guard lock(latest->mutex); latest->failed = true;
}
@end

namespace ucd::screen {
namespace {
class VideoToolboxDesktop : public DesktopEncoder {
    SCStream* stream_ = nil;
    SCStreamConfiguration* configuration_ = nil;
    UCDScreenOutput* delegate_ = nil;
    std::shared_ptr<UCDLatestSurface> surface_;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef *device_ = nullptr, *frames_ = nullptr;
    QString monitor_;
    CodecOptions options_;
    int width_ = 0, height_ = 0;
    qint64 pts_ = 0;  // milliseconds, so the frame rate may change freely
    std::map<qint64, quint32> times_;
    bool forceKey_ = true;
    QElapsedTimer bitrateChanged_;
    void closeCodec() {
        avcodec_free_context(&codec_); times_.clear(); forceKey_ = true;
    }
    void close() {
        if (stream_) [stream_ stopCaptureWithCompletionHandler:^(NSError*) {}];
        stream_ = nil; configuration_ = nil; delegate_ = nil; surface_.reset();
        closeCodec(); av_buffer_unref(&frames_); av_buffer_unref(&device_);
        pts_ = 0; width_ = height_ = 0;
    }
    // The encoder alone, on the open capture. VideoToolbox (through FFmpeg)
    // takes a bitrate only when opened, so a new bitrate is a new encoder.
    QString openCodec(const CodecOptions& options) {
        const auto* encoder = avcodec_find_encoder_by_name("h264_videotoolbox"); if (!encoder) return QStringLiteral("no VideoToolbox encoder");
        codec_ = avcodec_alloc_context3(encoder); codec_->width = width_; codec_->height = height_;
        codec_->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX; codec_->hw_frames_ctx = av_buffer_ref(frames_);
        codec_->time_base = {1, 1000}; codec_->framerate = {options.fps, 1};
        codec_->bit_rate = codec_->rc_max_rate = options.bitrate; codec_->rc_buffer_size = options.bitrate / 8;
        codec_->gop_size = options.fps * 4; codec_->max_b_frames = 0; codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        AVDictionary* tuning = nullptr; av_dict_set(&tuning, "realtime", "1", 0); av_dict_set(&tuning, "allow_sw", "0", 0);
        const int status = avcodec_open2(codec_, encoder, &tuning); av_dict_free(&tuning);
        if (status < 0) return QStringLiteral("the encoder did not open (%1)").arg(status);
        options_.bitrate = options.bitrate; bitrateChanged_.start();
        return {};
    }
    // Why it could not open, for people; empty once it is open.
    QString open(const QString& monitor, const CodecOptions& options) {
        if (@available(macOS 12.3, *)) {
            if (!lgplCodecBuild()) return QStringLiteral("this build has no VideoToolbox encoder");
            if (!CGPreflightScreenCaptureAccess()) {
                // Checking never asks; ask once, which shows macOS's prompt. A
                // grant takes effect when the app is next opened.
                static std::once_flag asked;
                std::call_once(asked, [] { dispatch_async(dispatch_get_main_queue(), ^{ CGRequestScreenCaptureAccess(); }); });
                return QStringLiteral("no Screen Recording permission: allow uConnect under System Settings > "
                                      "Privacy & Security > Screen & System Audio Recording, then reopen uConnect");
            }
            __block SCShareableContent* content = nil;
            __block NSError* listError = nil;
            auto ready = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES completionHandler:^(SCShareableContent* result, NSError* error) {
                content = result; listError = error; dispatch_semaphore_signal(ready);
            }];
            if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0)
                return QStringLiteral("macOS did not list the screens in time");
            if (!content)
                return QStringLiteral("macOS would not list the screens (%1)")
                    .arg(listError ? QString::fromNSString(listError.localizedDescription) : QStringLiteral("no reason given"));
            SCDisplay* display = nil;
            QStringList listed;
            for (SCDisplay* candidate in content.displays) {
                listed << QString::number(candidate.displayID);
                if (candidate.displayID == monitor.toUInt()) display = candidate;
            }
            if (!display) return QStringLiteral("macOS does not list display %1 (it lists %2)").arg(monitor, listed.join(QStringLiteral(", ")));
            if (display.width < 2 || display.height < 2) return QStringLiteral("display %1 has no size").arg(monitor);
            int pixelsWide = int(display.width);
            if (auto mode = CGDisplayCopyDisplayMode(display.displayID)) {
                pixelsWide = int(CGDisplayModeGetPixelWidth(mode)); CGDisplayModeRelease(mode);
            }
            const int width = std::max(2, std::min(pixelsWide, options.width) & ~1);
            const int height = std::max(2, int(qint64(display.height) * width / display.width) & ~1);
            if (width > 3840 || height > 4320) return QStringLiteral("%1x%2 is larger than the encoder takes").arg(width).arg(height);
            configuration_ = [SCStreamConfiguration new];
            configuration_.width = width; configuration_.height = height;
            configuration_.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            configuration_.minimumFrameInterval = CMTimeMake(1, options.fps);
            configuration_.queueDepth = 3; configuration_.showsCursor = NO;
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            surface_ = std::make_shared<UCDLatestSurface>(); delegate_ = [UCDScreenOutput new]; delegate_->surface = surface_;
            stream_ = [[SCStream alloc] initWithFilter:filter configuration:configuration_ delegate:delegate_];
            NSError* error = nil;
            if (![stream_ addStreamOutput:delegate_ type:SCStreamOutputTypeScreen sampleHandlerQueue:dispatch_queue_create("uconnect.screen.surface", DISPATCH_QUEUE_SERIAL) error:&error])
                return QStringLiteral("the capture stream took no output (%1)").arg(QString::fromNSString(error.localizedDescription));
            __block NSError* startError = nil; __block bool started = false; auto start = dispatch_semaphore_create(0);
            [stream_ startCaptureWithCompletionHandler:^(NSError* error) { started = error == nil; startError = error; dispatch_semaphore_signal(start); }];
            if (dispatch_semaphore_wait(start, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0) return QStringLiteral("the capture did not start in time");
            if (!started) return QStringLiteral("the capture did not start (%1)").arg(QString::fromNSString(startError.localizedDescription));
            if (av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) return QStringLiteral("no VideoToolbox device");
            frames_ = av_hwframe_ctx_alloc(device_); if (!frames_) return QStringLiteral("no frame pool");
            auto* pool = reinterpret_cast<AVHWFramesContext*>(frames_->data);
            pool->format = AV_PIX_FMT_VIDEOTOOLBOX; pool->sw_format = AV_PIX_FMT_NV12; pool->width = width; pool->height = height;
            if (av_hwframe_ctx_init(frames_) < 0) return QStringLiteral("the frame pool did not start");
            width_ = width; height_ = height; monitor_ = monitor; options_ = options;
            return openCodec(options);
        }
        return QStringLiteral("macOS 12.3 or newer is needed");
    }
public:
    ~VideoToolboxDesktop() override { close(); }
    DesktopResult encode(const QString& monitor, const CodecOptions& options, quint32 number, quint32 captureMs, bool keyframe) override {
        @autoreleasepool {
            forceKey_ |= keyframe;
            // Congestion control moves the frame rate and bitrate all the time.
            // Neither restarts the capture (which would flash macOS's recording
            // indicator and drop frames): the frame rate is changed on the
            // running stream, and the bitrate needs only a new encoder, batched
            // (bitrateWorthChanging) as each costs a keyframe. Another monitor
            // or size starts afresh.
            if (monitor_ != monitor || options.width != options_.width) close();
            else if (codec_ && bitrateWorthChanging(options_.bitrate, options.bitrate, bitrateChanged_.elapsed())) closeCodec();
            if (stream_ && options.fps != options_.fps) {
                configuration_.minimumFrameInterval = CMTimeMake(1, options.fps);
                [stream_ updateConfiguration:configuration_ completionHandler:^(NSError*) {}];
                options_.fps = options.fps;
            }
            if (!stream_ || !codec_) {
                const QString why = stream_ ? openCodec(options) : open(monitor, options);
                if (!why.isEmpty()) {
                    close();
                    return {false, {}, {}, QStringLiteral("Native capture unavailable: %1.").arg(why)};
                }
            }
            CVPixelBufferRef buffer = nullptr;
            { std::lock_guard lock(surface_->mutex);
                if (surface_->failed) { /* close outside lock below */ }
                else if (surface_->buffer) { buffer = surface_->buffer; CVPixelBufferRetain(buffer); }
            }
            const QString pipeline = "ScreenCaptureKit / IOSurface / VideoToolbox";
            if (!buffer) {
                bool failed; { std::lock_guard lock(surface_->mutex); failed = surface_->failed; }
                if (failed) { close(); return {}; }
                return {true, {}, pipeline, {}};
            }
            auto* frame = av_frame_alloc(); frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
            frame->width = codec_->width; frame->height = codec_->height; frame->hw_frames_ctx = av_buffer_ref(frames_);
            frame->data[3] = reinterpret_cast<uint8_t*>(buffer);
            frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(buffer), 1, [](void*, uint8_t* data) { CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data)); }, nullptr, 0);
            frame->pts = pts_ = std::max<qint64>(pts_ + 1, captureMs); times_[frame->pts] = captureMs;
            frame->pict_type = forceKey_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            const int sent = avcodec_send_frame(codec_, frame); av_frame_free(&frame);
            if (sent < 0 || times_.size() > 16) { close(); return {}; }
            auto* packet = av_packet_alloc(); EncodedFrame encoded{1, number, captureMs, false, {}};
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
    }
};
}
std::unique_ptr<DesktopEncoder> desktopEncoder() { return std::make_unique<VideoToolboxDesktop>(); }
}
