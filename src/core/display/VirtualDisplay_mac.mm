// Virtual monitors on macOS, through CGVirtualDisplay: a private CoreGraphics
// API (used by Sidecar, DeskPad, BetterDisplay). Not in any SDK header, so
// its interface is declared here. The display lives as long as the object,
// and the system removes it when the process ends however it ends.
#include "VirtualDisplay.hpp"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>

#include <QThread>
#include <cstring>

@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString* name;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int vendorID;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray* modes;
@property(nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
@property(readonly, nonatomic) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings*)settings;
@end

namespace ucd::display {
namespace {

// The window server is driven from the main thread, as the apps using this
// API do; callers run on worker threads.
void onMain(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

class MacDisplay final : public VirtualDisplay {
public:
    ~MacDisplay() override { destroy(); }

    bool create(const QByteArray& identity, const QString& name, const Mode& mode, QString* error) override {
        destroy();
        if (!NSClassFromString(@"CGVirtualDisplay")) return fail(error, QStringLiteral("This macOS has no virtual displays."));
        unsigned int serial = 0;
        std::memcpy(&serial, identity.leftJustified(4, '\0').constData(), 4);
        const QByteArray utf8 = name.left(32).toUtf8();
        __block CGVirtualDisplay* created = nil;
        const unsigned int width = unsigned(mode.width), height = unsigned(mode.height);
        onMain(^{
            CGVirtualDisplayDescriptor* descriptor = [[CGVirtualDisplayDescriptor alloc] init];
            descriptor.queue = dispatch_get_main_queue();
            descriptor.name = [NSString stringWithUTF8String:utf8.constData()];
            descriptor.maxPixelsWide = width;
            descriptor.maxPixelsHigh = height;
            // About 110 dpi at 1x, 220 dpi at 2x: macOS picks text sizes from this.
            const double mmPerPixel = mode.hiDpi ? 25.4 / 220 : 25.4 / 110;
            descriptor.sizeInMillimeters = CGSizeMake(width * mmPerPixel, height * mmPerPixel);
            descriptor.vendorID = 0x5543;  // "UC"
            descriptor.productID = 0x0001;
            descriptor.serialNum = serial;
            descriptor.terminationHandler = ^(id, id) {};
            created = [[CGVirtualDisplay alloc] initWithDescriptor:descriptor];
        });
        if (!created) return fail(error, QStringLiteral("macOS refused to create a virtual display."));
        display_ = created;
        if (!setMode(mode, error)) { destroy(); return false; }
        // The display appears to the window server asynchronously.
        for (int i = 0; i < 100 && !CGDisplayIsActive(display_.displayID); ++i) QThread::msleep(50);
        if (!CGDisplayIsActive(display_.displayID)) return fail(error, QStringLiteral("macOS did not show the new display."));
        return true;
    }

    bool setMode(const Mode& mode, QString* error) override {
        if (!display_) { if (error) *error = QStringLiteral("No display."); return false; }
        // In HiDPI, modes are in points: half the pixels each way.
        const unsigned int scale = mode.hiDpi ? 2 : 1;
        const unsigned int w = unsigned(mode.width) / scale, h = unsigned(mode.height) / scale;
        const double hz = mode.refreshMilliHz / 1000.0;
        __block BOOL applied = NO;
        CGVirtualDisplay* display = display_;
        const bool hiDpi = mode.hiDpi;
        onMain(^{
            CGVirtualDisplaySettings* settings = [[CGVirtualDisplaySettings alloc] init];
            settings.hiDPI = hiDpi ? 1 : 0;
            settings.modes = @[[[CGVirtualDisplayMode alloc] initWithWidth:w height:h refreshRate:hz]];
            applied = [display applySettings:settings];
        });
        if (!applied) { if (error) *error = QStringLiteral("macOS refused the display mode."); return false; }
        mode_ = mode;
        return true;
    }

    void destroy() override {
        if (!display_) return;
        // Releasing the last reference removes the display. Asynchronously:
        // this may run while the main thread waits for the caller's thread
        // (shutdown), and the process ending removes the display anyway.
        __block CGVirtualDisplay* display = display_;
        display_ = nil;
        if ([NSThread isMainThread]) display = nil;
        else dispatch_async(dispatch_get_main_queue(), ^{ display = nil; });
    }

    void keepAlive() override {}

    QString captureId() const override { return display_ ? QString::number(display_.displayID) : QString(); }

    QRect geometry() const override {
        if (!display_) return {};
        const CGRect bounds = CGDisplayBounds(display_.displayID);
        return QRect(int(bounds.origin.x), int(bounds.origin.y), int(bounds.size.width), int(bounds.size.height));
    }

    QSize pixels() const override {
        if (!display_) return {};
        QSize size(mode_.width, mode_.height);
        if (CGDisplayModeRef current = CGDisplayCopyDisplayMode(display_.displayID)) {
            size = QSize(int(CGDisplayModeGetPixelWidth(current)), int(CGDisplayModeGetPixelHeight(current)));
            CGDisplayModeRelease(current);
        }
        return size;
    }

private:
    bool fail(QString* error, const QString& text) {
        if (error) *error = text;
        destroy();
        return false;
    }

    CGVirtualDisplay* display_ = nil;
    Mode mode_;
};

}  // namespace

Status status() {
    if (!NSClassFromString(@"CGVirtualDisplay"))
        return {Availability::NotSupported, QStringLiteral("This version of macOS has no virtual displays.")};
    return {Availability::Ready, {}};
}

std::unique_ptr<VirtualDisplay> create() { return std::make_unique<MacDisplay>(); }

}  // namespace ucd::display
