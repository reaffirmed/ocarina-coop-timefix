// The Co-op screen of the file select. Press R on the file select to turn the window to it (the same turn as the
// Options menu). It is drawn like the game's own menus: the file select window, the message font, the cursor arrow
// and the menu sounds, and it is used with the control stick (or the D-pad), A and B.
//
// Pages: Co-op (host or join) > Host: which playthrough > its story settings > hosting. Joining lists the Steam
// friends who host. Once the party is ready, "Choose a save file" (or B) turns back to the files while the session
// keeps running; R comes back to it. Everything behind it is CoopLobbyWindow (ZeldaOnlineLobby.cpp), which also has
// an overlay version in the Network menu.
#include "ZeldaOnlineLobbyFileSelect.h"
#include "ZeldaOnlineLobby.hpp"

#include <ship/Context.h>
#include <ship/window/Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/gamestates/ovl_file_choose/file_choose.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "soh/ShipUtils.h"
#include "soh/cvar_prefixes.h"
#include "soh_assets.h"
#include "textures/title_static/title_static.h"
#include "textures/icon_item_24_static/icon_item_24_static.h"
#include "soh/SaveManager.h"
#include "soh/frame_interpolation.h"

extern "C" {
void FileChoose_UpdateStickDirectionPromptAnim(GameState* thisx);
void FileChoose_DrawTextRec(GraphicsContext* gfxCtx, s32 r, s32 g, s32 b, s32 a, f32 x, f32 y, f32 z, s32 s, s32 t,
                            f32 dx, f32 dy);
void SpriteLoad(FileChooseContext* thisx, Sprite* sprite);
void SpriteDraw(FileChooseContext* thisx, Sprite* sprite, int left, int top, int width, int height);
}

using namespace ZeldaOnline;

namespace {

enum class Page { Home, HostPick, Settings, Session, CopyPick, CopyConfirm, Name };

struct Row {
    std::string label;
    std::string value; // an option shows its value on the right and changes with left / right
    bool enabled = true;
    int id = 0;
};

enum RowId {
    ROW_HOST,
    ROW_JOIN,
    ROW_BACK,
    ROW_PLAYTHROUGH, // + index
    ROW_NEW = 1000,
    ROW_SHARE,
    ROW_PRIVATE,
    ROW_EXTRA,
    ROW_ENEMY_HP,
    ROW_BOSS_HP,
    ROW_ENEMY_DMG,
    ROW_START_HOSTING,
    ROW_DONE,
    ROW_CHOOSE_FILE,
    ROW_SETTINGS,
    ROW_STOP,
    ROW_LAYOUT,
    ROW_PLAY,
    ROW_NAME,
    ROW_COPY,
    ROW_COPY_YES,
    ROW_COPY_NO,
    ROW_FRIENDLY_FIRE,
    ROW_SONG_REVIVE,
    ROW_BOTTLE_REVIVE,
    ROW_DOWNED_TIME,
    ROW_DEATH_PENALTY,
    ROW_RUPEES_LOST,
    ROW_DARK_LINK,
    ROW_RESPAWN,
    ROW_READY_CHECK,
    ROW_FRIEND = 2000, // + index
    ROW_COPY_SLOT = 3000, // + solo file
};

constexpr float kTextScale = 0.8f;

void AddCoopRuleRows(const PartySettings& s, std::vector<Row>& rows);
}
static std::string Fit(const std::string& text, float width, float scale);
namespace {
std::string SettingHelp(int id);

struct CoopScreen {
    Page page = Page::Home;
    int cursor = 0;
    int offset = 0;
    int settingsIndex = -1;    // the playthrough whose settings are shown
    bool settingsLive = false; // opened while hosting: changes apply right away, no "Start hosting" row
    int16_t alpha = 0;
    int arrowAnim = 0;
    int copySlot = -1;      // the solo file picked to copy into the co-op file
    std::string notice;     // a line shown on the session page after a copy
    std::string nameEdit;   // the name being typed on the name page
    int nameCol = 0, nameRow = 0;
};
CoopScreen sScreen;

constexpr const char* kModernCVar = "gZeldaOnline.Lobby.ModernLayout";

int RowsOnScreen() {
    if (CVarGetInteger(kModernCVar, 0) == 0)
        return sScreen.page == Page::Settings ? 5 : 6; // the settings keep two lines for their help
    return sScreen.page == Page::Settings ? 6 : 5; // the settings panel ends above the button legend
}

CoopLobbyWindow* Lobby() {
    return CoopLobbyWindow::Instance;
}

void PlaySfx(u16 sfx) {
    Audio_PlaySoundGeneral(sfx, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                           &gSfxDefaultReverb);
}

// The story settings are shown as steps, like the game's own options.
const float kWeights[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f };
constexpr int kWeightCount = sizeof(kWeights) / sizeof(kWeights[0]);

int WeightStep(float weight) {
    int best = 0;
    for (int i = 1; i < kWeightCount; i++)
        if (std::fabs(kWeights[i] - weight) < std::fabs(kWeights[best] - weight))
            best = i;
    return best;
}

std::string WeightText(bool on, float weight) {
    if (!on || weight <= 0.0f)
        return "Off";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "x%.1f", weight);
    return buf;
}

// Steps through Off and the weights; `on` and `weight` are the setting's switch and value.
void StepWeight(bool& on, float& weight, int dir) {
    int step = (on && weight > 0.0f) ? WeightStep(weight) + 1 : 0; // 0 is Off
    step = (step + dir + kWeightCount + 1) % (kWeightCount + 1);
    if (step == 0) {
        on = false;
    } else {
        on = true;
        weight = kWeights[step - 1];
    }
}

std::string FormatDate(int64_t t) {
    if (t <= 0)
        return "";
    time_t tt = (time_t)t;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d %b %Y", std::localtime(&tt));
    return buf;
}

float TextWidth(const std::string& text, float scale) {
    float w = 0.0f;
    for (unsigned char c : text)
        w += Ship_GetCharFontWidth(c) * (R_TEXT_CHAR_SCALE / 100.0f) * scale;
    return w;
}

// Splits text into lines that fit `width` (screen units).
std::vector<std::string> Wrap(const std::string& text, float width, float scale) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(tryLine, scale) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tryLine;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ')
            flushWord();
        else
            word += c;
    }
    flushWord();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

void DrawText(FileChooseContext* ctx, const std::string& text, int x, int y, u8 r, u8 g, u8 b,
              float scale = kTextScale) {
    Interface_DrawTextLine(ctx->state.gfxCtx, (char*)text.c_str(), x, y, r, g, b, sScreen.alpha, scale, true);
}

void DrawCentered(FileChooseContext* ctx, const std::string& text, int y, u8 r, u8 g, u8 b, float scale = kTextScale) {
    DrawText(ctx, text, (int)(160.0f - TextWidth(text, scale) / 2.0f), y, r, g, b, scale);
}

// ---------------------------------------------------------------------------------------------- pages

std::string Title() {
    CoopLobbyWindow* lobby = Lobby();
    switch (sScreen.page) {
        case Page::Home:
            return "Co-op";
        case Page::HostPick:
            return "Host: choose a playthrough";
        case Page::Settings:
            return "Story settings";
        case Page::Name:
            return "Your name";
        case Page::CopyPick:
            return "Copy a solo file";
        case Page::CopyConfirm:
            return "Replace the co-op file?";
        case Page::Session:
            if (lobby->IsHosting())
                return "Hosting";
            return lobby->ConnectedTo().empty() ? "Join a friend" : "Playing with " + lobby->ConnectedTo();
    }
    return "Co-op";
}

