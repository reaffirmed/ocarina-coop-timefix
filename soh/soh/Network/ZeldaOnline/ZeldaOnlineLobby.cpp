// Co-op Lobby window: host or join a Story Co-op game from the title screen, and choose the team playthrough.
// See ZeldaOnlineLobby.hpp for how it fits together.
#include "ZeldaOnlineLobby.hpp"
#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "PartySettings.hpp"
#include "soh/Enhancements/AutoTest/AutoTest.h"
#include "soh/SaveManager.h"

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

static const char* kHideAddressFormCVar = "gZeldaOnline.Lobby.HideAddressForm";
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
    if (fileNum == SaveManager::CoopFileNum)
        return "your co-op file";
    return fileNum >= 0 ? "File " + std::to_string(fileNum + 1) : "";
}

// ---------------------------------------------------------------------------------------------- playthrough list

// The party settings this player used last (the online window's party settings), the start for a new playthrough.
static PartySettings SettingsFromCVars() {
    PartySettings s;
    s.privateDungeons = CVarGetInteger("gZeldaOnline.Party.PrivateDungeons", 0) != 0;
    s.extraEnemies = CVarGetInteger("gZeldaOnline.Party.ExtraEnemies", 0) != 0;
    s.extraEnemyWeight = CVarGetFloat("gZeldaOnline.Party.ExtraEnemyWeight", 1.0f);
    s.healthMultiplier = CVarGetInteger("gZeldaOnline.Party.HealthMultiplier", 0) != 0;
    s.enemyHealthWeight = CVarGetFloat("gZeldaOnline.Party.EnemyHealthWeight", 1.0f);
    s.bossHealthWeight = CVarGetFloat("gZeldaOnline.Party.BossHealthWeight", 1.0f);
    s.damageMultiplier = CVarGetInteger("gZeldaOnline.Party.DamageMultiplier", 0) != 0;
    s.enemyDamageWeight = CVarGetFloat("gZeldaOnline.Party.EnemyDamageWeight", 1.0f);
    s.shareAmmoRupeesHp = CVarGetInteger("gZeldaOnline.Party.ShareAmmoRupeesHp", 0) != 0;
    s.storyCoop = true;
    return s;
}

static nlohmann::json SettingsToJson(const PartySettings& s) {
    nlohmann::json j = { { "privateDungeons", s.privateDungeons },     { "extraEnemies", s.extraEnemies },
             { "extraEnemyWeight", s.extraEnemyWeight },   { "healthMultiplier", s.healthMultiplier },
             { "enemyHealthWeight", s.enemyHealthWeight }, { "bossHealthWeight", s.bossHealthWeight },
             { "damageMultiplier", s.damageMultiplier },   { "enemyDamageWeight", s.enemyDamageWeight },
             { "shareAmmoRupeesHp", s.shareAmmoRupeesHp } };
    WriteCoopRulesJSON(s, j);
    return j;
}

static PartySettings SettingsFromJson(const nlohmann::json& j) {
    PartySettings s = SettingsFromCVars();
    if (!j.is_object())
        return s;
    s.privateDungeons = j.value("privateDungeons", s.privateDungeons);
    s.extraEnemies = j.value("extraEnemies", s.extraEnemies);
    s.extraEnemyWeight = std::clamp(j.value("extraEnemyWeight", s.extraEnemyWeight), 0.0f, 5.0f);
    s.healthMultiplier = j.value("healthMultiplier", s.healthMultiplier);
    s.enemyHealthWeight = std::clamp(j.value("enemyHealthWeight", s.enemyHealthWeight), 0.0f, 5.0f);
    s.bossHealthWeight = std::clamp(j.value("bossHealthWeight", s.bossHealthWeight), 0.0f, 5.0f);
    s.damageMultiplier = j.value("damageMultiplier", s.damageMultiplier);
    s.enemyDamageWeight = std::clamp(j.value("enemyDamageWeight", s.enemyDamageWeight), 0.0f, 5.0f);
    s.shareAmmoRupeesHp = j.value("shareAmmoRupeesHp", s.shareAmmoRupeesHp);
    ReadCoopRulesJSON(j, s);
    return s;
}

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
                p.settings = SettingsFromJson(e.value("settings", nlohmann::json()));
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
        p.settings = SettingsFromCVars();
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
                                { "hostNick", p.hostNick },
                                { "settings", SettingsToJson(p.settings) } });
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
    PartySettings settings;
    if (host) {
        if (CoopPlaythrough* p = m_list.FindByCode(m_activeCode))
            settings = p->settings;
        settings.storyCode = m_activeCode;
    }
    client->LobbyConnect(host ? ZeldaOnlineClient::LobbyRole::Host : ZeldaOnlineClient::LobbyRole::Join, "127.0.0.1",
                         m_helper.port, settings);
    m_clientStarted = true;
    if (!host)
        m_connectedTo = m_helper.hostName;
}

