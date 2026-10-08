#include "Cursor.hpp"
#include <QCursor>
#include <QPainter>
#include <algorithm>
#import <AppKit/AppKit.h>

namespace ucd::screen {
NativeCursor nativeCursor(quintptr previousShape) {
    @autoreleasepool {
        NativeCursor result;
        result.position = QCursor::pos();
        NSCursor* cursor = [NSCursor currentSystemCursor];
        if (!cursor) cursor = [NSCursor arrowCursor];
        result.shape = reinterpret_cast<quintptr>(cursor);
        if (result.shape != previousShape) {
            NSImage* image = [cursor image];
            NSBitmapImageRep* bitmap = [NSBitmapImageRep imageRepWithData:[image TIFFRepresentation]];
            NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            result.image = QImage::fromData(static_cast<const uchar*>([png bytes]), [png length], "PNG");
            if (result.image.width() > 128 || result.image.height() > 128) result.image = {};
            const NSPoint hotspot = [cursor hotSpot]; const NSSize points = [image size];
            result.hotspot = {int(hotspot.x * result.image.width() / std::max(1., points.width)),
                int(hotspot.y * result.image.height() / std::max(1., points.height))};
        }
        return result;
    }
}
}
