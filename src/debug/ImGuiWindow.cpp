#include "ImGuiWindow.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPlatformSurfaceEvent>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <cfloat>

#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"

namespace ucd {
namespace {

ImGuiKey toImGuiKey(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return static_cast<ImGuiKey>(ImGuiKey_0 + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Tab: return ImGuiKey_Tab;
    case Qt::Key_Left: return ImGuiKey_LeftArrow;
    case Qt::Key_Right: return ImGuiKey_RightArrow;
    case Qt::Key_Up: return ImGuiKey_UpArrow;
    case Qt::Key_Down: return ImGuiKey_DownArrow;
    case Qt::Key_PageUp: return ImGuiKey_PageUp;
    case Qt::Key_PageDown: return ImGuiKey_PageDown;
    case Qt::Key_Home: return ImGuiKey_Home;
    case Qt::Key_End: return ImGuiKey_End;
    case Qt::Key_Insert: return ImGuiKey_Insert;
    case Qt::Key_Delete: return ImGuiKey_Delete;
    case Qt::Key_Backspace: return ImGuiKey_Backspace;
    case Qt::Key_Space: return ImGuiKey_Space;
    case Qt::Key_Return: return ImGuiKey_Enter;
    case Qt::Key_Enter: return ImGuiKey_KeypadEnter;
    case Qt::Key_Escape: return ImGuiKey_Escape;
    case Qt::Key_Control: return ImGuiKey_LeftCtrl;
    case Qt::Key_Shift: return ImGuiKey_LeftShift;
    case Qt::Key_Alt: return ImGuiKey_LeftAlt;
    case Qt::Key_Meta: return ImGuiKey_LeftSuper;
    default: return ImGuiKey_None;
    }
}

int toImGuiButton(Qt::MouseButton b) {
    switch (b) {
    case Qt::LeftButton: return 0;
    case Qt::RightButton: return 1;
    case Qt::MiddleButton: return 2;
    default: return -1;
    }
}

void addModifiers(ImGuiIO& io, Qt::KeyboardModifiers m) {
    io.AddKeyEvent(ImGuiMod_Ctrl, m.testFlag(Qt::ControlModifier));
    io.AddKeyEvent(ImGuiMod_Shift, m.testFlag(Qt::ShiftModifier));
    io.AddKeyEvent(ImGuiMod_Alt, m.testFlag(Qt::AltModifier));
    io.AddKeyEvent(ImGuiMod_Super, m.testFlag(Qt::MetaModifier));
}

}  // namespace

ImGuiWindow::ImGuiWindow(const QString& title) {
    setTitle(title);
    resize(1000, 700);
#ifdef Q_OS_MACOS
    // macOS only offers core profiles past 2.1; ImGui's backend picks GLSL 150.
    QSurfaceFormat fmt = format();
    fmt.setVersion(3, 2);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(fmt);
#endif
    repaint_.setInterval(33);
    connect(&repaint_, &QTimer::timeout, this, [this] { update(); });
}

ImGuiWindow::~ImGuiWindow() {
    releaseImGui();
}

void ImGuiWindow::initializeGL() {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);
    ImGui::StyleColorsDark();

    ImGuiIO& io            = ImGui::GetIO();
    io.IniFilename         = nullptr;
    io.BackendPlatformName = "qt";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGuiPlatformIO& pio            = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        static QByteArray buffer;
        buffer = QGuiApplication::clipboard()->text().toUtf8();
        return buffer.constData();
    };
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        QGuiApplication::clipboard()->setText(QString::fromUtf8(text));
    };

    initialized_ = ImGui_ImplOpenGL3_Init(nullptr);
    frameClock_.start();
    repaint_.start();
}