void CoopLobbyWindow::OnClientLeftLobby() {
    m_clientStarted = false;
}

CoopLobbyWindow::Status CoopLobbyWindow::SessionStatus() const {
    if (m_stopping)
        return Status::Stopping;
    ZeldaOnlineClient* client = ZeldaOnlineClient::Instance;
    switch (m_helper.state) {
        case CoopLobbyHelper::State::Idle:
        case CoopLobbyHelper::State::Stopped:
            return Status::Idle;
        case CoopLobbyHelper::State::Starting:
            return Status::Starting;
        case CoopLobbyHelper::State::Picking:
            return Status::Picking;
        case CoopLobbyHelper::State::Failed:
            return Status::Failed;
        case CoopLobbyHelper::State::Ready:
            break;
    }
    if (m_helper.hostGone)
        return Status::HostGone;
    if (client == nullptr || !client->IsConnectedToServer() || !m_clientStarted)
        return Status::Connecting;
    if (client->PartyID() == 0)
        return Status::FormingParty;
    return Status::Ready;
}

bool CoopLobbyWindow::SessionActive() const {
    const Status s = SessionStatus();
    return s != Status::Idle && s != Status::Failed;
}

std::vector<std::string> CoopLobbyWindow::PartyNames() const {
    std::vector<std::string> names;
    if (ZeldaOnlineRoomWindow::Instance != nullptr)
        for (const auto& e : ZeldaOnlineRoomWindow::Instance->Players())
            if (e.isInParty)
                names.push_back(e.name);
    return names;
}

int CoopLobbyWindow::LastFileForSession() {
    EnsureListLoaded();
    if (IsHosting()) {
        CoopPlaythrough* p = m_list.FindByCode(m_activeCode);
        return p != nullptr ? p->lastFile : -1;
    }
    for (const auto& j : m_list.joined)
        if (j.host == m_connectedTo)
            return j.lastFile;
    return -1;
}

std::string CoopLobbyWindow::PlayerName() const {
    return CVarGetString("gZeldaOnline.Nickname", "Player");
}

void CoopLobbyWindow::SetPlayerName(const std::string& name) {
    if (name.empty())
        return;
    snprintf(m_nameBuffer, sizeof(m_nameBuffer), "%s", name.c_str());
    if (Client() != nullptr) {
        Client()->SetNickname(name);
    } else {
        CVarSetString("gZeldaOnline.Nickname", name.c_str());
        SaveCVarsSoon();
    }
}

// ---------------------------------------------------------------------------------------------- co-op save files

// Letters, digits, '-' and '_' only: the name is a file name.
static std::string SafeFilePart(const std::string& text) {
    std::string out;
    for (char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out += c;
        if (out.size() >= 40)
            break;
    }
    return out;
}

std::filesystem::path CoopLobbyWindow::CoopFilePath() {
    const std::filesystem::path dir =
        std::filesystem::path(Ship::Context::GetPathRelativeToAppDirectory("Save")) / "coop";
    if (IsHosting()) {
        const std::string code = SafeFilePart(m_activeCode);
        return code.empty() ? std::filesystem::path() : dir / ("host-" + code + ".sav");
    }
    ZeldaOnlineClient* client = Client();
    if (client == nullptr || client->PartyID() == 0)
        return {};
    std::string key = SafeFilePart(client->CurrentPartySettings().storyCode);
    if (key.empty())
        key = SafeFilePart(m_connectedTo);
    return key.empty() ? std::filesystem::path() : dir / ("join-" + key + ".sav");
}

