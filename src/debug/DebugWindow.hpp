#pragma once

#include "debug/ImGuiWindow.hpp"

namespace ucd {

class AppController;

class DebugWindow : public ImGuiWindow {
public:
    explicit DebugWindow(AppController* app);

protected:
    void drawUi() override;

private:
    void drawNode();
    void drawPeers();
    void drawLog();
    void drawServer();

    AppController* app_;
    bool           showDemo_   = false;
    bool           autoScroll_ = true;
};

}  // namespace ucd
