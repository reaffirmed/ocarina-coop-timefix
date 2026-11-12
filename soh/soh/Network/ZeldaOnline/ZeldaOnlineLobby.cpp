// Co-op Lobby window: host or join a Story Co-op game from the title screen, and choose the team playthrough.
// See ZeldaOnlineLobby.hpp for how it fits together.
#include "ZeldaOnlineLobby.hpp"
#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "PartySettings.hpp"
#include "soh/Enhancements/AutoTest/AutoTest.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/IconsFontAwesome4.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace ZeldaOnline {

static const char* kShowOnTitleCVar = "gZeldaOnline.Lobby.ShowOnTitle";
static const ImVec4 kErrorColor = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
static const ImVec4 kGoodColor = ImVec4(0.55f, 0.9f, 0.55f, 1.0f);

static void SaveCVarsSoon() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

static std::string ListPath() {
    return Ship::Context::GetPathRelativeToAppDirectory("coop-playthroughs.json");
}

static std::string FormatDate(int64_t t) {
    if (t <= 0)
        return "never";
    time_t tt = (time_t)t;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d %b %Y", std::localtime(&tt));
    return buf;
}

static std::string FileLabel(int fileNum) {
    return fileNum >= 0 ? "File " + std::to_string(fileNum + 1) : "";
}

// ---------------------------------------------------------------------------------------------- playthrough list

void CoopPlaythroughList::Load() {
    hosted.clear();
    joined.clear();
    std::ifstream in(ListPath());
    if (in) {
        try {
            nlohmann::json j = nlohmann::json::parse(in);
            for (const auto& e : j.value("hosted", nlohmann::json::array())) {
                CoopPlaythrough p;
                p.name = e.value("name", std::string());
                p.code = e.value("code", std::string());
                p.created = e.value("created", (int64_t)0);
                p.lastPlayed = e.value("lastPlayed", (int64_t)0);
                p.lastFile = e.value("lastFile", -1);
                p.lastPlayers = e.value("lastPlayers", std::string());
                p.hostNick = e.value("hostNick", std::string());
                if (!p.code.empty())
                    hosted.push_back(p);
            }
            for (const auto& e : j.value("joined", nlohmann::json::array())) {
                Joined p;
                p.host = e.value("host", std::string());
                p.lastFile = e.value("lastFile", -1);
                p.lastPlayed = e.value("lastPlayed", (int64_t)0);
                if (!p.host.empty())
                    joined.push_back(p);
            }
        } catch (const std::exception& e) {
            printf("Co-op Lobby: could not read %s (%s)\n", ListPath().c_str(), e.what());
        }
    }

    // Story Co-op played before the lobby existed: keep that team code, so its progress is one click away
    const std::string earlier = CVarGetString("gZeldaOnline.Party.StoryCode", "");
    if (!earlier.empty() && FindByCode(earlier) == nullptr) {
        CoopPlaythrough p;
        p.name = "Earlier playthrough";
        p.code = earlier;
        p.created = (int64_t)time(nullptr);
        p.hostNick = CVarGetString("gZeldaOnline.Nickname", "Player");
        hosted.push_back(p);
        Save();
    }
    SortByLastPlayed();
}

void CoopPlaythroughList::Save() const {
    nlohmann::json j;
    j["hosted"] = nlohmann::json::array();
    for (const auto& p : hosted) {
        j["hosted"].push_back({ { "name", p.name },
                                { "code", p.code },
                                { "created", p.created },
                                { "lastPlayed", p.lastPlayed },
                                { "lastFile", p.lastFile },
                                { "lastPlayers", p.lastPlayers },
                                { "hostNick", p.hostNick } });
    }
    j["joined"] = nlohmann::json::array();
    for (const auto& p : joined)
        j["joined"].push_back({ { "host", p.host }, { "lastFile", p.lastFile }, { "lastPlayed", p.lastPlayed } });
    const std::string path = ListPath();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return;
        out << j.dump(2);
    }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

