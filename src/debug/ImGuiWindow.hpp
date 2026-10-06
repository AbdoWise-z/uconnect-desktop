#pragma once
// A top-level window that hosts a Dear ImGui UI: Qt supplies the window, the GL
// context and the input events, ImGui's stock OpenGL3 backend does the drawing.
// Subclasses implement drawUi() with ordinary ImGui calls.

#include <QElapsedTimer>
#include <QOpenGLWindow>
#include <QTimer>

struct ImGuiContext;

namespace ucd {

class ImGuiWindow : public QOpenGLWindow {
public:
    explicit ImGuiWindow(const QString& title);
    ~ImGuiWindow() override;

protected:
    virtual void drawUi() = 0;

    void initializeGL() override;
    void paintGL() override;

    bool event(QEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    void updateCursor();
    void releaseImGui();

    ImGuiContext* ctx_         = nullptr;
    bool          initialized_ = false;
    QElapsedTimer frameClock_;
    QTimer        repaint_;
    int           lastCursor_ = -2;
};

}  // namespace ucd