// Text above the list (white) and below it (grey), for the current page.
void Info(std::vector<std::string>& above, std::string& below, std::vector<Row>& rows) {
    CoopLobbyWindow* lobby = Lobby();
    (void)rows;
    if (sScreen.page == Page::Home) {
        const std::string problem = CoopLobbyWindow::SteamProblem();
        below = problem.empty() ? "Play the story together over Steam." : problem;
        return;
    }
    if (sScreen.page == Page::HostPick) {
        if (sScreen.cursor < (int)rows.size() && rows[sScreen.cursor].id >= ROW_PLAYTHROUGH &&
            rows[sScreen.cursor].id < ROW_NEW) {
            const CoopPlaythrough& p = lobby->Playthroughs().hosted[rows[sScreen.cursor].id - ROW_PLAYTHROUGH];
            below = p.lastPlayed > 0 ? "Last played " + FormatDate(p.lastPlayed) : "Not played yet";
            if (p.lastFile == SaveManager::CoopFileNum)
                below += ", co-op file";
            else if (p.lastFile >= 0)
                below += ", File " + std::to_string(p.lastFile + 1);
        } else {
            below = "A new team save, with its own story settings.";
        }
        return;
    }
    if (sScreen.page == Page::Settings) {
        below = SettingHelp(sScreen.cursor < (int)rows.size() ? rows[sScreen.cursor].id : -1);
        return;
    }
    if (sScreen.page == Page::CopyPick) {
        above.push_back("Copies a solo file into this playthrough's co-op file.");
        below = "The solo file itself is not changed.";
        return;
    }
    if (sScreen.page == Page::CopyConfirm) {
        above.push_back("Replace your co-op file with File " + std::to_string(sScreen.copySlot + 1) + "?");
        below = "The old co-op file is kept as a backup (.bak).";
        return;
    }
    // session
    switch (lobby->SessionStatus()) {
        case CoopLobbyWindow::Status::Starting:
            above.push_back(lobby->IsHosting() ? "Starting the server..." : "Signing in to Steam...");
            break;
        case CoopLobbyWindow::Status::Picking:
            if (lobby->HostingFriends().empty()) {
                above.push_back("Looking for friends who are hosting...");
                below = "A Steam invite works too.";
            } else {
                below = "Choose who to join.";
            }
            break;
        case CoopLobbyWindow::Status::Connecting:
            above.push_back("Connecting...");
            break;
        case CoopLobbyWindow::Status::FormingParty:
            above.push_back(lobby->IsHosting() ? "Creating the party..." : "Joining the party...");
            break;
        case CoopLobbyWindow::Status::Ready: {
            const auto names = lobby->PartyNames();
            if (names.empty()) {
                above.push_back(lobby->IsHosting() ? "Waiting for friends to join." : "Party ready.");
            } else {
                std::string list;
                for (const auto& n : names)
                    list += (list.empty() ? "" : ", ") + n;
                above.push_back("With: " + list);
            }
            if (!sScreen.notice.empty())
                below = sScreen.notice;
            else if (lobby->CoopFileExists())
                below = "Your co-op file for this playthrough. Solo files are never used online.";
            else
                below = "Starts a new file for this playthrough, or copy a solo file into it.";
            break;
        }
        case CoopLobbyWindow::Status::HostGone:
            above.push_back(lobby->ConnectedTo() + " stopped hosting.");
            break;
        case CoopLobbyWindow::Status::Failed:
            above.push_back(lobby->Error());
            break;
        case CoopLobbyWindow::Status::Stopping:
            above.push_back(lobby->IsHosting() ? "Saving and stopping..." : "Leaving...");
            break;
        case CoopLobbyWindow::Status::Idle:
            break;
    }
}

std::vector<Row> BuildRows() {
    CoopLobbyWindow* lobby = Lobby();
    std::vector<Row> rows;
    switch (sScreen.page) {
        case Page::Home: {
            const bool steam = CoopLobbyWindow::SteamProblem().empty();
            rows.push_back({ "Your name", Fit(lobby->PlayerName(), 70.0f, 0.7f), true, ROW_NAME });
            rows.push_back({ "Host a game", "", steam, ROW_HOST });
            rows.push_back({ "Join a friend", "", steam, ROW_JOIN });
            rows.push_back({ "Layout", CVarGetInteger(kModernCVar, 0) ? "Modern" : "Classic", true, ROW_LAYOUT });
            rows.push_back({ "Back", "", true, ROW_BACK });
            break;
        }
        case Page::HostPick: {
            auto& list = lobby->Playthroughs().hosted;
            for (int i = 0; i < (int)list.size(); i++)
                rows.push_back({ list[i].name, "", true, ROW_PLAYTHROUGH + i });
            rows.push_back({ "New playthrough", "", true, ROW_NEW });
            break;
        }
        case Page::Settings: {
            auto& list = lobby->Playthroughs().hosted;
            if (sScreen.settingsIndex < 0 || sScreen.settingsIndex >= (int)list.size())
                break;
            const PartySettings& s = list[sScreen.settingsIndex].settings;
            rows.push_back({ "Share ammo & health", s.shareAmmoRupeesHp ? "On" : "Off", true, ROW_SHARE });
            rows.push_back({ "Private dungeons", s.privateDungeons ? "On" : "Off", true, ROW_PRIVATE });
            rows.push_back({ "Extra enemies", WeightText(s.extraEnemies, s.extraEnemyWeight), true, ROW_EXTRA });
            rows.push_back({ "Enemy health", WeightText(s.healthMultiplier, s.enemyHealthWeight), true, ROW_ENEMY_HP });
            rows.push_back({ "Boss health", WeightText(s.healthMultiplier, s.bossHealthWeight), true, ROW_BOSS_HP });
            rows.push_back(
                { "Enemy damage", WeightText(s.damageMultiplier, s.enemyDamageWeight), true, ROW_ENEMY_DMG });
            AddCoopRuleRows(s, rows);
            if (sScreen.settingsLive)
                rows.push_back({ "Done", "", true, ROW_DONE });
            else
                rows.push_back({ "Start hosting", "", true, ROW_START_HOSTING });
            break;
        }
        case Page::Session: {
            const auto status = lobby->SessionStatus();
            const bool host = lobby->IsHosting();
            if (status == CoopLobbyWindow::Status::Picking) {
                const auto& friends = lobby->HostingFriends();
                for (int i = 0; i < (int)friends.size(); i++)
                    rows.push_back({ "Join " + friends[i].name, "", true, ROW_FRIEND + i });
                rows.push_back({ "Cancel", "", true, ROW_STOP });
            } else if (status == CoopLobbyWindow::Status::Failed) {
                rows.push_back({ "Back", "", true, ROW_BACK });
            } else if (status == CoopLobbyWindow::Status::Stopping || status == CoopLobbyWindow::Status::Idle) {
                // nothing to choose while it stops
            } else {
                if (status == CoopLobbyWindow::Status::Ready) {
                    const bool exists = lobby->CoopFileExists();
                    rows.push_back({ exists ? "Continue co-op file" : "Start a new co-op file", "", true, ROW_PLAY });
                    rows.push_back({ "Copy a solo file", "", true, ROW_COPY });
                }
                if (host && lobby->ActivePlaythrough() != nullptr)
                    rows.push_back({ "Story settings", "", true, ROW_SETTINGS });
                rows.push_back({ host ? "Stop hosting" : "Leave", "", true, ROW_STOP });
            }
            break;
        }
        case Page::CopyPick: {
            for (int i = 0; i < SaveManager::MaxFiles; i++) {
                const SaveFileMetaInfo& meta = SaveManager::Instance->fileMetaInfo[i];
                std::string label = "File " + std::to_string(i + 1);
                label += meta.valid ? "  (" + std::to_string(meta.healthCapacity / 16) + " hearts)" : "  (empty)";
                rows.push_back({ label, "", meta.valid, ROW_COPY_SLOT + i });
            }
            rows.push_back({ "Back", "", true, ROW_BACK });
            break;
        }
        case Page::CopyConfirm:
            rows.push_back({ "Yes, replace it", "", true, ROW_COPY_YES });
            rows.push_back({ "No", "", true, ROW_COPY_NO });
            break;
    }
    return rows;
}

// ---------------------------------------------------------------------------------------------- co-op rules

const char* FriendlyFireText(int v) {
    return v == FRIENDLY_FIRE_FULL ? "Full" : (v == FRIENDLY_FIRE_LIGHT ? "Light" : "Off");
}

const char* DeathPenaltyText(int v) {
    return v == DEATH_PENALTY_DARK_LINK ? "+ Dark Link" : (v == DEATH_PENALTY_MARKER ? "Marker" : "Off");
}

const char* RupeesLostText(int v) {
    static const char* kNames[] = { "None", "Quarter", "Half", "All" };
    return kNames[std::clamp(v, 0, 3)];
}