CoopPlaythrough* CoopPlaythroughList::FindByCode(const std::string& code) {
    for (auto& p : hosted)
        if (p.code == code)
            return &p;
    return nullptr;
}

CoopPlaythroughList::Joined& CoopPlaythroughList::JoinedEntry(const std::string& host) {
    for (auto& p : joined)
        if (p.host == host)
            return p;
    Joined j;
    j.host = host;
    joined.push_back(j);
    return joined.back();
}

void CoopPlaythroughList::SortByLastPlayed() {
    std::stable_sort(hosted.begin(), hosted.end(), [](const CoopPlaythrough& a, const CoopPlaythrough& b) {
        return std::max(a.lastPlayed, a.created) > std::max(b.lastPlayed, b.created);
    });
}

// ---------------------------------------------------------------------------------------------- window

void CoopLobbyWindow::InitElement() {
}

void CoopLobbyWindow::EnsureListLoaded() {
    if (m_listLoaded)
        return;
    m_list.Load();
    m_listLoaded = true;
}

void CoopLobbyWindow::Open() {
    EnsureListLoaded();
    Show();
    m_autoShown = false;
}

static ZeldaOnlineClient* Client() {
    return ZeldaOnlineClient::Instance;
}

void CoopLobbyWindow::ConnectClient() {
    ZeldaOnlineClient* client = Client();
    if (client == nullptr)
        return;
    if (CVarGetInteger("gZeldaOnline.Enabled", 1) == 0) {
        CVarSetInteger("gZeldaOnline.Enabled", 1);
        SaveCVarsSoon();
        client->Enable();
    }
    const bool host = m_helper.hosting;
    client->LobbyConnect(host ? ZeldaOnlineClient::LobbyRole::Host : ZeldaOnlineClient::LobbyRole::Join, "127.0.0.1",
                         m_helper.port, host ? m_activeCode : std::string());
    m_clientStarted = true;
    if (!host)
        m_connectedTo = m_helper.hostName;
}

void CoopLobbyWindow::OnClientLeftLobby() {
    m_clientStarted = false;
}

void CoopLobbyWindow::StopAll() {
    if (Client() != nullptr)
        Client()->LobbyLeave();
    m_clientStarted = false;
    m_helper.Stop(false); // non blocking: the page shows "Stopping" until the helper is gone
    m_stopping = true;
}

void CoopLobbyWindow::StartHosting(int index) {
    if (index < 0 || index >= (int)m_list.hosted.size())
        return;
    CoopPlaythrough& p = m_list.hosted[index];
    m_message.clear();
    // the server keys the team save by the host's nickname and the team code: host under the same name as before
    if (p.hostNick.empty()) {
        p.hostNick = CVarGetString("gZeldaOnline.Nickname", "Player");
        m_list.Save();
    } else if (p.hostNick != CVarGetString("gZeldaOnline.Nickname", "Player")) {
        CVarSetString("gZeldaOnline.Nickname", p.hostNick.c_str());
        SaveCVarsSoon();
        snprintf(m_nameBuffer, sizeof(m_nameBuffer), "%s", p.hostNick.c_str());
        m_message = "Hosting as \"" + p.hostNick + "\", the name this playthrough is saved under.";
    }
    m_activeCode = p.code;
    m_lastRecordedFile = -2;
    m_page = Page::Session;
    m_stopping = false;
    m_helper.Start({ "host", "--no-game" });
}

void CoopLobbyWindow::StartJoining() {
    m_activeCode.clear();
    m_connectedTo.clear();
    m_lastRecordedFile = -2;
    m_page = Page::Session;
    m_stopping = false;
    m_message.clear();
    m_helper.Start({ "join", "--no-launch" });
}