bool CoopLobbyWindow::CoopFileExists() {
    const std::filesystem::path path = CoopFilePath();
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

bool CoopLobbyWindow::PrepareCoopFile() {
    const std::filesystem::path path = CoopFilePath();
    if (path.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (SaveManager::Instance->CoopFile() != path)
        SaveManager::Instance->SetCoopFile(path);
    if (!std::filesystem::exists(path, ec))
        return false;
    if (!SaveManager::Instance->fileMetaInfo[SaveManager::CoopFileNum].valid)
        SaveManager::Instance->LoadCoopMeta();
    return true;
}

bool CoopLobbyWindow::CopySoloFile(int slot, std::string& error) {
    const std::filesystem::path path = CoopFilePath();
    if (path.empty() || slot < 0 || slot >= SaveManager::MaxFiles) {
        error = "The party is not ready yet.";
        return false;
    }
    const std::filesystem::path solo = SaveManager::Instance->SlotFileName(slot);
    std::error_code ec;
    if (!std::filesystem::exists(solo, ec)) {
        error = "File " + std::to_string(slot + 1) + " is empty.";
        return false;
    }
    SaveManager::Instance->ThreadPoolWait();
    std::filesystem::create_directories(path.parent_path(), ec);
    if (std::filesystem::exists(path, ec)) {
        std::filesystem::path backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Could not keep a backup of the co-op file: " + ec.message();
            return false;
        }
    }
    // the solo file is only read
    std::filesystem::copy_file(solo, path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Could not copy File " + std::to_string(slot + 1) + ": " + ec.message();
        return false;
    }
    SaveManager::Instance->SetCoopFile(path);
    SaveManager::Instance->LoadCoopMeta();
    return true;
}

std::string CoopLobbyWindow::SteamProblem() {
    static int checked = -1; // the folder does not change while the game runs
    static std::string problem;
    if (checked < 0) {
        checked = 1;
        if (!CoopLobbyHelper::Supported())
            problem = "Steam hosting is not available on this system.";
        else if (CoopLobbyHelper::FindHelperExecutable().empty())
            problem = "OOTSteamLink is missing from the game folder.";
    }
    return problem;
}

int CoopLobbyWindow::NewPlaythrough() {
    EnsureListLoaded();
    CoopPlaythrough p;
    p.name = "Playthrough " + std::to_string(m_list.hosted.size() + 1);
    p.code = GenerateStoryCode();
    p.created = (int64_t)time(nullptr);
    p.hostNick = CVarGetString("gZeldaOnline.Nickname", "Player");
    p.settings = SettingsFromCVars();
    m_list.hosted.insert(m_list.hosted.begin(), p);
    m_list.Save();
    return 0;
}

void CoopLobbyWindow::ClearFinished() {
    if (m_helper.state == CoopLobbyHelper::State::Failed || m_helper.state == CoopLobbyHelper::State::Stopped)
        m_helper.state = CoopLobbyHelper::State::Idle;
    m_page = Page::Home;
    m_message.clear();
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
    if (AutoTest::IsEnabled() || std::getenv("OOT_SERVER") != nullptr)
        return; // started by Host/Join over Steam, or by a test: that already connects
    // The Co-op screen of the file select (R) replaces the online window's address form at the title screen; the
    // address form is still in the Network menu and behind "Connect by address" in this window.
    if (CVarGetInteger(kHideAddressFormCVar, 1) != 0 && ZeldaOnlineRoomWindow::Instance != nullptr &&
        !ZeldaOnlineRoomWindow::Instance->IsConnected())
        ZeldaOnlineRoomWindow::Instance->Hide();
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
    // nobody picked a name yet: the Steam name is a better start than "Player"
    if (!m_helper.steamName.empty() && CVarGetString("gZeldaOnline.Nickname", "")[0] == '\0')
        SetPlayerName(m_helper.steamName.substr(0, 32));

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
    const ImVec2 size = ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f); // follows the menu's font scale
    auto vp = ImGui::GetMainViewport();
    // kept in the middle of the screen as the pages change size
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    // tall pages (many playthroughs, the story settings) scroll instead of running off the screen
    ImGui::SetNextWindowSizeConstraints(ImVec2(size.x, 0.0f), ImVec2(size.x, vp->Size.y * 0.9f));
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

    const bool steam = SteamProblem().empty();
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
    ImGui::TextDisabled("Also on the file select screen: press R for Co-op.");
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

    std::vector<std::string> details;
    details.push_back((p.lastPlayed > 0 ? "Last played " + FormatDate(p.lastPlayed) : "Not played from the lobby yet") +
                      (p.lastFile >= 0 ? ", " + FileLabel(p.lastFile) : std::string()));
    if (!p.lastPlayers.empty())
        details.push_back("With " + p.lastPlayers);
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * (float)(1 + details.size());
    if (ImGui::Selectable(("##row" + std::to_string(index)).c_str(), m_selected == index, 0, ImVec2(0.0f, rowHeight)))
        m_selected = index;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        StartHosting(index);
    ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(p.name.c_str());
    for (const auto& line : details)
        ImGui::TextDisabled("%s", line.c_str());
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

// The host's story settings for a playthrough. Returns true when one changed.
bool CoopLobbyWindow::DrawStorySettings(PartySettings& s) {
    bool changed = false;
    // a slider counts as changed when it is let go, not on every frame of a drag
    auto slider = [](const char* id, float* v, const char* format) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat(id, v, 0.0f, 5.0f, format, ImGuiSliderFlags_AlwaysClamp);
        return ImGui::IsItemDeactivatedAfterEdit();
    };
    auto tooltip = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", text);
    };
    ImGui::PushID("##zo_story_settings");
    ImGui::Indent();

    changed |= ImGui::Checkbox("Share ammo, rupees and health", &s.shareAmmoRupeesHp);
    tooltip("Off: everybody has their own arrows, seeds, bombs, rupees and hearts.\n"
            "On: one shared pool for the whole party.");

    if (ImGui::Checkbox("Private dungeons", &s.privateDungeons)) {
        changed = true;
        if (!s.privateDungeons) {
            s.extraEnemies = false;
            s.healthMultiplier = false;
            s.damageMultiplier = false;
        }
    }
    tooltip("Your party gets its own copy of every dungeon, apart from other players on the server.\n"
            "Needed for the difficulty settings below.");

    ImGui::BeginDisabled(!s.privateDungeons);
    changed |= ImGui::Checkbox("Extra enemies", &s.extraEnemies);
    tooltip("More enemies in dungeons, the more players the more enemies.");
    if (s.extraEnemies) {
        changed |= slider("##zo_lobby_extra", &s.extraEnemyWeight, "Amount %.1f");
    }
    changed |= ImGui::Checkbox("Tougher enemies", &s.healthMultiplier);
    tooltip("Enemies and bosses get more health for each player in the party:\n"
            "+0.25 x party size x the weight below.");
    if (s.healthMultiplier) {
        changed |= slider("##zo_lobby_enemy_hp", &s.enemyHealthWeight, "Enemy health %.1f");
        changed |= slider("##zo_lobby_boss_hp", &s.bossHealthWeight, "Boss health %.1f");
    }
    changed |= ImGui::Checkbox("Harder hits", &s.damageMultiplier);
    tooltip("Enemies and bosses deal more damage for each player in the party:\n"
            "+0.25 x party size x the weight below.");
    if (s.damageMultiplier) {
        changed |= slider("##zo_lobby_enemy_dmg", &s.enemyDamageWeight, "Enemy damage %.1f");
    }
    ImGui::EndDisabled();

    // Co-op rules (ZeldaOnlineCoopRules.cpp), the same ones the in-game lobby offers
    ImGui::Spacing();
    ImGui::TextDisabled("Co-op rules (all off by default)");
    auto combo = [&](const char* label, int* v, const char* const* names, int count, const char* help) {
        ImGui::SetNextItemWidth(160.0f);
        bool c = ImGui::Combo(label, v, names, count);
        tooltip(help);
        return c;
    };
    static const char* const kFf[] = { "Off", "Light", "Full" };
    changed |= combo("Friendly fire", &s.friendlyFire, kFf, 3,
                     "Swords, arrows, seeds and boomerangs hurt teammates.\n"
                     "Light: half damage and never the last heart. Never during cutscenes, text or votes.");
    changed |= ImGui::Checkbox("Revive with Zelda's Lullaby", &s.songRevive);
    tooltip("A player who dies lies down for a while. A teammate next to them plays Zelda's Lullaby\n"
            "(48 magic) to get them up with 3 hearts.");
    changed |= ImGui::Checkbox("Revive and heal with bottles", &s.bottleRevive);
    tooltip("Use a bottle next to a teammate to give it to them. Fairy: revive with 8 hearts.\n"
            "Red potion: revive or heal fully. Blue potion: same, plus magic. Green potion: magic.\n"
            "Milk: 5 hearts, no revive.");
    if (s.DownedEnabled()) {
        ImGui::Indent();
        int downed = s.downedSeconds >= 45 ? 1 : 0;
        static const char* const kDowned[] = { "30 seconds", "60 seconds" };
        if (combo("Time to revive", &downed, kDowned, 2, "How long a downed player waits before the Game Over.\n"
                                                         "Hold B to give up sooner.")) {
            s.downedSeconds = downed == 1 ? 60 : 30;
            changed = true;
        }
        ImGui::Unindent();
    }
    static const char* const kPenalty[] = { "Off", "Marker", "Marker + Dark Link" };
    changed |= combo("Death penalty", &s.deathPenalty, kPenalty, 3,
                     "Marker: a Game Over leaves some of your rupees where you fell. Go back to pick them up.\n"
                     "Dark Link: as adult, Dark Link guards the marker. He is only in your game.");
    if (s.deathPenalty != DEATH_PENALTY_OFF) {
        ImGui::Indent();
        static const char* const kRupees[] = { "None", "A quarter", "Half", "All" };
        changed |= combo("Rupees lost", &s.rupeesLost, kRupees, 4, "The share of rupees left in the marker.");
        if (s.deathPenalty == DEATH_PENALTY_DARK_LINK) {
            static const char* const kStrength[] = { "Weaker", "Water Temple" };
            changed |= combo("Dark Link strength", &s.darkLinkStrength, kStrength, 2,
                             "Weaker: half his health. Water Temple: as strong as there.");
        }
        ImGui::Unindent();
    }
    int cooldown = s.respawnCooldown >= 8 ? 2 : (s.respawnCooldown >= 3 ? 1 : 0);
    static const char* const kCooldown[] = { "None", "5 seconds", "10 seconds" };
    if (combo("Respawn cooldown", &cooldown, kCooldown, 3, "Extra wait before the Game Over menu after a death.")) {
        s.respawnCooldown = cooldown * 5;
        changed = true;
    }
    changed |= ImGui::Checkbox("Ready check", &s.readyCheck);
    tooltip("Walking into a dungeon or through a boss door waits until everyone in the area is ready (L).");

    ImGui::Unindent();
    ImGui::PopID();
    return changed;
}