// The co-op rules rows. A row that only matters when another one is on shows up under it.
void AddCoopRuleRows(const PartySettings& s, std::vector<Row>& rows) {
    rows.push_back({ "Friendly fire", FriendlyFireText(s.friendlyFire), true, ROW_FRIENDLY_FIRE });
    rows.push_back({ "Revive with song", s.songRevive ? "On" : "Off", true, ROW_SONG_REVIVE });
    rows.push_back({ "Revive with bottles", s.bottleRevive ? "On" : "Off", true, ROW_BOTTLE_REVIVE });
    if (s.DownedEnabled())
        rows.push_back({ "  Time to revive", s.downedSeconds >= 60 ? "60 s" : "30 s", true, ROW_DOWNED_TIME });
    rows.push_back({ "Death penalty", DeathPenaltyText(s.deathPenalty), true, ROW_DEATH_PENALTY });
    if (s.deathPenalty != DEATH_PENALTY_OFF)
        rows.push_back({ "  Rupees lost", RupeesLostText(s.rupeesLost), true, ROW_RUPEES_LOST });
    if (s.deathPenalty == DEATH_PENALTY_DARK_LINK)
        rows.push_back({ "  Dark Link", s.darkLinkStrength ? "Water Temple" : "Weaker", true, ROW_DARK_LINK });
    rows.push_back({ "Respawn wait",
                     s.respawnCooldown >= 10 ? "10 s" : (s.respawnCooldown >= 5 ? "5 s" : "None"), true,
                     ROW_RESPAWN });
    rows.push_back({ "Ready check", s.readyCheck ? "On" : "Off", true, ROW_READY_CHECK });
}

// A short help line for the setting under the cursor.
std::string SettingHelp(int id) {
    switch (id) {
        case ROW_SHARE:
            return "Ammo, rupees and hearts picked up go to everybody.";
        case ROW_PRIVATE:
            return "The party gets its own copy of each dungeon.";
        case ROW_EXTRA:
            return "More enemies in dungeons. Turns on private dungeons.";
        case ROW_ENEMY_HP:
            return "Tougher enemies in dungeons. Turns on private dungeons.";
        case ROW_BOSS_HP:
            return "Tougher bosses. Turns on private dungeons.";
        case ROW_ENEMY_DMG:
            return "Enemies hit harder in dungeons. Turns on private dungeons.";
        case ROW_FRIENDLY_FIRE:
            return "Teammates' hits hurt. Light: half damage, never the last heart.";
        case ROW_SONG_REVIVE:
            return "Zelda's Lullaby revives a downed teammate: 3 hearts, 48 magic.";
        case ROW_BOTTLE_REVIVE:
            return "Fairies and potions revive teammates. Potions and milk heal.";
        case ROW_DOWNED_TIME:
            return "How long a downed player waits for help before the Game Over.";
        case ROW_DEATH_PENALTY:
            return "Marker: lost rupees wait where you fell. Dark Link guards them.";
        case ROW_RUPEES_LOST:
            return "How many of your rupees go into the marker when you die.";
        case ROW_DARK_LINK:
            return "Weaker: half his health. Water Temple: as strong as there.";
        case ROW_RESPAWN:
            return "Seconds before the Game Over menu shows after a death.";
        case ROW_READY_CHECK:
            return "Dungeon and boss doors wait until everybody there is ready.";
        case ROW_START_HOSTING:
            return "Everything here can be changed later while hosting.";
        case ROW_DONE:
            return "Changes apply to everybody right away.";
        default:
            return "Left / right changes a setting.";
    }
}

// ---------------------------------------------------------------------------------------------- co-op save file

void StartCoopFile(FileChooseContext* ctx) {
    CoopLobbyWindow* lobby = Lobby();
    if (lobby->CoopFilePath().empty()) {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        return;
    }
    const bool exists = lobby->PrepareCoopFile();
    // a new file is named after the nickname, in the 8 letters a file name has
    std::string name = CVarGetString("gZeldaOnline.Nickname", "Player");
    std::string fileName;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
            c == '.')
            fileName += c;
        if (fileName.size() >= 8)
            break;
    }
    if (fileName.empty())
        fileName = "Link";
    FileChoose_StartCoopFile(&ctx->state, exists ? 0 : 1, fileName.c_str());
}

void CopyPicked();

void GoTo(Page page, int cursor = 0) {
    sScreen.page = page;
    sScreen.cursor = cursor;
    sScreen.offset = std::max(0, cursor - RowsOnScreen() + 1);
}

void ExitToMain(FileChooseContext* ctx) {
    ctx->configMode = CM_COOP_TO_MAIN;
}

void ChangeOption(int id, int dir) {
    CoopLobbyWindow* lobby = Lobby();
    if (id == ROW_NAME) {
        if (dir > 0) { // A (or right) opens the name page
            sScreen.nameEdit = lobby->PlayerName();
            if (sScreen.nameEdit == "Player")
                sScreen.nameEdit.clear();
            sScreen.nameCol = sScreen.nameRow = 0;
            GoTo(Page::Name);
        }
        return;
    }
    if (id == ROW_LAYOUT) {
        CVarSetInteger(kModernCVar, CVarGetInteger(kModernCVar, 0) ? 0 : 1);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        return;
    }
    auto& list = lobby->Playthroughs().hosted;
    if (sScreen.settingsIndex < 0 || sScreen.settingsIndex >= (int)list.size())
        return;
    CoopPlaythrough& p = list[sScreen.settingsIndex];
    PartySettings& s = p.settings;
    switch (id) {
        case ROW_SHARE:
            s.shareAmmoRupeesHp = !s.shareAmmoRupeesHp;
            break;
        case ROW_PRIVATE:
            s.privateDungeons = !s.privateDungeons;
            if (!s.privateDungeons) { // the difficulty settings need private dungeons
                s.extraEnemies = false;
                s.healthMultiplier = false;
                s.damageMultiplier = false;
            }
            break;
        case ROW_EXTRA:
            StepWeight(s.extraEnemies, s.extraEnemyWeight, dir);
            break;
        case ROW_ENEMY_HP:
        case ROW_BOSS_HP: {
            // one switch for both: Off on one side keeps the other's weight
            bool on = s.healthMultiplier && (id == ROW_ENEMY_HP ? s.enemyHealthWeight : s.bossHealthWeight) > 0.0f;
            float& weight = id == ROW_ENEMY_HP ? s.enemyHealthWeight : s.bossHealthWeight;
            float& other = id == ROW_ENEMY_HP ? s.bossHealthWeight : s.enemyHealthWeight;
            const bool otherOn = s.healthMultiplier && other > 0.0f;
            StepWeight(on, weight, dir);
            if (!on)
                weight = 0.0f;
            if (on && !s.healthMultiplier && !otherOn)
                other = 0.0f; // turning one on leaves the other Off
            s.healthMultiplier = on || otherOn;
            break;
        }
        case ROW_ENEMY_DMG:
            StepWeight(s.damageMultiplier, s.enemyDamageWeight, dir);
            break;
        case ROW_FRIENDLY_FIRE:
            s.friendlyFire = (s.friendlyFire + dir + 3) % 3;
            break;
        case ROW_SONG_REVIVE:
            s.songRevive = !s.songRevive;
            break;
        case ROW_BOTTLE_REVIVE:
            s.bottleRevive = !s.bottleRevive;
            break;
        case ROW_DOWNED_TIME:
            s.downedSeconds = s.downedSeconds == 30 ? 60 : 30;
            break;
        case ROW_DEATH_PENALTY:
            s.deathPenalty = (s.deathPenalty + dir + 3) % 3;
            break;
        case ROW_RUPEES_LOST:
            s.rupeesLost = (s.rupeesLost + dir + 4) % 4;
            break;
        case ROW_DARK_LINK:
            s.darkLinkStrength = s.darkLinkStrength == 0 ? 1 : 0;
            break;
        case ROW_RESPAWN: {
            static const int kSteps[] = { 0, 5, 10 };
            int i = s.respawnCooldown >= 10 ? 2 : (s.respawnCooldown >= 5 ? 1 : 0);
            s.respawnCooldown = kSteps[(i + dir + 3) % 3];
            break;
        }
        case ROW_READY_CHECK:
            s.readyCheck = !s.readyCheck;
            break;
        default:
            return;
    }
    if ((s.extraEnemies || s.healthMultiplier || s.damageMultiplier) && !s.privateDungeons)
        s.privateDungeons = true;
    lobby->OnStorySettingsChanged(p);
}

