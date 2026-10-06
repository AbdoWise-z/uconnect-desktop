#include "DebugWindow.hpp"

#include <string>

#include "app/AppController.hpp"
#include "imgui.h"

namespace ucd {
namespace {

void text(const QString& s) {
    const std::string utf8 = s.toStdString();
    ImGui::TextUnformatted(utf8.c_str());
}

void row(const char* label, const QString& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    text(value.isEmpty() ? QStringLiteral("-") : value);
}

QString yesNo(bool b) {
    return b ? QStringLiteral("yes") : QStringLiteral("no");
}

ImVec4 stateColor(int code) {
    switch (static_cast<uconnect::PeerState>(code)) {
    case uconnect::PeerState::Connected: return {0.40f, 0.85f, 0.45f, 1.0f};
    case uconnect::PeerState::Probing:
    case uconnect::PeerState::Handshaking: return {0.95f, 0.80f, 0.30f, 1.0f};
    case uconnect::PeerState::Failed: return {0.95f, 0.40f, 0.40f, 1.0f};
    default: return {0.65f, 0.65f, 0.65f, 1.0f};
    }
}

}  // namespace

DebugWindow::DebugWindow(AppController* app)
    : ImGuiWindow(QStringLiteral("uConnect — debug")), app_(app) {}

void DebugWindow::drawUi() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags kRoot = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##debug-root", nullptr, kRoot);

    ImGui::Text("status: %s", app_->status().toStdString().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app_->statusDetail().toStdString().c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 120);
    ImGui::Checkbox("ImGui demo", &showDemo_);
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Node")) { drawNode(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Peers")) { drawPeers(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) { drawLog(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Server")) { drawServer(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void DebugWindow::drawNode() {
    const NodeSnapshot& s = app_->snapshot();
    if (ImGui::BeginTable("node", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("value");
        row("running", yesNo(s.running));
        row("server connected", yesNo(s.serverConnected));
        row("published", yesNo(s.published));
        row("topic authenticated", yesNo(s.authenticated));
        row("server", app_->serverAddress());
        row("topic id", s.topicId);
        row("self dev id", s.selfId);
        row("local TCP port", s.localPort ? QString::number(s.localPort) : QString());
        row("reflexive address", s.reflexive);
        row("pending requests", QString::number(s.pendingRequests));
        row("pending connections", QString::number(s.pendingConnections));
        row("remembered peers", QString::number(s.rememberedPeers));
        row("advertised name", app_->deviceName());
        row("advertised services", app_->offeredServices().join(QStringLiteral(", ")));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (!app_->active()) {
        if (ImGui::Button("Connect")) app_->connectToTopic();
    } else if (ImGui::Button("Disconnect")) {
        app_->disconnectFromTopic();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!app_->active());
    if (ImGui::Button("Lookup peers now")) app_->refreshPeers();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copy self id")) app_->copyText(s.selfId);
    ImGui::SameLine();
    if (ImGui::Button("Copy topic link")) app_->copyTopicLink();
}

void DebugWindow::drawPeers() {
    const auto& peers = app_->peers()->peers();
    ImGui::Text("%d peer(s), %d connected", static_cast<int>(peers.size()), app_->peers()->connectedCount());

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("peers", 9, kFlags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("dev id");
    ImGui::TableSetupColumn("name");
    ImGui::TableSetupColumn("os");
    ImGui::TableSetupColumn("state");
    ImGui::TableSetupColumn("path");
    ImGui::TableSetupColumn("services");
    ImGui::TableSetupColumn("msgs out/in");
    ImGui::TableSetupColumn("bytes out/in");
    ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const PeerRecord& p : peers) {
        ImGui::PushID(p.devId.toStdString().c_str());
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        text(shortId(p.devId));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.devId.toStdString().c_str());

        ImGui::TableNextColumn();
        text(p.hasHello ? p.name : QStringLiteral("(no hello yet)"));

        ImGui::TableNextColumn();
        text(p.os);

        ImGui::TableNextColumn();
        QString state = p.state;
        if (!p.closeReason.isEmpty()) state += QStringLiteral(" (%1)").arg(p.closeReason);
        if (p.stale) state += QStringLiteral(" [stale]");
        ImGui::TextColored(stateColor(p.stateCode), "%s", state.toStdString().c_str());

        ImGui::TableNextColumn();
        text(p.stateCode == static_cast<int>(uconnect::PeerState::Connected)
                 ? (p.relayed ? QStringLiteral("relayed") : QStringLiteral("direct"))
                 : QString());

        ImGui::TableNextColumn();
        text(serviceNames(p.services).join(QStringLiteral(",")));

        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(p.messagesSent),
                    static_cast<unsigned long long>(p.messagesReceived));

        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(p.bytesSent),
                    static_cast<unsigned long long>(p.bytesReceived));

        ImGui::TableNextColumn();
        if (ImGui::SmallButton("connect")) app_->connectPeer(p.devId);
        ImGui::SameLine();
        if (ImGui::SmallButton("disconnect")) app_->disconnectPeer(p.devId);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy id")) app_->copyText(p.devId);

        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DebugWindow::drawLog() {
    if (ImGui::Button("Clear")) app_->clearLog();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::Separator();

    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const QString& line : app_->logLines()) text(line);
    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void DebugWindow::drawServer() {
    ImGui::BeginDisabled(!app_->active());
    if (ImGui::Button("Server stats")) app_->requestServerStats();
    ImGui::SameLine();
    if (ImGui::Button("List public topics")) app_->requestTopicListing();
    ImGui::EndDisabled();
    ImGui::Separator();
    text(app_->serverReport());
}

}  // namespace ucd
