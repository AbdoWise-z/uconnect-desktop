#include "Cursor.hpp"
#include <QCursor>
#include <QPainter>
#include <algorithm>
#include <cmath>
#import <AppKit/AppKit.h>

namespace ucd::screen {
NativeCursor nativeCursor(quintptr previousShape) {
    @autoreleasepool {
        NativeCursor result;
        result.position = QCursor::pos();
        NSCursor* cursor = [NSCursor currentSystemCursor];
        if (!cursor) cursor = [NSCursor arrowCursor];
        // The image is drawn in the pixels of the screen under the pointer
        // (1x on most external and virtual displays, 2x on Retina ones), as
        // that screen is captured. Its TIFF form holds every size the cursor
        // has, largest first (10x), which no viewer takes.
        CGFloat scale = 1;
        const NSPoint mouse = [NSEvent mouseLocation];
        for (NSScreen* screen in [NSScreen screens])
            if (NSMouseInRect(mouse, screen.frame, NO)) { scale = screen.backingScaleFactor; break; }
        // The scale is part of the shape: moving to a screen of another scale
        // sends the image again.
        result.shape = reinterpret_cast<quintptr>(cursor) + (scale > 1.5 ? 1 : 0);
        if (result.shape != previousShape) {
            NSImage* image = [cursor image];
            const NSSize points = [image size];
            if (points.width < 1 || points.height < 1) return result;
            // Within the 128x128 a viewer accepts.
            scale = std::min({scale, 128 / points.width, 128 / points.height});
            const NSInteger wide = std::max<NSInteger>(1, std::lround(points.width * scale));
            const NSInteger high = std::max<NSInteger>(1, std::lround(points.height * scale));
            NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:wide pixelsHigh:high
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            bitmap.size = points;
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
            [image drawInRect:NSMakeRect(0, 0, points.width, points.height) fromRect:NSZeroRect
                    operation:NSCompositingOperationCopy fraction:1];
            [NSGraphicsContext restoreGraphicsState];
            NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            result.image = QImage::fromData(static_cast<const uchar*>([png bytes]), int([png length]), "PNG");
            const NSPoint hotspot = [cursor hotSpot];
            result.hotspot = {std::clamp(int(hotspot.x * wide / points.width), 0, int(wide) - 1),
                              std::clamp(int(hotspot.y * high / points.height), 0, int(high) - 1)};
        }
        return result;
    }
}
}