void Activate(FileChooseContext* ctx, const Row& row) {
    CoopLobbyWindow* lobby = Lobby();
    if (!row.enabled) {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        return;
    }
    PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    if (row.id >= ROW_COPY_SLOT) {
        sScreen.copySlot = row.id - ROW_COPY_SLOT;
        if (lobby->CoopFileExists())
            GoTo(Page::CopyConfirm, 1); // on "No": a stray A does not replace it
        else
            CopyPicked();
        return;
    }
    if (row.id >= ROW_FRIEND) {
        const auto& friends = lobby->HostingFriends();
        const int i = row.id - ROW_FRIEND;
        if (i < (int)friends.size())
            lobby->JoinFriend(friends[i]);
        return;
    }
    if (row.id >= ROW_PLAYTHROUGH && row.id < ROW_NEW) {
        sScreen.settingsIndex = row.id - ROW_PLAYTHROUGH;
        sScreen.settingsLive = false;
        GoTo(Page::Settings);
        GoTo(Page::Settings, std::max(0, (int)BuildRows().size() - 1)); // on "Start hosting": A A hosts it as it was
        return;
    }
    switch (row.id) {
        case ROW_HOST:
            GoTo(Page::HostPick);
            break;
        case ROW_JOIN:
            lobby->StartJoining();
            GoTo(Page::Session);
            break;
        case ROW_BACK:
            if (sScreen.page == Page::CopyPick) {
                GoTo(Page::Session, 1);
            } else if (sScreen.page == Page::Session) {
                lobby->ClearFinished();
                GoTo(Page::Home);
            } else {
                ExitToMain(ctx);
            }
            break;
        case ROW_NEW:
            sScreen.settingsIndex = lobby->NewPlaythrough();
            sScreen.settingsLive = false;
            GoTo(Page::Settings);
            break;
        case ROW_START_HOSTING:
            lobby->StartHosting(sScreen.settingsIndex);
            GoTo(Page::Session);
            break;
        case ROW_DONE:
            GoTo(Page::Session);
            break;
        case ROW_CHOOSE_FILE:
            ExitToMain(ctx);
            break;
        case ROW_PLAY:
            StartCoopFile(ctx);
            break;
        case ROW_COPY:
            sScreen.notice.clear();
            GoTo(Page::CopyPick);
            break;
        case ROW_COPY_YES:
            CopyPicked();
            break;
        case ROW_COPY_NO:
            GoTo(Page::CopyPick, std::max(0, sScreen.copySlot));
            break;
        case ROW_SETTINGS: {
            CoopPlaythrough* p = lobby->ActivePlaythrough();
            auto& list = lobby->Playthroughs().hosted;
            if (p == nullptr)
                break;
            sScreen.settingsIndex = (int)(p - list.data());
            sScreen.settingsLive = true;
            GoTo(Page::Settings);
            break;
        }
        case ROW_STOP:
            lobby->StopAll();
            break;
        default:
            break;
    }
}

void Back(FileChooseContext* ctx) {
    CoopLobbyWindow* lobby = Lobby();
    PlaySfx(NA_SE_SY_FSEL_CLOSE);
    switch (sScreen.page) {
        case Page::Home:
            ExitToMain(ctx);
            break;
        case Page::HostPick:
            GoTo(Page::Home);
            break;
        case Page::Name:
            GoTo(Page::Home);
            break;
        case Page::CopyPick:
            GoTo(Page::Session, 1);
            break;
        case Page::CopyConfirm:
            GoTo(Page::CopyPick, std::max(0, sScreen.copySlot));
            break;
        case Page::Settings:
            if (sScreen.settingsLive)
                GoTo(Page::Session);
            else
                GoTo(Page::HostPick, std::max(0, sScreen.settingsIndex));
            break;
        case Page::Session: {
            const auto status = lobby->SessionStatus();
            if (status == CoopLobbyWindow::Status::Picking) {
                lobby->StopAll(); // cancel joining
            } else if (status == CoopLobbyWindow::Status::Failed || status == CoopLobbyWindow::Status::Idle) {
                lobby->ClearFinished();
                GoTo(Page::Home);
            } else {
                ExitToMain(ctx); // the session keeps running; R comes back here
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------- name page
//
// A letter grid like the game's own name entry: the stick or D-pad moves, A types, B deletes, Start or "OK" saves.

const char* const kNameGrid[] = { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789-_." };
constexpr int kNameGridRows = 5;
constexpr int kNameCols = 13;
constexpr int kNameMax = 16;
// the last row: Space, Delete, OK, each a third of the width
constexpr int kNameActionRow = kNameGridRows;

bool NameSave() {
    std::string name = sScreen.nameEdit;
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.empty()) {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        return false;
    }
    Lobby()->SetPlayerName(name);
    PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    GoTo(Page::Home);
    return true;
}

void NameType(char c) {
    if ((int)sScreen.nameEdit.size() >= kNameMax) {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        return;
    }
    sScreen.nameEdit += c;
    PlaySfx(NA_SE_SY_FSEL_DECIDE_S);
}

void NameDelete() {
    if (sScreen.nameEdit.empty()) {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        return;
    }
    sScreen.nameEdit.pop_back();
    PlaySfx(NA_SE_SY_FSEL_CLOSE);
}

void UpdateNamePage(FileChooseContext* ctx) {
    Input* input = &ctx->state.input[0];
    const bool dpad = CVarGetInteger(CVAR_SETTING("DpadInText"), 0);
    int dx = 0, dy = 0;
    if (ctx->stickRelX > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DRIGHT)))
        dx = 1;
    else if (ctx->stickRelX < -30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DLEFT)))
        dx = -1;
    if (ctx->stickRelY > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DUP)))
        dy = -1;
    else if (ctx->stickRelY < -30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DDOWN)))
        dy = 1;
    if (dx != 0 || dy != 0) {
        const int rowsTotal = kNameGridRows + 1;
        if (dy != 0) {
            // moving between the grid and the three wide buttons keeps the column roughly in place
            const int oldRow = sScreen.nameRow;
            sScreen.nameRow = (sScreen.nameRow + dy + rowsTotal) % rowsTotal;
            if (sScreen.nameRow == kNameActionRow && oldRow != kNameActionRow)
                sScreen.nameCol = std::min(2, sScreen.nameCol * 3 / kNameCols);
            else if (oldRow == kNameActionRow && sScreen.nameRow != kNameActionRow)
                sScreen.nameCol = sScreen.nameCol * kNameCols / 3 + 2;
        }
        const int cols = sScreen.nameRow == kNameActionRow ? 3 : kNameCols;
        sScreen.nameCol = (sScreen.nameCol + dx + cols) % cols;
        PlaySfx(NA_SE_SY_FSEL_CURSOR);
    }

    if (CHECK_BTN_ALL(input->press.button, BTN_START)) {
        NameSave();
    } else if (CHECK_BTN_ALL(input->press.button, BTN_B)) {
        if (sScreen.nameEdit.empty()) {
            PlaySfx(NA_SE_SY_FSEL_CLOSE);
            GoTo(Page::Home); // nothing typed: back, the name stays as it was
        } else {
            NameDelete();
        }
    } else if (CHECK_BTN_ALL(input->press.button, BTN_A)) {
        if (sScreen.nameRow == kNameActionRow) {
            if (sScreen.nameCol == 0)
                NameType(' ');
            else if (sScreen.nameCol == 1)
                NameDelete();
            else
                NameSave();
        } else {
            NameType(kNameGrid[sScreen.nameRow][sScreen.nameCol]);
        }
    }
}

