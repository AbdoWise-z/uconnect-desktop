#include "Cursor.hpp"
#include <QCursor>
#include <QPainter>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(UCD_HAS_XFIXES)
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#undef None
#endif

namespace ucd::screen {
#ifndef Q_OS_MACOS
NativeCursor nativeCursor(quintptr previousShape) {
    NativeCursor cursor;
#ifdef Q_OS_WIN
    CURSORINFO info{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&info)) { cursor.visible = false; return cursor; }
    cursor.position = {info.ptScreenPos.x, info.ptScreenPos.y}; cursor.visible = info.flags & CURSOR_SHOWING;
    cursor.shape = reinterpret_cast<quintptr>(info.hCursor);
    if (cursor.shape == previousShape || !info.hCursor) return cursor;
    ICONINFO icon{};
    if (!GetIconInfo(info.hCursor, &icon)) return cursor;
    BITMAP bitmap{}; GetObject(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap);
    const int width = bitmap.bmWidth, height = icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    cursor.hotspot = {int(icon.xHotspot), int(icon.yHotspot)};
    if (width > 0 && height > 0 && width <= 128 && height <= 128) {
        const HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth = width; format.bmiHeader.biHeight = -height;
        format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32; format.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const auto dib = CreateDIBSection(dc, &format, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (dib) {
            const auto old = SelectObject(dc, dib);
            memset(pixels, 0, width * height * 4); DrawIconEx(dc, 0, 0, info.hCursor, width, height, 0, nullptr, DI_NORMAL);
            const QImage black = QImage(static_cast<uchar*>(pixels), width, height, QImage::Format_ARGB32).copy();
            memset(pixels, 255, width * height * 4); DrawIconEx(dc, 0, 0, info.hCursor, width, height, 0, nullptr, DI_NORMAL);
            const QImage white = QImage(static_cast<uchar*>(pixels), width, height, QImage::Format_ARGB32).copy();
            cursor.image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto a = black.pixel(x,y), b = white.pixel(x,y);
                const int alpha = 255 - std::clamp(std::max({qRed(b)-qRed(a),qGreen(b)-qGreen(a),qBlue(b)-qBlue(a)}), 0, 255);
                cursor.image.setPixel(x,y,qRgba(qRed(a),qGreen(a),qBlue(a),alpha));
            }
            SelectObject(dc, old); DeleteObject(dib);
        }
        DeleteDC(dc);
    }
    if (icon.hbmColor) DeleteObject(icon.hbmColor); if (icon.hbmMask) DeleteObject(icon.hbmMask);
#elif defined(UCD_HAS_XFIXES)
    auto* display = XOpenDisplay(nullptr);
    if (!display) { cursor.visible = false; return cursor; }
    int event, error;
    if (XFixesQueryExtension(display, &event, &error)) {
        auto* image = XFixesGetCursorImage(display);
        if (image) {
            cursor.position = {image->x, image->y}; cursor.hotspot = {image->xhot, image->yhot}; cursor.shape = image->cursor_serial;
            if (cursor.shape != previousShape && image->width <= 128 && image->height <= 128) {
                cursor.image = QImage(image->width, image->height, QImage::Format_ARGB32_Premultiplied);
                for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x)
                    cursor.image.setPixel(x,y,QRgb(image->pixels[y * image->width + x]));
            }
            XFree(image);
        }
    }
    XCloseDisplay(display);
#else
    cursor.position = QCursor::pos(); cursor.shape = 1;
    if (previousShape != 1) {
        cursor.image = QImage(16, 24, QImage::Format_ARGB32_Premultiplied); cursor.image.fill(Qt::transparent);
        QPainter painter(&cursor.image); painter.setPen(Qt::white); painter.setBrush(Qt::black);
        painter.drawPolygon(QPolygon{{1,1},{1,19},{6,14},{11,22},{14,20},{9,12},{16,12}});
    }
#endif
    return cursor;
}
#endif
}