// The save file this player uses for the playthrough, so the lobby can point at it next time.
void CoopLobbyWindow::RecordSaveFile() {
    ZeldaOnlineClient* client = Client();
    if (client == nullptr || client->GetLobbyRole() == ZeldaOnlineClient::LobbyRole::None)
        return;
    if (gPlayState == nullptr || gSaveContext.fileNum == 0xFF || gSaveContext.fileNum == m_lastRecordedFile)
        return;
    EnsureListLoaded();
    m_lastRecordedFile = gSaveContext.fileNum;
    const int64_t now = (int64_t)time(nullptr);
    if (client->GetLobbyRole() == ZeldaOnlineClient::LobbyRole::Host) {
        CoopPlaythrough* p = m_list.FindByCode(m_activeCode);
        if (p == nullptr)
            return;
        p->lastFile = gSaveContext.fileNum;
        p->lastPlayed = now;
        std::string names;
        if (ZeldaOnlineRoomWindow::Instance != nullptr) {
            for (const auto& e : ZeldaOnlineRoomWindow::Instance->Players()) {
                if (!e.isInParty)
                    continue;
                names += (names.empty() ? "" : ", ") + e.name;
            }
        }
        if (!names.empty())
            p->lastPlayers = names;
    } else if (!m_connectedTo.empty()) {
        auto& j = m_list.JoinedEntry(m_connectedTo);
        j.lastFile = gSaveContext.fileNum;
        j.lastPlayed = now;
    }
    m_list.Save();
}

void CoopLobbyWindow::UpdateAutoShow() {
    const bool inGame = gPlayState != nullptr;
    if (inGame && !m_wasInGame) {
        if (m_autoShown && IsVisible())
            Hide(); // out of the way while playing; the Network menu opens it again
        m_autoShown = false;
        // the online window carries the party list and the chat while playing
        ZeldaOnlineClient* client = Client();
        if (m_clientStarted && client != nullptr && ZeldaOnlineRoomWindow::Instance != nullptr &&
            client->GetLobbyRole() != ZeldaOnlineClient::LobbyRole::None)
            ZeldaOnlineRoomWindow::Instance->Show();
    } else if (!inGame && m_wasInGame) {
        m_seenTitle = false; // back at the title screen
    }
    m_wasInGame = inGame;

    if (inGame || m_seenTitle)
        return;
    m_seenTitle = true;
    if (CVarGetInteger(kShowOnTitleCVar, 1) == 0 || AutoTest::IsEnabled() || std::getenv("OOT_SERVER") != nullptr)
        return; // started by Host/Join over Steam, or by a test: that already connects
    ZeldaOnlineClient* client = Client();
    if (client != nullptr && client->IsConnectedToServer() &&
        client->GetLobbyRole() == ZeldaOnlineClient::LobbyRole::None)
        return; // connected another way (the online window)
    if (!IsVisible()) {
        EnsureListLoaded();
        Show();
        m_autoShown = true;
        // the lobby replaces the online window's address form at the title screen ("Connect by address" opens it)
        if (ZeldaOnlineRoomWindow::Instance != nullptr && !ZeldaOnlineRoomWindow::Instance->IsConnected())
            ZeldaOnlineRoomWindow::Instance->Hide();
    }
}