void CopyPicked() {
    CoopLobbyWindow* lobby = Lobby();
    std::string error;
    if (lobby->CopySoloFile(sScreen.copySlot, error)) {
        sScreen.notice = "File " + std::to_string(sScreen.copySlot + 1) + " copied. Choose Continue co-op file to play.";
        GoTo(Page::Session, 0);
    } else {
        PlaySfx(NA_SE_SY_FSEL_ERROR);
        sScreen.notice = error;
        GoTo(Page::Session, 1);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------- entry points

extern "C" int CoopLobby_BlockSoloFile(FileChooseContext* ctx) {
    (void)ctx;
    CoopLobbyWindow* lobby = Lobby();
    if (lobby == nullptr || !lobby->SessionActive())
        return 0;
    lobby->soloBlockedFrames = 80;
    return 1;
}

extern "C" int CoopLobby_FileSelectCheckOpen(FileChooseContext* ctx) {
    Input* input = &ctx->state.input[0];
    if (Lobby() != nullptr && Lobby()->TakePlayRequest() &&
        Lobby()->SessionStatus() == CoopLobbyWindow::Status::Ready) {
        StartCoopFile(ctx);
        return 1;
    }
    if (Lobby() == nullptr || !CHECK_BTN_ALL(input->press.button, BTN_R))
        return 0;
    PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    ctx->prevConfigMode = ctx->configMode;
    ctx->configMode = CM_ROTATE_TO_COOP_MENU;
    ctx->warningLabel = FS_WARNING_NONE;
    return 1;
}

static void Rect(FileChooseContext* ctx, int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a);

extern "C" void CoopLobby_FileSelectDrawPrompt(FileChooseContext* ctx) {
    CoopLobbyWindow* lobby = Lobby();
    if (lobby == nullptr)
        return;
    std::string text = "R: Co-op";
    u8 r = 255, g = 255, b = 255;
    switch (lobby->SessionStatus()) {
        case CoopLobbyWindow::Status::Idle:
            break;
        case CoopLobbyWindow::Status::Ready:
            text += lobby->IsHosting() ? " - hosting" : " - with " + lobby->ConnectedTo();
            r = 120;
            b = 120;
            break;
        case CoopLobbyWindow::Status::Failed:
        case CoopLobbyWindow::Status::HostGone:
            text += " - stopped";
            g = 120;
            b = 120;
            break;
        default:
            text += " - connecting";
            b = 80;
            break;
    }
    OPEN_DISPS(ctx->state.gfxCtx);
    // the file buttons leave a combiner that ignores the primitive colour: set the one the text needs
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);
    CLOSE_DISPS(ctx->state.gfxCtx);
    const int16_t saved = sScreen.alpha;
    sScreen.alpha = std::min<int16_t>(ctx->windowAlpha, 255);
    // top right, above the window: the bottom has the A / B prompts
    DrawText(ctx, text, (int)(292.0f - TextWidth(text, 0.7f)), 20, r, g, b, 0.7f);
    if (lobby->soloBlockedFrames > 0) {
        lobby->soloBlockedFrames--;
        const std::string why = "Solo files stay offline. Press R, then Continue co-op file.";
        const int w = (int)TextWidth(why, 0.65f);
        Rect(ctx, 160 - w / 2 - 4, 31, w + 8, 11, 0, 0, 0, 150); // readable on the bright sky too
        DrawText(ctx, why, 160 - w / 2, 32, 255, 140, 140, 0.65f);
    }
    sScreen.alpha = saved;
}

extern "C" void FileChoose_RotateToCoop(GameState* gameState) {
    FileChooseContext* ctx = (FileChooseContext*)gameState;
    ctx->windowRot += VREG(16);
    if (ctx->windowRot >= 314.0f) {
        ctx->windowRot = 314.0f;
        ctx->configMode = CM_START_COOP_MENU;
        sScreen.alpha = 0;
        // back where it was: the running session, else the start
        CoopLobbyWindow* lobby = Lobby();
        if (lobby != nullptr && lobby->SessionStatus() != CoopLobbyWindow::Status::Idle)
            GoTo(Page::Session);
        else
            GoTo(Page::Home);
    }
}

extern "C" void FileChoose_StartCoopMenu(GameState* gameState) {
    FileChooseContext* ctx = (FileChooseContext*)gameState;
    sScreen.alpha = std::min<int16_t>(sScreen.alpha + 25, 255);
    if (sScreen.alpha >= 255)
        ctx->configMode = CM_COOP_MENU;
}

extern "C" void FileChoose_UpdateCoopMenu(GameState* gameState) {
    FileChooseContext* ctx = (FileChooseContext*)gameState;
    CoopLobbyWindow* lobby = Lobby();
    Input* input = &ctx->state.input[0];
    const bool dpad = CVarGetInteger(CVAR_SETTING("DpadInText"), 0);
    FileChoose_UpdateStickDirectionPromptAnim(gameState);
    sScreen.arrowAnim = (sScreen.arrowAnim + 1) % 30;
    if (lobby == nullptr) {
        ExitToMain(ctx);
        return;
    }

    if (sScreen.page == Page::Name) {
        UpdateNamePage(ctx);
        return;
    }

    // a stop that finished: back to the start
    if (sScreen.page == Page::Session && lobby->SessionStatus() == CoopLobbyWindow::Status::Idle)
        GoTo(Page::Home);

    std::vector<Row> rows = BuildRows();
    if (rows.empty()) {
        sScreen.cursor = 0;
    } else {
        sScreen.cursor = std::clamp(sScreen.cursor, 0, (int)rows.size() - 1);
    }

    if (!rows.empty() &&
        (std::abs(ctx->stickRelY) > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DDOWN | BTN_DUP)))) {
        const bool down = ctx->stickRelY < -30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DDOWN));
        sScreen.cursor = (sScreen.cursor + (down ? 1 : -1) + (int)rows.size()) % (int)rows.size();
        PlaySfx(NA_SE_SY_FSEL_CURSOR);
    }
    if (sScreen.cursor < sScreen.offset)
        sScreen.offset = sScreen.cursor;
    if (sScreen.cursor >= sScreen.offset + RowsOnScreen())
        sScreen.offset = sScreen.cursor - RowsOnScreen() + 1;

    if (!rows.empty() && !rows[sScreen.cursor].value.empty() &&
        (std::abs(ctx->stickRelX) > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DLEFT | BTN_DRIGHT)))) {
        const bool right = ctx->stickRelX > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DRIGHT));
        ChangeOption(rows[sScreen.cursor].id, right ? 1 : -1);
        PlaySfx(NA_SE_SY_FSEL_CURSOR);
    }

    if (CHECK_BTN_ALL(input->press.button, BTN_B)) {
        Back(ctx);
    } else if (CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_START)) {
        if (!rows.empty()) {
            if (!rows[sScreen.cursor].value.empty())
                ChangeOption(rows[sScreen.cursor].id, 1), PlaySfx(NA_SE_SY_FSEL_CURSOR);
            else
                Activate(ctx, rows[sScreen.cursor]);
        }
    }
}

// The classic layout: the list on the face of the file select window (the face of the Options menu).

static constexpr int kListX = 72;
static constexpr int kListY = 82;
static constexpr int kRowStep = 16;

static void DrawNamePage(FileChooseContext* ctx, bool modern);

static void DrawClassic(FileChooseContext* ctx) {
    if (sScreen.page == Page::Name) {
        DrawCentered(ctx, Title(), 58, 255, 255, 255, 0.9f);
        DrawNamePage(ctx, false);
        return;
    }
    OPEN_DISPS(ctx->state.gfxCtx);

    std::vector<Row> rows = BuildRows();
    std::vector<std::string> above;
    std::string below;
    Info(above, below, rows);

    DrawCentered(ctx, Title(), 58, 255, 255, 255, 0.9f);

    // status lines above the list (wrapped), then the list
    int y = kListY;
    for (const auto& text : above) {
        for (const auto& line : Wrap(text, 176.0f, kTextScale)) {
            DrawText(ctx, line, kListX, y, 200, 200, 200);
            y += 14;
        }
    }
    if (!above.empty())
        y += 4;

    const int first = sScreen.offset;
    const int visible = std::max(1, RowsOnScreen() - (int)((y - kListY + kRowStep - 1) / kRowStep));
    const int last = std::min((int)rows.size(), first + visible);
    const int listTop = y;
    for (int i = first; i < last; i++) {
        const Row& row = rows[i];
        const bool selected = i == sScreen.cursor;
        u8 r = 255, g = 255, b = selected ? 80 : 255;
        if (!row.enabled)
            r = g = b = 100;
        DrawText(ctx, row.label, kListX, y, r, g, b);
        if (!row.value.empty()) {
            const float w = TextWidth(row.value, kTextScale);
            DrawText(ctx, row.value, (int)(244.0f - w), y, 255, 255, selected ? 80 : 255);
            if (selected) {
                // arrows around the value, like the game's options
                Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
                gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
                gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowCursorTex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 24, 0,
                                    G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, G_TX_NOMASK, G_TX_NOLOD,
                                    G_TX_NOLOD);
                FileChoose_DrawTextRec(ctx->state.gfxCtx, ctx->stickLeftPrompt.arrowColorR,
                                       ctx->stickLeftPrompt.arrowColorG, ctx->stickLeftPrompt.arrowColorB,
                                       sScreen.alpha, 238.0f - w, (f32)(y + 5), 0.42f, 0, 0, -1.0f, 1.0f);
                FileChoose_DrawTextRec(ctx->state.gfxCtx, ctx->stickRightPrompt.arrowColorR,
                                       ctx->stickRightPrompt.arrowColorG, ctx->stickRightPrompt.arrowColorB,
                                       sScreen.alpha, 250.0f, (f32)(y + 5), 0.42f, 0, 0, 1.0f, 1.0f);
            }
        }
        if (selected) {
            // the cursor arrow in front of the row
            Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowCursorTex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 24, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, G_TX_NOMASK, G_TX_NOLOD,
                                G_TX_NOLOD);
            FileChoose_DrawTextRec(ctx->state.gfxCtx, ctx->stickRightPrompt.arrowColorR,
                                   ctx->stickRightPrompt.arrowColorG, ctx->stickRightPrompt.arrowColorB, sScreen.alpha,
                                   kListX - 8, (f32)(y + 5), 0.42f, 0, 0, 1.0f, 1.0f);
        }
        y += kRowStep;
    }

    // more rows above / below
    if (first > 0 || last < (int)rows.size()) {
        Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, sScreen.alpha);
        if (first > 0) {
            const int ay = listTop - 10 - sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowUpTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, 156 << 2, ay << 2, (156 + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE,
                                    0, 0, (1 << 11), (1 << 11));
        }
        if (last < (int)rows.size()) {
            const int ay = y - 2 + sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowDownTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, 156 << 2, ay << 2, (156 + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE,
                                    0, 0, (1 << 11), (1 << 11));
        }
    }

    // the hint under the list
    if (!below.empty()) {
        const bool problem = sScreen.page == Page::Home && !CoopLobbyWindow::SteamProblem().empty();
        // under the list, and under its "more" arrow when there is one
        const bool more = last < (int)rows.size();
        int by = std::max(y + (more ? 10 : 4), kListY + RowsOnScreen() * kRowStep - 6);
        for (const auto& line : Wrap(below, 176.0f, 0.7f)) {
            DrawText(ctx, line, kListX, by, problem ? 255 : 170, problem ? 120 : 170, problem ? 120 : 170, 0.7f);
            by += 12;
        }
    }

    CLOSE_DISPS(ctx->state.gfxCtx);
}

