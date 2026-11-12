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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
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

extern "C" {
void FileChoose_UpdateStickDirectionPromptAnim(GameState* thisx);
void FileChoose_DrawTextRec(GraphicsContext* gfxCtx, s32 r, s32 g, s32 b, s32 a, f32 x, f32 y, f32 z, s32 s, s32 t,
                            f32 dx, f32 dy);
}

using namespace ZeldaOnline;

namespace {

enum class Page { Home, HostPick, Settings, Session };

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
    ROW_START_HOSTING,
    ROW_DONE,
    ROW_CHOOSE_FILE,
    ROW_SETTINGS,
    ROW_STOP,
    ROW_FRIEND = 2000, // + index
};

constexpr int kRowsOnScreen = 6;
constexpr int kListX = 72;
constexpr int kListY = 82;
constexpr int kRowStep = 16;
constexpr float kTextScale = 0.8f;

struct CoopScreen {
    Page page = Page::Home;
    int cursor = 0;
    int offset = 0;
    int settingsIndex = -1;    // the playthrough whose settings are shown
    bool settingsLive = false; // opened while hosting: changes apply right away, no "Start hosting" row
    int16_t alpha = 0;
    int arrowAnim = 0;
};
CoopScreen sScreen;

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
            if (p.lastFile >= 0)
                below += ", File " + std::to_string(p.lastFile + 1);
        } else {
            below = "A new team save, with its own story settings.";
        }
        return;
    }
    if (sScreen.page == Page::Settings) {
        below = sScreen.settingsLive ? "Changes apply to everybody right away." : "Left / right changes a setting.";
        return;
    }
    // session
    const int lastFile = lobby->LastFileForSession();
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
            below = lastFile >= 0 ? "Last time you used File " + std::to_string(lastFile + 1) + "."
                                  : "Any file works: the team progress is added.";
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
            rows.push_back({ "Host a game", "", steam, ROW_HOST });
            rows.push_back({ "Join a friend", "", steam, ROW_JOIN });
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
                if (status == CoopLobbyWindow::Status::Ready)
                    rows.push_back({ "Choose a save file", "", true, ROW_CHOOSE_FILE });
                if (host && lobby->ActivePlaythrough() != nullptr)
                    rows.push_back({ "Story settings", "", true, ROW_SETTINGS });
                rows.push_back({ host ? "Stop hosting" : "Leave", "", true, ROW_STOP });
            }
            break;
        }
    }
    return rows;
}

void GoTo(Page page, int cursor = 0) {
    sScreen.page = page;
    sScreen.cursor = cursor;
    sScreen.offset = std::max(0, cursor - kRowsOnScreen + 1);
}

void ExitToMain(FileChooseContext* ctx) {
    ctx->configMode = CM_COOP_TO_MAIN;
}

void ChangeOption(int id, int dir) {
    CoopLobbyWindow* lobby = Lobby();
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
        default:
            return;
    }
    if ((s.extraEnemies || s.healthMultiplier) && !s.privateDungeons)
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
        GoTo(Page::Settings, 5); // on "Start hosting": A A hosts it as it was
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
            if (sScreen.page == Page::Session) {
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

} // namespace

// ---------------------------------------------------------------------------------------------- entry points

extern "C" int CoopLobby_FileSelectCheckOpen(FileChooseContext* ctx) {
    Input* input = &ctx->state.input[0];
    if (Lobby() == nullptr || !CHECK_BTN_ALL(input->press.button, BTN_R))
        return 0;
    PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    ctx->prevConfigMode = ctx->configMode;
    ctx->configMode = CM_ROTATE_TO_COOP_MENU;
    ctx->warningLabel = FS_WARNING_NONE;
    return 1;
}

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
    if (sScreen.cursor >= sScreen.offset + kRowsOnScreen)
        sScreen.offset = sScreen.cursor - kRowsOnScreen + 1;

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

extern "C" void FileChoose_DrawCoopMenu(FileChooseContext* ctx) {
    if (Lobby() == nullptr)
        return;
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
    const int visible = std::max(1, kRowsOnScreen - (int)((y - kListY + kRowStep - 1) / kRowStep));
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
        int by = std::max(y + 4, kListY + kRowsOnScreen * kRowStep - 6);
        for (const auto& line : Wrap(below, 176.0f, 0.7f)) {
            DrawText(ctx, line, kListX, by, problem ? 255 : 170, problem ? 120 : 170, problem ? 120 : 170, 0.7f);
            by += 12;
        }
    }

    CLOSE_DISPS(ctx->state.gfxCtx);
}