void CoopLobbyWindow::Draw() {
    // the helper and the client are followed also while the window is closed; the status file a few times a second
    const double now = ImGui::GetTime();
    if (now - m_lastPoll >= 0.2) {
        m_lastPoll = now;
        m_helper.Poll();
    }
    UpdateAutoShow();
    RecordSaveFile();

    if (m_helper.state == CoopLobbyHelper::State::Ready && !m_clientStarted && !m_stopping)
        ConnectClient();
    if (m_clientStarted && m_helper.state == CoopLobbyHelper::State::Failed) {
        if (Client() != nullptr)
            Client()->LobbyLeave();
        m_clientStarted = false;
        Open(); // tell the player what happened
    }
    if (m_stopping && !m_helper.Running()) {
        m_stopping = false;
        m_page = Page::Home;
    }

    if (!IsVisible())
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    const ImVec2 size = ImVec2(420.0f, 0.0f);
    auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool open = true;
    if (ImGui::Begin("Co-op Lobby###zo_coop_lobby", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
        DrawElement();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (!open) {
        Hide();
        m_autoShown = false;
    }
}

void CoopLobbyWindow::DrawElement() {
    switch (m_page) {
        case Page::Home:
            DrawHome();
            break;
        case Page::HostPick:
            DrawHostPick();
            break;
        case Page::Session:
            DrawSession();
            break;
    }
}

static bool BigButton(const char* label) {
    return ImGui::Button(label, ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f));
}

void CoopLobbyWindow::DrawNicknameField() {
    if (m_nameBuffer[0] == '\0')
        snprintf(m_nameBuffer, sizeof(m_nameBuffer), "%s", CVarGetString("gZeldaOnline.Nickname", "Player"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##zo_lobby_nick", "Player", m_nameBuffer, sizeof(m_nameBuffer))) {
        if (m_nameBuffer[0] != '\0') {
            CVarSetString("gZeldaOnline.Nickname", m_nameBuffer);
            SaveCVarsSoon();
        }
    }
}

void CoopLobbyWindow::DrawHome() {
    EnsureListLoaded();
    ImGui::TextWrapped("Play the story together over Steam. No IP addresses or port forwarding.");
    ImGui::Spacing();
    DrawNicknameField();
    ImGui::Spacing();

    const bool steam = CoopLobbyHelper::Supported() && !CoopLobbyHelper::FindExecutable().empty();
    ImGui::BeginDisabled(!steam);
    if (BigButton(ICON_FA_HOME "  Host a game")) {
        m_page = Page::HostPick;
        m_message.clear();
        m_selected = m_list.hosted.empty() ? -1 : 0;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Start a server and a friends-only Steam lobby, then pick which playthrough to continue");
    if (BigButton(ICON_FA_USERS "  Join a friend"))
        StartJoining();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Join a Steam friend who is hosting");
    ImGui::EndDisabled();

    if (!steam) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped(CoopLobbyHelper::Supported()
                               ? "OOTSteamLink was not found next to the game, so Steam hosting is off. It comes "
                                 "with the co-op download."
                               : "Steam hosting is not available on this system. Connect by address instead.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_GLOBE " Connect by address...")) {
        if (ZeldaOnlineRoomWindow::Instance != nullptr) {
            ZeldaOnlineRoomWindow::Instance->Show();
            ZeldaOnlineRoomWindow::Instance->CenterAndExpand();
        }
    }
    ImGui::SameLine();
    bool showOnTitle = CVarGetInteger(kShowOnTitleCVar, 1) != 0;
    if (ImGui::Checkbox("Show at the title screen", &showOnTitle)) {
        CVarSetInteger(kShowOnTitleCVar, showOnTitle ? 1 : 0);
        SaveCVarsSoon();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("You can always open it from the menu: Network > Zelda Online > Co-op Lobby");
}

void CoopLobbyWindow::DrawPlaythroughRow(int index) {
    CoopPlaythrough& p = m_list.hosted[index];
    ImGui::PushID(index);
    if (m_renaming == index) {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Done").x - ImGui::GetStyle().FramePadding.x * 4.0f);
        ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Done") && m_renameBuffer[0] != '\0') {
            p.name = m_renameBuffer;
            m_list.Save();
            m_renaming = -1;
        }
        ImGui::PopID();
        return;
    }

    std::string detail = "Last played " + FormatDate(p.lastPlayed);
    if (p.lastFile >= 0)
        detail += ", " + FileLabel(p.lastFile);
    if (!p.lastPlayers.empty())
        detail += ", with " + p.lastPlayers;
    if (ImGui::Selectable(("##row" + std::to_string(index)).c_str(), m_selected == index, 0,
                          ImVec2(0.0f, ImGui::GetTextLineHeight() * 2.0f + 2.0f)))
        m_selected = index;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        StartHosting(index);
    ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::TextDisabled("%s", detail.c_str());
    ImGui::EndGroup();
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("Rename")) {
            m_renaming = index;
            snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", p.name.c_str());
        }
        ImGui::TextDisabled("Team code: %s", p.code.c_str());
        if (ImGui::MenuItem("Copy team code"))
            ImGui::SetClipboardText(p.code.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from list"))
            m_confirmForget = index;
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void CoopLobbyWindow::DrawHostPick() {
    EnsureListLoaded();
    ImGui::TextUnformatted("Which playthrough?");
    ImGui::TextDisabled("Right-click one to rename it or see its team code.");
    ImGui::Separator();

    if (m_list.hosted.empty())
        ImGui::TextDisabled("No playthroughs yet. Start a new one below.");
    for (int i = 0; i < (int)m_list.hosted.size(); i++)
        DrawPlaythroughRow(i);

    if (m_confirmForget >= 0 && m_confirmForget < (int)m_list.hosted.size()) {
        ImGui::Separator();
        ImGui::TextWrapped("Remove \"%s\" from this list? The team progress stays on your server; paste its team code "
                           "(%s) into the party window to play it again.",
                           m_list.hosted[m_confirmForget].name.c_str(), m_list.hosted[m_confirmForget].code.c_str());
        if (ImGui::Button("Remove")) {
            m_list.hosted.erase(m_list.hosted.begin() + m_confirmForget);
            m_list.Save();
            m_selected = -1;
            m_confirmForget = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep"))
            m_confirmForget = -1;
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(m_selected < 0 || m_selected >= (int)m_list.hosted.size());
    if (BigButton(ICON_FA_PLAY "  Continue this playthrough"))
        StartHosting(m_selected);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("New playthrough");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Start new").x - ImGui::GetStyle().FramePadding.x * 4.0f);
    ImGui::InputTextWithHint("##zo_new_name", "Name, e.g. Saturday run", m_newName, sizeof(m_newName));
    ImGui::SameLine();
    if (ImGui::Button("Start new")) {
        CoopPlaythrough p;
        p.name =
            m_newName[0] != '\0' ? std::string(m_newName) : "Playthrough " + std::to_string(m_list.hosted.size() + 1);
        p.code = GenerateStoryCode();
        p.created = (int64_t)time(nullptr);
        p.hostNick = CVarGetString("gZeldaOnline.Nickname", "Player");
        m_list.hosted.insert(m_list.hosted.begin(), p);
        m_list.Save();
        m_newName[0] = '\0';
        StartHosting(0);
    }

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_CHEVRON_LEFT " Back"))
        m_page = Page::Home;
}

void CoopLobbyWindow::DrawJoinPick() {
    ImGui::TextUnformatted("Friends who are hosting");
    ImGui::Separator();
    if (m_helper.friends.empty()) {
        ImGui::TextDisabled("Looking for Steam friends who are hosting...");
        ImGui::TextWrapped("Ask your friend to choose Host a game. Accepting their Steam invite, or Join Game in the "
                           "Steam friends list, also works while this is open.");
    }
    for (size_t i = 0; i < m_helper.friends.size(); i++) {
        const auto& f = m_helper.friends[i];
        ImGui::PushID((int)i);
        if (BigButton((std::string(ICON_FA_SIGN_IN "  Join ") + f.name).c_str()))
            m_helper.Pick(f.lobby);
        ImGui::PopID();
    }
}

void CoopLobbyWindow::DrawSession() {
    const bool host = m_helper.hosting || !m_activeCode.empty();
    ZeldaOnlineClient* client = Client();
    const bool connected = client != nullptr && client->IsConnectedToServer() && m_clientStarted;
    const bool inParty = connected && client->PartyID() != 0;

    if (host) {
        CoopPlaythrough* p = m_list.FindByCode(m_activeCode);
        ImGui::Text(ICON_FA_HOME " Hosting: %s", p != nullptr ? p->name.c_str() : m_activeCode.c_str());
    } else if (!m_connectedTo.empty()) {
        ImGui::Text(ICON_FA_USERS " Playing with %s", m_connectedTo.c_str());
    } else {
        ImGui::TextUnformatted(ICON_FA_USERS " Join a friend");
    }
    ImGui::Separator();

    if (!m_message.empty()) {
        ImGui::TextWrapped("%s", m_message.c_str());
        ImGui::Spacing();
    }

    if (m_stopping) {
        ImGui::TextUnformatted(host ? "Saving the team progress and stopping..." : "Leaving...");
        return;
    }

    switch (m_helper.state) {
        case CoopLobbyHelper::State::Starting:
            ImGui::TextUnformatted(host ? "Starting the server and the Steam lobby..." : "Signing in to Steam...");
            break;
        case CoopLobbyHelper::State::Picking:
            DrawJoinPick();
            break;
        case CoopLobbyHelper::State::Ready: {
            if (m_helper.hostGone) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%s stopped hosting.", m_connectedTo.c_str());
                ImGui::PopStyleColor();
                break;
            }
            if (!connected) {
                ImGui::TextUnformatted("Connecting...");
                break;
            }
            if (!inParty) {
                ImGui::TextUnformatted(host ? "Creating the Story Co-op party..." : "Waiting for the host's party...");
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kGoodColor);
            ImGui::TextUnformatted(ICON_FA_CHECK " Story Co-op party ready");
            ImGui::PopStyleColor();

            if (ZeldaOnlineRoomWindow::Instance != nullptr) {
                int others = 0;
                for (const auto& e : ZeldaOnlineRoomWindow::Instance->Players()) {
                    if (!e.isInParty)
                        continue;
                    ImGui::BulletText("%s", e.name.c_str());
                    others++;
                }
                if (others == 0)
                    ImGui::TextDisabled(host ? "Nobody has joined yet. Friends choose Join a friend in their Co-op "
                                               "Lobby, or right-click you in Steam and choose Join Game."
                                             : "Nobody else is here yet.");
            }

            ImGui::Spacing();
            int lastFile = -1;
            if (host) {
                if (CoopPlaythrough* p = m_list.FindByCode(m_activeCode))
                    lastFile = p->lastFile;
            } else {
                for (const auto& j : m_list.joined)
                    if (j.host == m_connectedTo)
                        lastFile = j.lastFile;
            }
            if (gPlayState == nullptr) {
                if (lastFile >= 0)
                    ImGui::TextWrapped("Now choose a save file. Last time you played this with %s.",
                                       FileLabel(lastFile).c_str());
                else
                    ImGui::TextWrapped("Now choose a save file, or start a new one. Any file works: the team's "
                                       "progress is added to it.");
            } else {
                ImGui::TextWrapped("Playing %s. Your friends' progress is shared with this file.",
                                   FileLabel(gSaveContext.fileNum).c_str());
            }
            break;
        }
        case CoopLobbyHelper::State::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", m_helper.error.c_str());
            ImGui::PopStyleColor();
            break;
        case CoopLobbyHelper::State::Stopped:
        case CoopLobbyHelper::State::Idle:
            ImGui::TextUnformatted("Stopped.");
            break;
    }

    ImGui::Spacing();
    const bool finished =
        m_helper.state == CoopLobbyHelper::State::Failed || m_helper.state == CoopLobbyHelper::State::Stopped;
    if (finished) {
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT " Back")) {
            m_helper.state = CoopLobbyHelper::State::Idle;
            m_page = Page::Home;
            m_message.clear();
        }
        if (host) {
            ImGui::SameLine();
            if (ImGui::Button("Try again")) {
                CoopPlaythrough* p = m_list.FindByCode(m_activeCode);
                if (p != nullptr)
                    StartHosting((int)(p - m_list.hosted.data()));
            }
        }
    } else {
        if (ImGui::Button(host ? ICON_FA_STOP " Stop hosting" : ICON_FA_SIGN_OUT " Leave"))
            StopAll();
        if (host && ImGui::IsItemHovered())
            ImGui::SetTooltip("Ends the game for everybody. The team progress is saved first.");
    }
}

} // namespace ZeldaOnline