// ---------------------------------------------------------------------------------------------- drawing
//
// A full screen lobby over the file select's sky (320 x 240 screen units):
//   top:          "Co-op" and the page name, a line under them, the status under the line
//   left column:  the choices as file select buttons
//   bottom left:  a card with the playthrough (or friend) and its progress icons
//   right column: the party, "You" first
//   middle:       left free for Link
// The story settings use one wide panel instead of the columns.

static SaveFileMetaInfo* Save_GetSaveMetaInfo(int file) {
    return &SaveManager::Instance->fileMetaInfo[file];
}

static constexpr int kLeftX = 20;
static constexpr int kLeftW = 118;
static constexpr int kTopY = 50;
static constexpr int kButtonH = 16;
static constexpr int kButtonStep = 19;
static constexpr int kCardY = 146;
static constexpr int kCardH = 54;
static constexpr int kRightX = 194;
static constexpr int kRightW = 108;
static constexpr int kWideX = 44;
static constexpr int kWideW = 232;
static constexpr int kWideStep = 17;

static u8 A(int a) {
    return (u8)std::clamp(a * sScreen.alpha / 255, 0, 255);
}

static void Rect(FileChooseContext* ctx, int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a) {
    OPEN_DISPS(ctx->state.gfxCtx);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, r, g, b, A(a));
    gDPFillRectangle(POLY_OPA_DISP++, x, y, x + w, y + h);
    CLOSE_DISPS(ctx->state.gfxCtx);
}

// a panel: dark glass with a thin blue edge
static void Panel(FileChooseContext* ctx, int x, int y, int w, int h) {
    Rect(ctx, x, y, w, h, 0, 10, 40, 150);
    Rect(ctx, x, y, w, 1, 100, 150, 255, 200);
    Rect(ctx, x, y + h - 1, w, 1, 100, 150, 255, 120);
    Rect(ctx, x, y, 1, h, 100, 150, 255, 120);
    Rect(ctx, x + w - 1, y, 1, h, 100, 150, 255, 120);
}

// a button made from the file select's name box, stretched
static void Button(FileChooseContext* ctx, int x, int y, int w, bool selected, bool enabled) {
    OPEN_DISPS(ctx->state.gfxCtx);
    Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    if (!enabled)
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 100, 100, 100, A(200));
    else if (selected)
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 140, 190, 255, A(255));
    else
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 100, 150, 255, A(200));
    gDPLoadTextureBlock(POLY_OPA_DISP++, gFileSelNameBoxTex, G_IM_FMT_IA, G_IM_SIZ_16b, 108, 16, 0,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD,
                        G_TX_NOLOD);
    // from texel 8: the name box has a mark on its left end
    gSPWideTextureRectangle(POLY_OPA_DISP++, x << 2, y << 2, (x + w) << 2, (y + kButtonH) << 2, G_TX_RENDERTILE, 8 << 5,
                            0, (1 << 10) * 100 / w, 1 << 10);
    CLOSE_DISPS(ctx->state.gfxCtx);
}

static void Arrow(FileChooseContext* ctx, int x, int y, float dir) {
    OPEN_DISPS(ctx->state.gfxCtx);
    Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowCursorTex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 24, 0,
                        G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    const auto& prompt = dir < 0 ? ctx->stickLeftPrompt : ctx->stickRightPrompt;
    FileChoose_DrawTextRec(ctx->state.gfxCtx, prompt.arrowColorR, prompt.arrowColorG, prompt.arrowColorB, sScreen.alpha,
                           (f32)x, (f32)y, 0.42f, 0, 0, dir, 1.0f);
    CLOSE_DISPS(ctx->state.gfxCtx);
}

static void TextAlpha(FileChooseContext* ctx, const std::string& text, int x, int y, u8 r, u8 g, u8 b, float scale) {
    OPEN_DISPS(ctx->state.gfxCtx);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);
    CLOSE_DISPS(ctx->state.gfxCtx);
    DrawText(ctx, text, x, y, r, g, b, scale);
}

// Cuts text to fit `width`, with "..." at the end.
static std::string Fit(const std::string& text, float width, float scale) {
    if (TextWidth(text, scale) <= width)
        return text;
    std::string out = text;
    while (!out.empty() && TextWidth(out + "...", scale) > width)
        out.pop_back();
    return out + "...";
}

static void Icon(FileChooseContext* ctx, const char* tex, int x, int y, int size, bool have) {
    // The display list keeps a pointer to the texture name until the frame is drawn, so the sprite must outlive
    // this call (a sprite on the stack drew noise).
    static std::unordered_map<std::string, Sprite> sSprites;
    auto it = sSprites.find(tex);
    if (it == sSprites.end()) {
        Sprite fresh = {};
        std::snprintf(fresh.tex, sizeof(fresh.tex), "%s", tex);
        fresh.width = 24;
        fresh.height = 24;
        fresh.im_fmt = G_IM_FMT_RGBA;
        fresh.im_siz = G_IM_SIZ_32b;
        it = sSprites.emplace(tex, fresh).first;
    }
    Sprite& sprite = it->second;
    Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
    OPEN_DISPS(ctx->state.gfxCtx);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetTextureLUT(POLY_OPA_DISP++, G_TT_NONE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
    if (have)
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, A(255));
    else
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 40, 40, 60, A(160));
    CLOSE_DISPS(ctx->state.gfxCtx);
    SpriteLoad(ctx, &sprite);
    SpriteDraw(ctx, &sprite, x, y, size, size);
}

// The spiritual stones and medallions of a save file, and its hearts.
static void ProgressIcons(FileChooseContext* ctx, int file, int x, int y) {
    if (file < 0 || file > SaveManager::CoopFileNum)
        return;
    const SaveFileMetaInfo* meta = Save_GetSaveMetaInfo(file);
    if (meta == nullptr || !meta->valid)
        return;
    static const char* const kStones[] = { dgQuestIconKokiriEmeraldTex, dgQuestIconGoronRubyTex,
                                           dgQuestIconZoraSapphireTex };
    static const char* const kMedallions[] = { dgQuestIconMedallionForestTex, dgQuestIconMedallionFireTex,
                                               dgQuestIconMedallionWaterTex,  dgQuestIconMedallionSpiritTex,
                                               dgQuestIconMedallionShadowTex, dgQuestIconMedallionLightTex };
    for (int i = 0; i < 3; i++)
        Icon(ctx, kStones[i], x + i * 11, y, 11, (meta->questItems >> (QUEST_KOKIRI_EMERALD + i)) & 1);
    for (int i = 0; i < 6; i++)
        Icon(ctx, kMedallions[i], x + 36 + i * 11, y, 11, (meta->questItems >> (QUEST_MEDALLION_FOREST + i)) & 1);
}

static int HeartsOf(int file) {
    if (file < 0 || file > SaveManager::CoopFileNum)
        return 0;
    const SaveFileMetaInfo* meta = Save_GetSaveMetaInfo(file);
    return (meta != nullptr && meta->valid) ? meta->healthCapacity / 16 : 0;
}