void ImGuiWindow::paintGL() {
    if (!initialized_) return;
    ImGui::SetCurrentContext(ctx_);

    ImGuiIO&    io  = ImGui::GetIO();
    const float dpr = static_cast<float>(devicePixelRatio());
    io.DisplaySize  = ImVec2(static_cast<float>(width()), static_cast<float>(height()));
    io.DisplayFramebufferScale = ImVec2(dpr, dpr);
    io.DeltaTime = std::max(static_cast<float>(frameClock_.restart()) / 1000.0f, 1e-4f);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    drawUi();
    ImGui::Render();
    updateCursor();

    QOpenGLFunctions* gl = context()->functions();
    gl->glViewport(0, 0, static_cast<int>(width() * dpr), static_cast<int>(height() * dpr));
    gl->glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiWindow::updateCursor() {
    const int cursor = ImGui::GetMouseCursor();
    if (cursor == lastCursor_) return;
    lastCursor_ = cursor;
    switch (cursor) {
    case ImGuiMouseCursor_TextInput: setCursor(Qt::IBeamCursor); break;
    case ImGuiMouseCursor_ResizeAll: setCursor(Qt::SizeAllCursor); break;
    case ImGuiMouseCursor_ResizeNS: setCursor(Qt::SizeVerCursor); break;
    case ImGuiMouseCursor_ResizeEW: setCursor(Qt::SizeHorCursor); break;
    case ImGuiMouseCursor_ResizeNESW: setCursor(Qt::SizeBDiagCursor); break;
    case ImGuiMouseCursor_ResizeNWSE: setCursor(Qt::SizeFDiagCursor); break;
    case ImGuiMouseCursor_Hand: setCursor(Qt::PointingHandCursor); break;
    case ImGuiMouseCursor_NotAllowed: setCursor(Qt::ForbiddenCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
    }
}

bool ImGuiWindow::event(QEvent* e) {
    if (e->type() == QEvent::Leave && ctx_) {
        ImGui::SetCurrentContext(ctx_);
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    // Closing destroys the surface, and QOpenGLWindow drops its GL context with
    // it; release ImGui's GL objects while that context still exists.
    if (e->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        releaseImGui();
    }
    return QOpenGLWindow::event(e);
}

void ImGuiWindow::releaseImGui() {
    repaint_.stop();
    if (!ctx_) return;
    makeCurrent();
    ImGui::SetCurrentContext(ctx_);
    if (initialized_) ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(ctx_);
    doneCurrent();
    ctx_         = nullptr;
    initialized_ = false;
    lastCursor_  = -2;
}

void ImGuiWindow::mouseMoveEvent(QMouseEvent* e) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    const QPointF p = e->position();
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(p.x()), static_cast<float>(p.y()));
}

void ImGuiWindow::mousePressEvent(QMouseEvent* e) {
    const int button = toImGuiButton(e->button());
    if (!ctx_ || button < 0) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    addModifiers(io, e->modifiers());
    io.AddMousePosEvent(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    io.AddMouseButtonEvent(button, true);
}

void ImGuiWindow::mouseReleaseEvent(QMouseEvent* e) {
    const int button = toImGuiButton(e->button());
    if (!ctx_ || button < 0) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddMouseButtonEvent(button, false);
}

void ImGuiWindow::wheelEvent(QWheelEvent* e) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    const QPoint d = e->angleDelta();
    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(d.x()) / 120.0f,
                                      static_cast<float>(d.y()) / 120.0f);
}

void ImGuiWindow::keyPressEvent(QKeyEvent* e) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    addModifiers(io, e->modifiers());
    if (const ImGuiKey k = toImGuiKey(e->key()); k != ImGuiKey_None) io.AddKeyEvent(k, true);

    const QString text = e->text();
    if (!text.isEmpty() && text.at(0).isPrint()) io.AddInputCharactersUTF8(text.toUtf8().constData());
}

void ImGuiWindow::keyReleaseEvent(QKeyEvent* e) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    addModifiers(io, e->modifiers());
    if (const ImGuiKey k = toImGuiKey(e->key()); k != ImGuiKey_None) io.AddKeyEvent(k, false);
}

void ImGuiWindow::focusInEvent(QFocusEvent*) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddFocusEvent(true);
}

void ImGuiWindow::focusOutEvent(QFocusEvent*) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui::GetIO().AddFocusEvent(false);
}

}  // namespace ucd