void CoopLobbyWindow::OnStorySettingsChanged(CoopPlaythrough& p) {
    p.settings.storyCoop = true;
    p.settings.storyCode = p.code;
    m_list.Save();
    // while hosting this playthrough the party gets the change right away
    if (Client() != nullptr && p.code == m_activeCode && m_clientStarted)
        Client()->LobbyUpdateSettings(p.settings);
}

void CoopLobbyWindow::DrawHostPick() {
    EnsureListLoaded();
    ImGui::TextUnformatted("Which playthrough?");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Right-click one to rename it or see its team code.");
    ImGui::PopStyleColor();
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

    const bool haveSelection = m_selected >= 0 && m_selected < (int)m_list.hosted.size();
    if (haveSelection) {
        ImGui::Spacing();
        CoopPlaythrough& p = m_list.hosted[m_selected];
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(
                (std::string(ICON_FA_COG " Story settings: ") + p.name + "###zo_story_settings").c_str())) {
            if (DrawStorySettings(p.settings))
                OnStorySettingsChanged(p);
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!haveSelection);
    if (BigButton(ICON_FA_PLAY "  Host this playthrough"))
        StartHosting(m_selected);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("New playthrough");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Start new").x - ImGui::GetStyle().FramePadding.x * 4.0f);
    ImGui::InputTextWithHint("##zo_new_name", "Name, e.g. Saturday run", m_newName, sizeof(m_newName));
    ImGui::SameLine();
    if (ImGui::Button("Start new")) {
        const int index = NewPlaythrough();
        if (m_newName[0] != '\0') {
            m_list.hosted[index].name = m_newName;
            m_list.Save();
        }
        m_newName[0] = '\0';
        m_selected = 0; // its story settings show, then Host this playthrough
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
                (void)lastFile;
                // online play always uses the co-op file of this playthrough: solo files stay offline
                const bool exists = CoopFileExists();
                if (ImGui::Button(exists ? "Continue co-op file" : "Start a new co-op file"))
                    RequestPlay();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(exists ? "This playthrough has its own save file. Your solo files are not used "
                                            "online."
                                          : "This playthrough gets its own save file. Your solo files are not used "
                                            "online. To bring one along, choose Copy a solo file in the in-game "
                                            "lobby (R on the file select).");
                ImGui::PopStyleColor();
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
        CoopPlaythrough* p = host ? m_list.FindByCode(m_activeCode) : nullptr;
        if (p != nullptr && m_showSettings) {
            ImGui::SeparatorText("Story settings");
            if (DrawStorySettings(p->settings))
                OnStorySettingsChanged(*p);
            ImGui::Spacing();
        }
        if (ImGui::Button(host ? ICON_FA_STOP " Stop hosting" : ICON_FA_SIGN_OUT " Leave"))
            StopAll();
        if (host && ImGui::IsItemHovered())
            ImGui::SetTooltip("Ends the game for everybody. The team progress is saved first.");
        if (p != nullptr) {
            ImGui::SameLine();
            if (ImGui::Button(m_showSettings ? ICON_FA_COG " Hide story settings" : ICON_FA_COG " Story settings"))
                m_showSettings = !m_showSettings;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Changes apply to everybody right away");
        }
    }
}

} // namespace ZeldaOnline