// The card at the bottom left: what this page is about.
static void DrawCard(FileChooseContext* ctx, const std::vector<Row>& rows, const std::string& hint) {
    CoopLobbyWindow* lobby = Lobby();
    std::string title, line;
    int file = -1;
    bool hintIsProblem = false;
    if (sScreen.page == Page::HostPick && sScreen.cursor < (int)rows.size() &&
        rows[sScreen.cursor].id >= ROW_PLAYTHROUGH && rows[sScreen.cursor].id < ROW_NEW) {
        const CoopPlaythrough& p = lobby->Playthroughs().hosted[rows[sScreen.cursor].id - ROW_PLAYTHROUGH];
        title = p.name;
        line = p.lastPlayed > 0 ? "Played " + FormatDate(p.lastPlayed) : "Not played yet";
        file = p.lastFile;
    } else if (sScreen.page == Page::Session && lobby->IsHosting() && lobby->ActivePlaythrough() != nullptr) {
        title = lobby->ActivePlaythrough()->name;
        file = lobby->PrepareCoopFile() ? SaveManager::CoopFileNum : -1;
        // one line: the file's progress icons take the second
        line = file >= 0 ? "Your co-op save file:" : hint;
    } else if (sScreen.page == Page::Session && !lobby->ConnectedTo().empty()) {
        title = lobby->ConnectedTo() + "'s game";
        file = lobby->PrepareCoopFile() ? SaveManager::CoopFileNum : -1;
        line = file >= 0 ? "Your co-op save file:" : hint;
    } else {
        title = sScreen.page == Page::HostPick  ? "New playthrough"
                : sScreen.page == Page::Session ? "Join a friend"
                                                : "Story Co-op";
        line = hint;
        hintIsProblem = sScreen.page == Page::Home && !CoopLobbyWindow::SteamProblem().empty();
    }
    if (file >= 0 && file <= 2 && Save_GetSaveMetaInfo(file)->valid && sScreen.page == Page::HostPick)
        line += ", File " + std::to_string(file + 1);

    Panel(ctx, kLeftX, kCardY, kLeftW, kCardH);
    Rect(ctx, kLeftX + 1, kCardY + 1, kLeftW - 2, 15, 60, 100, 200, 90);
    TextAlpha(ctx, Fit(title, kLeftW - 12, 0.75f), kLeftX + 6, kCardY + 3, 255, 255, 255, 0.75f);
    int y = kCardY + 20;
    const bool icons = file >= 0 && file <= SaveManager::CoopFileNum && Save_GetSaveMetaInfo(file)->valid;
    const int maxLines = icons ? 1 : 3;
    int n = 0;
    for (const auto& l : Wrap(line, kLeftW - 12, 0.62f)) {
        if (n++ >= maxLines)
            break;
        TextAlpha(ctx, l, kLeftX + 6, y, hintIsProblem ? 255 : 200, hintIsProblem ? 130 : 210,
                  hintIsProblem ? 130 : 230, 0.62f);
        y += 11;
    }
    if (icons) {
        ProgressIcons(ctx, file, kLeftX + 6, kCardY + kCardH - 16);
        const int hearts = HeartsOf(file);
        if (hearts > 0) {
            Icon(ctx, dgQuestIconHeartContainerTex, kLeftX + kLeftW - 30, kCardY + kCardH - 16, 11, true);
            TextAlpha(ctx, std::to_string(hearts), kLeftX + kLeftW - 18, kCardY + kCardH - 15, 255, 255, 255, 0.62f);
        }
    }
}

// The party on the right: you first, then everybody in the party.
static void DrawParty(FileChooseContext* ctx) {
    CoopLobbyWindow* lobby = Lobby();
    Panel(ctx, kRightX, kTopY, kRightW, kCardY + kCardH - kTopY);
    Rect(ctx, kRightX + 1, kTopY + 1, kRightW - 2, 15, 60, 100, 200, 90);
    TextAlpha(ctx, "Party", kRightX + 6, kTopY + 3, 255, 255, 255, 0.75f);

    const auto status = lobby->SessionStatus();
    // picking a friend to join is not a party yet
    const bool inSession = status != CoopLobbyWindow::Status::Idle && status != CoopLobbyWindow::Status::Failed &&
                           status != CoopLobbyWindow::Status::Picking;
    std::vector<std::string> others = lobby->PartyNames();
    if (inSession) {
        char count[16];
        std::snprintf(count, sizeof(count), "%d", (int)others.size() + 1);
        TextAlpha(ctx, count, kRightX + kRightW - 8 - (int)TextWidth(count, 0.75f), kTopY + 3, 255, 255, 120, 0.75f);
    }

    int y = kTopY + 22;
    const std::string me = CVarGetString("gZeldaOnline.Nickname", "Player");
    // "You" tag
    Rect(ctx, kRightX + 5, y - 1, 22, 11, 240, 200, 60, 230);
    TextAlpha(ctx, "", 0, 0, 0, 0, 0, 0.6f); // sets the text combiner
    Interface_DrawTextLine(ctx->state.gfxCtx, (char*)"You", kRightX + 8, y, 40, 30, 0, sScreen.alpha, 0.6f, false);
    TextAlpha(ctx, Fit(me, kRightW - 40, 0.7f), kRightX + 31, y, 255, 230, 120, 0.7f);
    if (inSession && lobby->IsHosting())
        TextAlpha(ctx, "Host", kRightX + kRightW - 6 - (int)TextWidth("Host", 0.55f), y + 10, 150, 200, 255, 0.55f);
    y += 22;

    if (!inSession) {
        for (const auto& l : Wrap("Host a game or join a friend to fill the party.", kRightW - 12, 0.6f)) {
            TextAlpha(ctx, l, kRightX + 6, y, 170, 180, 200, 0.6f);
            y += 11;
        }
        return;
    }
    for (const auto& name : others) {
        if (y > kCardY + kCardH - 14)
            break;
        Rect(ctx, kRightX + 5, y - 2, kRightW - 10, 14, 100, 150, 255, 50);
        TextAlpha(ctx, Fit(name, kRightW - 20, 0.7f), kRightX + 10, y, 255, 255, 255, 0.7f);
        y += 17;
    }
    if (others.empty()) {
        const char* waiting = lobby->IsHosting() ? "Waiting for friends..." : "Joining...";
        TextAlpha(ctx, waiting, kRightX + 10, y, 170, 180, 200, 0.62f);
    }
}

static void DrawRowsLeft(FileChooseContext* ctx, const std::vector<Row>& rows) {
    const int first = sScreen.offset;
    const int last = std::min((int)rows.size(), first + RowsOnScreen());
    int y = kTopY;
    for (int i = first; i < last; i++) {
        const Row& row = rows[i];
        const bool selected = i == sScreen.cursor;
        Button(ctx, kLeftX, y, kLeftW, selected, row.enabled);
        const u8 tb = selected ? 80 : 255;
        TextAlpha(ctx, Fit(row.label, kLeftW - 16, 0.75f), kLeftX + 10, y + 3, row.enabled ? 255 : 150,
                  row.enabled ? 255 : 150, row.enabled ? tb : 150, 0.75f);
        if (!row.value.empty()) {
            const int vx = kLeftX + kLeftW - 14 - (int)TextWidth(row.value, 0.7f);
            TextAlpha(ctx, row.value, vx, y + 3, 255, 255, tb, 0.7f);
            if (selected) {
                Arrow(ctx, vx - 7, y + 4, -1.0f);
                Arrow(ctx, kLeftX + kLeftW - 9, y + 4, 1.0f);
            }
        }
        if (selected)
            Arrow(ctx, kLeftX - 6, y + 4, 1.0f);
        y += kButtonStep;
    }
    if (first > 0 || last < (int)rows.size()) {
        OPEN_DISPS(ctx->state.gfxCtx);
        Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, sScreen.alpha);
        const int ax = kLeftX + kLeftW / 2 - 4;
        if (first > 0) {
            const int ay = kTopY - 9 - sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowUpTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, ax << 2, ay << 2, (ax + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE, 0,
                                    0, (1 << 11), (1 << 11));
        }
        if (last < (int)rows.size()) {
            const int ay = y - 3 + sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowDownTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, ax << 2, ay << 2, (ax + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE, 0,
                                    0, (1 << 11), (1 << 11));
        }
        CLOSE_DISPS(ctx->state.gfxCtx);
    }
}

// The story settings: one wide panel, label on the left, value with arrows on the right.
static void DrawSettings(FileChooseContext* ctx, const std::vector<Row>& rows, const std::string& hint) {
    CoopLobbyWindow* lobby = Lobby();
    auto& list = lobby->Playthroughs().hosted;
    const int shown = std::min((int)rows.size(), RowsOnScreen());
    const int h = 22 + shown * kWideStep + 30;
    Panel(ctx, kWideX, kTopY, kWideW, h);
    Rect(ctx, kWideX + 1, kTopY + 1, kWideW - 2, 15, 60, 100, 200, 90);
    std::string name = (sScreen.settingsIndex >= 0 && sScreen.settingsIndex < (int)list.size())
                           ? list[sScreen.settingsIndex].name
                           : "";
    TextAlpha(ctx, Fit(name, kWideW - 52, 0.75f), kWideX + 6, kTopY + 3, 255, 255, 255, 0.75f);
    int y = kTopY + 22;
    const int first = sScreen.offset;
    const int last = std::min((int)rows.size(), first + RowsOnScreen());
    for (int i = first; i < last; i++) {
        const Row& row = rows[i];
        const bool selected = i == sScreen.cursor;
        if (selected)
            Rect(ctx, kWideX + 4, y - 2, kWideW - 8, 14, 100, 150, 255, 70);
        const bool action = row.value.empty();
        const u8 tb = selected ? 80 : 255;
        TextAlpha(ctx, row.label, kWideX + 16, y, 255, 255, action ? (selected ? 80 : 200) : tb, 0.75f);
        if (selected)
            Arrow(ctx, kWideX + 6, y + 4, 1.0f);
        if (!action) {
            const float w = TextWidth(row.value, 0.75f);
            const int vx = kWideX + kWideW - 22 - (int)w;
            TextAlpha(ctx, row.value, vx, y, 255, 255, tb, 0.75f);
            if (selected) {
                Arrow(ctx, vx - 7, y + 4, -1.0f);
                Arrow(ctx, kWideX + kWideW - 16, y + 4, 1.0f);
            }
        }
        y += kWideStep;
    }
    // more rows above / below, and where the cursor is
    if (first > 0 || last < (int)rows.size()) {
        OPEN_DISPS(ctx->state.gfxCtx);
        Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, sScreen.alpha);
        const int ax = kWideX + kWideW / 2 - 4;
        if (first > 0) {
            const int ay = kTopY + 13 - sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowUpTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, ax << 2, ay << 2, (ax + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE, 0,
                                    0, (1 << 11), (1 << 11));
        }
        if (last < (int)rows.size()) {
            const int ay = y - 4 + sScreen.arrowAnim / 10;
            gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowDownTex, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(POLY_OPA_DISP++, ax << 2, ay << 2, (ax + 8) << 2, (ay + 8) << 2, G_TX_RENDERTILE, 0,
                                    0, (1 << 11), (1 << 11));
        }
        CLOSE_DISPS(ctx->state.gfxCtx);
        char pos[16];
        std::snprintf(pos, sizeof(pos), "%d/%d", sScreen.cursor + 1, (int)rows.size());
        TextAlpha(ctx, pos, kWideX + kWideW - 8 - (int)TextWidth(pos, 0.6f), kTopY + 4, 170, 190, 230, 0.6f);
    }
    y += 6;
    for (const auto& l : Wrap(hint, kWideW - 12, 0.62f)) {
        TextAlpha(ctx, l, kWideX + 6, y, 170, 180, 200, 0.62f);
        y += 11;
    }
}

// The name page: the name with a cursor, the letter grid, and Space / Delete / OK.
static void DrawNamePage(FileChooseContext* ctx, bool modern) {
    constexpr int cell = 12;
    constexpr int gridW = kNameCols * cell;
    const int gx = 160 - gridW / 2;
    const int top = modern ? 56 : 76;
    if (modern) {
        Panel(ctx, gx - 14, top - 6, gridW + 28, 150);
        Rect(ctx, gx - 13, top - 5, gridW + 26, 15, 60, 100, 200, 90);
        TextAlpha(ctx, "The name your teammates see", gx - 7, top - 3, 255, 255, 255, 0.65f);
    }

    // the name being typed
    std::string shown = sScreen.nameEdit;
    const bool blink = (sScreen.arrowAnim / 8) % 2 == 0;
    if ((int)shown.size() < kNameMax)
        shown += blink ? "_" : " ";
    const float nameScale = 0.85f;
    Rect(ctx, gx, top + (modern ? 14 : 0), gridW, 15, 0, 0, 0, 110);
    TextAlpha(ctx, shown, 160 - (int)(TextWidth(sScreen.nameEdit, nameScale) / 2.0f), top + (modern ? 15 : 1), 255,
              255, 255, nameScale);
    char count[16];
    std::snprintf(count, sizeof(count), "%d/%d", (int)sScreen.nameEdit.size(), kNameMax);
    TextAlpha(ctx, count, gx + gridW - (int)TextWidth(count, 0.55f) - 2, top + (modern ? 18 : 4), 150, 160, 190, 0.55f);

    // the grid
    const int gy = top + (modern ? 36 : 22);
    for (int r = 0; r < kNameGridRows; r++) {
        for (int c = 0; c < kNameCols; c++) {
            const bool selected = r == sScreen.nameRow && c == sScreen.nameCol;
            const int x = gx + c * cell;
            const int y = gy + r * cell;
            if (selected)
                Rect(ctx, x - 1, y - 1, cell, cell, 100, 150, 255, 160);
            const std::string ch(1, kNameGrid[r][c]);
            TextAlpha(ctx, ch, x + (cell - (int)TextWidth(ch, 0.75f)) / 2 - 1, y, 255, 255, selected ? 80 : 255,
                      0.75f);
        }
    }
    // Space / Delete / OK
    static const char* const kActions[] = { "Space", "Delete", "OK" };
    const int ay = gy + kNameGridRows * cell + 4;
    const int aw = gridW / 3;
    for (int i = 0; i < 3; i++) {
        const bool selected = sScreen.nameRow == kNameActionRow && sScreen.nameCol == i;
        const int x = gx + i * aw;
        Rect(ctx, x + 2, ay - 1, aw - 4, 13, selected ? 100 : 60, selected ? 150 : 90, 255, selected ? 170 : 90);
        TextAlpha(ctx, kActions[i], x + (aw - (int)TextWidth(kActions[i], 0.7f)) / 2, ay, 255, 255,
                  selected ? 80 : 255, 0.7f);
    }
    const std::string help = "A: type   B: delete   START: save";
    TextAlpha(ctx, help, 160 - (int)(TextWidth(help, 0.6f) / 2.0f), ay + 18, 170, 180, 200, 0.6f);
    const std::string help2 = "Shown over Link and in the party list.";
    TextAlpha(ctx, help2, 160 - (int)(TextWidth(help2, 0.6f) / 2.0f), ay + 29, 170, 180, 200, 0.6f);
}

static void DrawModern(FileChooseContext* ctx) {
    CoopLobbyWindow* lobby = Lobby();

    std::vector<Row> rows = BuildRows();
    std::vector<std::string> above;
    std::string below;
    Info(above, below, rows);

    // header
    TextAlpha(ctx, "Co-op", 20, 14, 255, 255, 255, 1.3f);
    const float titleW = TextWidth("Co-op", 1.3f);
    std::string page = Title();
    if (page != "Co-op")
        TextAlpha(ctx, Fit(page, 300 - 30 - titleW, 0.75f), 20 + (int)titleW + 10, 20, 150, 190, 255, 0.75f);
    Rect(ctx, 16, 34, 288, 1, 100, 150, 255, 160);
    if (!above.empty()) {
        const bool error = lobby->SessionStatus() == CoopLobbyWindow::Status::Failed;
        const auto lines = Wrap(above[0], 280, 0.65f);
        if (!lines.empty()) {
            const std::string& l = lines[0];
            TextAlpha(ctx, l, 160 - (int)(TextWidth(l, 0.65f) / 2), 37, error ? 255 : 220, error ? 130 : 230,
                      error ? 130 : 255, 0.65f);
        }
    }

    if (sScreen.page == Page::Settings) {
        DrawSettings(ctx, rows, below);
        return;
    }
    if (sScreen.page == Page::Name) {
        DrawNamePage(ctx, true);
        return;
    }
    DrawRowsLeft(ctx, rows);
    DrawCard(ctx, rows, below);
    DrawParty(ctx);
}

extern "C" int CoopLobby_ModernLayout(void) {
    return CVarGetInteger(kModernCVar, 0) != 0;
}

extern "C" void FileChoose_DrawCoopMenu(FileChooseContext* ctx) {
    if (Lobby() == nullptr)
        return;
    if (CoopLobby_ModernLayout())
        DrawModern(ctx);
    else
        DrawClassic(ctx);
}
