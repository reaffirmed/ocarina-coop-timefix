#include "ZeldaOnlinePolish.hpp"
#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "soh/OTRGlobals.h"
#include "soh/frame_interpolation.h"
#include "soh/util.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/Notification/Notification.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h"
#include "soh/Enhancements/randomizer/SeedContext.h"
#include <ship/Context.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <vector>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
float OTRGetRectDimensionFromLeftEdge(float v);
void* Ship_GetCharFontTexture(u8 character);
f32 Ship_GetCharFontWidth(u8 character);
}

// Drawing helpers: outside the namespace, because OPEN_DISPS declares the frame interpolation functions where it is
// used x is in screen pixels already (after the widescreen adjustment)
static void PolishDrawText(GraphicsContext* gfxCtx, const std::string& text, s16 x, s16 y, Color_RGBA8 color, f32 scale,
                           Color_RGBA8 (*glyphColour)(char)) {
    if (text.empty())
        return;

    Gfx_SetupDL_39Overlay(gfxCtx);
    OPEN_DISPS(gfxCtx);
    gDPSetAlphaCompare(OVERLAY_DISP++, G_AC_NONE);
    gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);

    s16 dsdx = (s16)((1 << 10) / scale);
    s16 charW = (s16)(FONT_CHAR_TEX_WIDTH * scale);
    s16 charH = (s16)(FONT_CHAR_TEX_HEIGHT * scale);
    float penX = x;
    bool glyph = true; // forces the first colour to be set
    for (char c : text) {
        bool isGlyph = (u8)c >= 0x9F;
        if (isGlyph || glyph) {
            Color_RGBA8 col = isGlyph ? glyphColour(c) : color;
            gDPPipeSync(OVERLAY_DISP++);
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, col.r, col.g, col.b, std::min(col.a, color.a));
            glyph = isGlyph;
        }
        uintptr_t texture = (uintptr_t)Ship_GetCharFontTexture((u8)c);
        gDPLoadTextureBlock_4b(OVERLAY_DISP++, texture, G_IM_FMT_I, FONT_CHAR_TEX_WIDTH, FONT_CHAR_TEX_HEIGHT, 0,
                               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
                               G_TX_NOLOD, G_TX_NOLOD);
        s16 px = (s16)penX;
        gSPWideTextureRectangle(OVERLAY_DISP++, px << 2, y << 2, (px + charW) << 2, (y + charH) << 2, G_TX_RENDERTILE,
                                0, 0, dsdx, dsdx);
        penX += Ship_GetCharFontWidth((u8)c) * scale;
    }
    CLOSE_DISPS(gfxCtx);
}

static void PolishFillRect(GraphicsContext* gfxCtx, s16 x, s16 y, s16 w, s16 h, Color_RGBA8 c) {
    if (w <= 0 || h <= 0)
        return;
    OPEN_DISPS(gfxCtx);
    gDPPipeSync(OVERLAY_DISP++);
    gDPSetCycleType(OVERLAY_DISP++, G_CYC_1CYCLE);
    gDPSetRenderMode(OVERLAY_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, c.r, c.g, c.b, c.a);
    gDPFillWideRectangle(OVERLAY_DISP++, x, y, x + w, y + h);
    CLOSE_DISPS(gfxCtx);
}

namespace ZeldaOnline {

// The game's font has the controller buttons at these characters (the same ones its text boxes use)
static constexpr char GLYPH_A = (char)0x9F;
static constexpr char GLYPH_B = (char)0xA0;
static constexpr char GLYPH_L = (char)0xA2;

// A text box id the game never shows; while a co-op message is up its text comes from here (see OnOpenText below)
static constexpr uint16_t POLISH_TEXT_ID = TEXT_UNUSED_014F;

using Clock = std::chrono::steady_clock;

namespace {

struct QueuedBox {
    std::string text;
};

struct State {
    bool hooks = false;

    // connection notices (bottom right, with the other notifications)
    enum class Conn { Hidden, Reconnecting } conn = Conn::Hidden;
    Clock::time_point connSince{};
    uint32_t lostNote = 0;
    uint32_t hostNote = 0;

    // message panel (outside of gameplay)
    std::string panelTitle;
    std::string panelBody;
    Clock::time_point panelUntil{};

    // the game's text box (gameplay)
    std::deque<QueuedBox> boxes;
    std::string boxText; // what the text id shows right now
    bool boxShowing = false;
    bool boxFroze = false;
    int boxOpenedFrames = 0;

    // items another member's chest already announced
    std::unordered_map<int, Clock::time_point> announced;

    // sounds and prompts, last frame
    int lastCsSeconds = -1;
    int csPromptMax = 0;
    int lastHoldVoted = -1;
    int lastWarpVoted = -1;
    bool lastAllReady = false;

    // game check
    std::string hostGame;
    std::string pushedGame;
    std::string warnedPair;
    int pushCooldown = 0;

    bool wasStoryActive = false;
} S;

bool SoundsOn() {
    return CVarGetInteger("gZeldaOnline.CoopSounds", 1) != 0;
}

void Cue(u16 sfx) {
    if (SoundsOn())
        Sfx_PlaySfxCentered(sfx);
}

ZeldaOnlineClient* Client() {
    return ZeldaOnlineClient::Instance;
}

// Drawing ----------------------------------------------------------------------------------------------------------

float TextWidth(const std::string& text, float scale) {
    float w = 0.0f;
    for (char c : text)
        w += Ship_GetCharFontWidth((u8)c) * scale;
    return w;
}

Color_RGBA8 GlyphColour(char c) {
    switch (c) {
        case GLYPH_A:
            return { 90, 140, 255, 255 }; // A is blue
        case GLYPH_B:
            return { 80, 220, 80, 255 }; // B is green
        default:
            return { 200, 200, 200, 255 }; // L, R, Z are grey
    }
}

// Keeps the characters the font has (space to the D-pad icon)
std::string Printable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        u8 u = (u8)c;
        if (u == '\n' || (u >= ' ' && u <= 0xAB))
            out += c;
    }
    return out;
}

void DrawText(GraphicsContext* gfxCtx, const std::string& raw, s16 x, s16 y, Color_RGBA8 color, f32 scale) {
    PolishDrawText(gfxCtx, Printable(raw), x, y, color, scale, GlyphColour);
}

void FillRect(GraphicsContext* gfxCtx, s16 x, s16 y, s16 w, s16 h, Color_RGBA8 c) {
    PolishFillRect(gfxCtx, x, y, w, h, c);
}

// The Co-op Lobby's panel: dark glass with a thin blue edge
void Panel(GraphicsContext* gfxCtx, s16 x, s16 y, s16 w, s16 h, u8 alpha, Color_RGBA8 edge = { 100, 150, 255, 200 }) {
    auto a = [alpha](int v) { return (u8)(v * alpha / 255); };
    FillRect(gfxCtx, x, y, w, h, { 0, 10, 40, a(170) });
    FillRect(gfxCtx, x, y, w, 1, { edge.r, edge.g, edge.b, a(edge.a) });
    FillRect(gfxCtx, x, y + h - 1, w, 1, { edge.r, edge.g, edge.b, a(edge.a * 6 / 10) });
    FillRect(gfxCtx, x, y, 1, h, { edge.r, edge.g, edge.b, a(edge.a * 6 / 10) });
    FillRect(gfxCtx, x + w - 1, y, 1, h, { edge.r, edge.g, edge.b, a(edge.a * 6 / 10) });
}

std::vector<std::string> Wrap(const std::string& text, float scale, float maxWidth) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(candidate, scale) > maxWidth) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') {
            flushWord();
        } else if (c == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else {
            word += c;
        }
    }
    flushWord();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); i++) {
        if (i != 0)
            out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

using Prompt = CoopPolish::PromptText;

// One short line at the bottom centre: "Time travel  1/2 ready   B Cancel", narrow enough to stay clear of the rupee
// counter at the bottom left and the map at the bottom right
void DrawPrompt(GraphicsContext* gfxCtx, const Prompt& p) {
    const float scale = 0.5f;
    const s16 pad = 6, gap = 8;
    const float titleW = TextWidth(p.title, scale);
    const float countW = p.count.empty() ? 0.0f : TextWidth(p.count, scale) + gap;
    const float hintW = p.hints.empty() ? 0.0f : TextWidth(p.hints, scale) + gap;
    const s16 w = (s16)(titleW + countW + hintW) + pad * 2;
    const s16 h = (s16)(p.bar >= 0.0f ? 19 : 15);
    const s16 x = (s16)(SCREEN_WIDTH / 2 - w / 2);
    const s16 y = (s16)(212 - h);

    Panel(gfxCtx, x, y, w, h, 255);
    s16 cx = x + pad;
    DrawText(gfxCtx, p.title, cx, y + 3, { 255, 230, 110, 255 }, scale);
    cx += (s16)(titleW + gap);
    if (!p.count.empty())
        DrawText(gfxCtx, p.count, cx, y + 3, { 170, 200, 255, 255 }, scale);
    if (!p.hints.empty())
        DrawText(gfxCtx, p.hints, (s16)(x + w - pad - (hintW - gap)), y + 3, { 230, 230, 230, 255 }, scale);
    if (p.bar >= 0.0f) {
        s16 bw = w - pad * 2;
        FillRect(gfxCtx, x + pad, y + h - 5, bw, 2, { 40, 60, 110, 200 });
        FillRect(gfxCtx, x + pad, y + h - 5, (s16)(bw * std::clamp(p.bar, 0.0f, 1.0f)), 2, { 120, 180, 255, 230 });
    }
}

void DrawPanelMessage(GraphicsContext* gfxCtx) {
    if (S.panelTitle.empty())
        return;
    const auto now = Clock::now();
    if (now >= S.panelUntil) {
        S.panelTitle.clear();
        S.panelBody.clear();
        return;
    }
    const float titleScale = 0.6f, bodyScale = 0.5f;
    const s16 w = 240;
    const s16 x = (s16)(SCREEN_WIDTH / 2 - w / 2);
    std::vector<std::string> lines = Wrap(S.panelBody, bodyScale, (float)(w - 16));
    s16 h = 8 + 13 + (s16)lines.size() * 10 + 8;
    s16 y = (s16)(SCREEN_HEIGHT / 2 - h / 2);
    Panel(gfxCtx, x, y, w, h, 240, { 255, 200, 110, 230 });
    DrawText(gfxCtx, S.panelTitle, x + 8, y + 7, { 255, 230, 110, 255 }, titleScale);
    s16 cy = y + 21;
    for (const std::string& line : lines) {
        DrawText(gfxCtx, line, x + 8, cy, { 255, 255, 255, 255 }, bodyScale);
        cy += 10;
    }
}

// Text box -----------------------------------------------------------------------------------------------------------

bool CanOpenTextBox(PlayState* play) {
    if (play == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL || gSaveContext.fileNum == 0xFF)
        return false;
    if (play->msgCtx.msgMode != MSGMODE_NONE || play->transitionMode != TRANS_MODE_OFF ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || play->pauseCtx.state != 0 || Player_InCsMode(play) ||
        play->csCtx.state != CS_STATE_IDLE)
        return false;
    Player* player = GET_PLAYER(play);
    if (player == nullptr || (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_CUTSCENE |
                                                     PLAYER_STATE1_TALKING | PLAYER_STATE1_GETTING_ITEM)))
        return false;
    // not in the air or in the water, where freezing Link would look broken
    if (!(player->actor.bgCheckFlags & 1) || (player->stateFlags1 & PLAYER_STATE1_IN_WATER))
        return false;
    return true;
}

void UpdateTextBox() {
    PlayState* play = gPlayState;
    if (play == nullptr)
        return;
    Player* player = GET_PLAYER(play);

    if (S.boxShowing) {
        S.boxOpenedFrames++;
        bool closed = play->msgCtx.msgMode == MSGMODE_NONE && S.boxOpenedFrames > 2;
        if (closed) {
            S.boxShowing = false;
            if (S.boxFroze && player != nullptr)
                player->stateFlags1 &= ~PLAYER_STATE1_IN_CUTSCENE;
            S.boxFroze = false;
        } else if (player != nullptr) {
            // held like during another member's story dialogue
            player->stateFlags1 |= PLAYER_STATE1_IN_CUTSCENE;
        }
        return;
    }

    if (S.boxes.empty() || !CanOpenTextBox(play))
        return;

    S.boxText = S.boxes.front().text;
    S.boxes.pop_front();
    S.boxShowing = true;
    S.boxOpenedFrames = 0;
    S.boxFroze = !(player->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE);
    player->stateFlags1 |= PLAYER_STATE1_IN_CUTSCENE;
    Message_StartTextbox(play, POLISH_TEXT_ID, nullptr);
}

void QueueBox(const std::string& text) {
    // the same message twice in a row is noise (a reconnect loop that is kicked every time)
    if (!S.boxes.empty() && S.boxes.back().text == text)
        return;
    if (S.boxShowing && S.boxText == text)
        return;
    S.boxes.push_back({ text });
}

// Prompts ------------------------------------------------------------------------------------------------------------

std::string HintPair(char glyph, const std::string& label) {
    return std::string(1, glyph) + " " + label;
}

} // namespace

bool CoopPolish::BuildPrompt(ZeldaOnlineClient* c, PromptText& p) {
    if (c->m_storyHold) {
        p.title = "Time travel";
        p.count = std::to_string(c->m_storyVoted) + "/" + std::to_string(c->m_storyTotal) + " ready";
        p.hints = HintPair(GLYPH_B, "Cancel");
        return true;
    }
    if (c->m_storyWarpHold || c->m_storyWarpVoting) {
        // a ready check (a co-op rule asking everyone before a transition) reads differently from a story warp
        const bool readyCheck =
            (c->m_storyWarpHold && c->m_coopReadyHold) || (!c->m_storyWarpHold && c->m_coopReadyVote);
        p.title = readyCheck ? "Ready check" : "Continue";
        p.count = std::to_string(c->m_storyWarpVoted) + "/" + std::to_string(c->m_storyWarpTotal) + " ready";
        if (c->m_storyWarpHold) {
            if (c->m_storyWarpHeard)
                p.hints = HintPair(GLYPH_L, "Go now");
        } else if (c->m_storyWarpReady) {
            p.hints = HintPair(GLYPH_B, "Cancel");
        } else {
            p.hints = HintPair(GLYPH_L, "Ready");
        }
        return true;
    }
    if (c->m_storyCsHasPending && c->m_storyCsPromptFrames > 0) {
        const int seconds = (c->m_storyCsPromptFrames + 19) / 20;
        p.title = "Story scene in " + std::to_string(seconds);
        p.bar = S.csPromptMax > 0 ? (float)c->m_storyCsPromptFrames / (float)S.csPromptMax : -1.0f;
        p.hints = HintPair(GLYPH_L, "Skip");
        return true;
    }
    return false;
}

void CoopPolish::UpdateCues(ZeldaOnlineClient* c) {
    // cutscene countdown
    if (c->m_storyCsHasPending && c->m_storyCsPromptFrames > 0) {
        int seconds = (c->m_storyCsPromptFrames + 19) / 20;
        if (S.lastCsSeconds < 0) {
            S.csPromptMax = c->m_storyCsPromptFrames;
            Cue(NA_SE_SY_WIN_OPEN);
        } else if (seconds < S.lastCsSeconds) {
            Cue(NA_SE_SY_WARNING_COUNT_N);
        }
        S.lastCsSeconds = seconds;
    } else {
        S.lastCsSeconds = -1;
        S.csPromptMax = 0;
    }

    // ready votes: a ding when somebody says ready, a chime when everyone is
    int holdVoted = c->m_storyHold ? c->m_storyVoted : -1;
    int warpVoted = (c->m_storyWarpHold || c->m_storyWarpVoting) ? c->m_storyWarpVoted : -1;
    bool allReady = (holdVoted >= 0 && c->m_storyTotal > 1 && holdVoted >= c->m_storyTotal) ||
                    (warpVoted >= 0 && c->m_storyWarpTotal > 1 && warpVoted >= c->m_storyWarpTotal);
    if (allReady && !S.lastAllReady) {
        Cue(NA_SE_SY_CORRECT_CHIME);
    } else if ((holdVoted > S.lastHoldVoted && S.lastHoldVoted >= 0) ||
               (warpVoted > S.lastWarpVoted && S.lastWarpVoted >= 0) || (warpVoted >= 0 && S.lastWarpVoted < 0)) {
        Cue(NA_SE_SY_DECIDE);
    }
    S.lastHoldVoted = holdVoted;
    S.lastWarpVoted = warpVoted;
    S.lastAllReady = allReady;
}

// Game check ---------------------------------------------------------------------------------------------------------

void CoopPolish::UpdateGameCheck(ZeldaOnlineClient* c) {
    if (!c->StoryCoopActive() || ZeldaOnlineRoomWindow::Instance == nullptr)
        return;
    std::string mine = CoopPolish::LocalGame();
    if (mine.empty())
        return;

    if (ZeldaOnlineRoomWindow::Instance->IsPartyAdmin()) {
        if (S.pushCooldown > 0) {
            S.pushCooldown--;
            return;
        }
        if (mine != S.pushedGame) {
            S.pushedGame = mine;
            S.hostGame = mine;
            S.pushCooldown = 40;
            c->WritePacket(newPacket(CLIENT_PACKET_UPDATE_PARTY_SETTINGS)
                           << c->BuildPartySettingsJSON(c->m_partySettings));
        }
        return;
    }

    if (S.hostGame.empty() || S.hostGame == mine)
        return;
    std::string pair = S.hostGame + "|" + mine;
    if (pair == S.warnedPair)
        return;
    S.warnedPair = pair;

    std::string host = "The host";
    if (uint32_t admin = ZeldaOnlineRoomWindow::Instance->PartyAdminID())
        host = ZeldaOnlineRoomWindow::Instance->NameOf(admin);
    CoopPolish::ShowMessage("This file doesn't match the party",
                            host + " is playing " + CoopPolish::DescribeGame(S.hostGame) + ", and this file is " +
                                CoopPolish::DescribeGame(mine) +
                                ". Shared progress would mix the two games up. Return to the file select and load a "
                                "matching file.");
}

// Per frame (gameplay) ----------------------------------------------------------------------------------------

void CoopPolish::OnFrame() {
    ZeldaOnlineClient* c = Client();
    UpdateTextBox();
    if (c == nullptr)
        return;

    UpdateCues(c);
    UpdateGameCheck(c);

    bool active = c->StoryCoopActive();
    if (active && !S.wasStoryActive)
        S.warnedPair.clear();
    S.wasStoryActive = active;
}

void CoopPolish::OnDraw(GraphicsContext* gfxCtx) {
    UpdateConnectionNotes();
    ZeldaOnlineClient* c = Client();
    if (c != nullptr && gPlayState != nullptr && c->isConnected) {
        Prompt p;
        if (BuildPrompt(c, p))
            DrawPrompt(gfxCtx, p);
    }
    DrawPanelMessage(gfxCtx);
}

namespace {

// Item names for announcements ---------------------------------------------------------------------------------------

static_assert(ITEM_SONG_STORMS - ITEM_SONG_MINUET == QUEST_SONG_STORMS - QUEST_SONG_MINUET, "song order");
static_assert(ITEM_MEDALLION_LIGHT - ITEM_MEDALLION_FOREST == QUEST_MEDALLION_LIGHT - QUEST_MEDALLION_FOREST,
              "medallion order");
static_assert(ITEM_ZORA_SAPPHIRE - ITEM_KOKIRI_EMERALD == QUEST_ZORA_SAPPHIRE - QUEST_KOKIRI_EMERALD, "stone order");
static_assert(ITEM_BOOTS_HOVER - ITEM_SWORD_KOKIRI == 11, "equipment order");

int QuestBitItem(int bit) {
    if (bit >= QUEST_MEDALLION_FOREST && bit <= QUEST_MEDALLION_LIGHT)
        return ITEM_MEDALLION_FOREST + (bit - QUEST_MEDALLION_FOREST);
    if (bit >= QUEST_SONG_MINUET && bit <= QUEST_SONG_STORMS)
        return ITEM_SONG_MINUET + (bit - QUEST_SONG_MINUET);
    if (bit >= QUEST_KOKIRI_EMERALD && bit <= QUEST_ZORA_SAPPHIRE)
        return ITEM_KOKIRI_EMERALD + (bit - QUEST_KOKIRI_EMERALD);
    if (bit == QUEST_STONE_OF_AGONY)
        return ITEM_STONE_OF_AGONY;
    if (bit == QUEST_GERUDO_CARD)
        return ITEM_GERUDO_CARD;
    return -1;
}

int UpgradeItem(int upg, int level) {
    if (level <= 0)
        return -1;
    switch (upg) {
        case UPG_QUIVER:
            return level <= 3 ? ITEM_QUIVER_30 + level - 1 : -1;
        case UPG_BOMB_BAG:
            return level <= 3 ? ITEM_BOMB_BAG_20 + level - 1 : -1;
        case UPG_STRENGTH:
            return level <= 3 ? ITEM_BRACELET + level - 1 : -1;
        case UPG_SCALE:
            return level <= 2 ? ITEM_SCALE_SILVER + level - 1 : -1;
        case UPG_WALLET:
            return level <= 2 ? ITEM_WALLET_ADULT + level - 1 : -1;
        case UPG_BULLET_BAG:
            return level <= 3 ? ITEM_BULLET_BAG_30 + level - 1 : -1;
        default:
            return -1;
    }
}

std::vector<int> NewItems(const StoryProgress& before, const StoryProgress& after) {
    std::vector<int> out;
    auto add = [&out](int item) {
        if (item >= 0 && item != ITEM_NONE && std::find(out.begin(), out.end(), item) == out.end())
            out.push_back(item);
    };

    // the quest status first: songs, medallions and stones are the big moments
    for (int bit = 0; bit <= QUEST_GERUDO_CARD; bit++) {
        if ((after.questItems & (1u << bit)) && !(before.questItems & (1u << bit)))
            add(QuestBitItem(bit));
    }
    for (int i = 0; i < 24; i++) {
        if (after.items[i] != before.items[i] && after.items[i] != ITEM_NONE) {
            // bottle contents are per player and never announced; a new bottle is
            if (i >= SLOT_BOTTLE_1 && i <= SLOT_BOTTLE_4 && before.items[i] != ITEM_NONE)
                continue;
            add(i >= SLOT_BOTTLE_1 && i <= SLOT_BOTTLE_4 ? (int)ITEM_BOTTLE : (int)after.items[i]);
        }
    }
    for (int type = 0; type < 4; type++) {
        for (int value = 0; value < 3; value++) {
            uint16_t bit = (uint16_t)(1u << (type * 4 + value));
            if ((after.equipment & bit) && !(before.equipment & bit))
                add(ITEM_SWORD_KOKIRI + type * 3 + value);
        }
    }
    for (int upg = UPG_QUIVER; upg <= UPG_BULLET_BAG; upg++) {
        int was = (int)((before.upgrades & STORY_UPGRADE_MASKS[upg]) >> STORY_UPGRADE_SHIFTS[upg]);
        int now = (int)((after.upgrades & STORY_UPGRADE_MASKS[upg]) >> STORY_UPGRADE_SHIFTS[upg]);
        if (now > was)
            add(UpgradeItem(upg, now));
    }
    if (after.isMagicAcquired && !before.isMagicAcquired)
        add(ITEM_MAGIC_SMALL);
    if (after.isDoubleMagicAcquired && !before.isDoubleMagicAcquired)
        add(ITEM_MAGIC_LARGE);
    if (after.isDoubleDefenseAcquired && !before.isDoubleDefenseAcquired)
        add(ITEM_DOUBLE_DEFENSE);
    if (after.heartQuarters > before.heartQuarters) {
        int gained = after.heartQuarters - before.heartQuarters;
        add(gained >= 4 && gained % 4 == 0 ? ITEM_HEART_CONTAINER : ITEM_HEART_PIECE_2);
    }
    for (int i = 0; i < 20; i++) {
        if ((after.dungeonItems[i] & 1) && !(before.dungeonItems[i] & 1))
            add(ITEM_KEY_BOSS);
    }
    return out;
}

} // namespace

// Public -------------------------------------------------------------------------------------------------------

void CoopPolish::EnsureHooks() {
    if (S.hooks)
        return;
    S.hooks = true;
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnDrawOverlay>(
        [](GraphicsContext* gfxCtx) { OnDraw(gfxCtx); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([]() { OnFrame(); });
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnOpenText>(
        POLISH_TEXT_ID, [](uint16_t* textId, bool* loadFromMessageTable) {
            if (!S.boxShowing)
                return;
            CustomMessage msg(S.boxText, TEXTBOX_TYPE_BLACK, TEXTBOX_POS_BOTTOM);
            msg.AutoFormat();
            msg.LoadIntoFont();
            *loadFromMessageTable = false;
        });
}

void CoopPolish::OnConnected(bool reconnected) {
    EnsureHooks();
    const bool wasLost = S.conn == State::Conn::Reconnecting;
    ClearConnectionNotes();
    if (reconnected || wasLost) {
        Notification::Emit({ .prefix = "Back online",
                             .prefixColor = ImVec4(0.45f, 0.9f, 0.5f, 1.0f),
                             .remainingTime = 5.0f,
                             .mute = true });
        Cue(NA_SE_SY_FSEL_DECIDE_S);
    }
}

void CoopPolish::OnDisconnected(bool willReconnect) {
    EnsureHooks();
    if (!willReconnect) {
        ClearConnectionNotes();
        return;
    }
    if (S.conn != State::Conn::Reconnecting) {
        S.conn = State::Conn::Reconnecting;
        S.connSince = Clock::now();
        // stays up until the connection is back
        S.lostNote = Notification::Emit({ .prefix = "Connection lost",
                                          .prefixColor = ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                          .message = "reconnecting...",
                                          .remainingTime = 24.0f * 3600.0f,
                                          .mute = true });
        Cue(NA_SE_SY_ERROR);
    }
}

void CoopPolish::UpdateConnectionNotes() {
    if (S.conn != State::Conn::Reconnecting || S.hostNote != 0)
        return;
    if (std::chrono::duration<float>(Clock::now() - S.connSince).count() >= 45.0f)
        S.hostNote = Notification::Emit(
            { .message = "Is the host still online?", .remainingTime = 24.0f * 3600.0f, .mute = true });
}

void CoopPolish::ClearConnectionNotes() {
    if (S.lostNote != 0)
        Notification::Dismiss(S.lostNote);
    if (S.hostNote != 0)
        Notification::Dismiss(S.hostNote);
    S.lostNote = S.hostNote = 0;
    S.conn = State::Conn::Hidden;
}

void CoopPolish::OnConnectFailed() {
    // the "Connection lost" notice stays up while the client keeps retrying
}

void CoopPolish::ShowMessage(const std::string& title, const std::string& body) {
    EnsureHooks();
    printf("Co-op: %s: %s\n", title.c_str(), body.c_str());
    if (gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL && gSaveContext.fileNum != 0xFF) {
        QueueBox("%y" + title + "%w^" + body);
        return;
    }
    S.panelTitle = title;
    S.panelBody = body;
    // long enough to read: 8 seconds plus a little per word
    size_t words = std::count(body.begin(), body.end(), ' ') + 1;
    S.panelUntil = Clock::now() + std::chrono::milliseconds(8000 + (int)words * 250);
}

void CoopPolish::NoteAnnounced(int itemId) {
    S.announced[itemId] = Clock::now();
}

void CoopPolish::AnnounceProgress(const StoryProgress& before, const StoryProgress& after) {
    EnsureHooks();
    std::vector<int> items = NewItems(before, after);

    // drop what a shared chest already announced in the last half minute
    const auto now = Clock::now();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](int item) {
                                   auto it = S.announced.find(item);
                                   return it != S.announced.end() && now - it->second < std::chrono::seconds(30);
                               }),
                items.end());
    for (int item : items)
        S.announced[item] = now;

    if (items.empty())
        return;

    const std::string who = PartnerLabel();
    const size_t shown = items.size() > 3 ? 2 : items.size();
    for (size_t i = 0; i < shown; i++) {
        Notification::Emit({
            .itemIcon = GetTextureForItemId(items[i]),
            .prefix = who,
            .message = "found",
            .suffix = SohUtils::GetItemName(items[i]),
            .mute = true,
        });
    }
    if (items.size() > shown) {
        Notification::Emit({
            .prefix = who,
            .message = "found " + std::to_string(items.size() - shown) + " more items",
            .mute = true,
        });
    }
    Cue(NA_SE_SY_GET_ITEM);
}

void CoopPolish::OnMemberJoined(uint32_t networkID) {
    EnsureHooks();
    ZeldaOnlineClient* c = Client();
    if (c == nullptr || ZeldaOnlineRoomWindow::Instance == nullptr || networkID == (uint32_t)c->m_myNetworkID)
        return;
    std::string name = ZeldaOnlineRoomWindow::Instance->NameOf(networkID);
    if (name.empty())
        return;
    Notification::Emit({ .prefix = name, .message = "joined the party", .mute = true });
    Cue(NA_SE_SY_FSEL_DECIDE_L);
}

void CoopPolish::OnMemberLeft(uint32_t networkID) {
    EnsureHooks();
    ZeldaOnlineClient* c = Client();
    if (c == nullptr || ZeldaOnlineRoomWindow::Instance == nullptr || networkID == (uint32_t)c->m_myNetworkID)
        return;
    std::string name = ZeldaOnlineRoomWindow::Instance->NameOf(networkID);
    if (name.empty())
        return;
    Notification::Emit({ .prefix = name, .message = "left the party", .mute = true });
    Cue(NA_SE_SY_WIN_CLOSE);
}

std::string CoopPolish::LocalGame() {
    if (gSaveContext.fileNum == 0xFF || gSaveContext.gameMode != GAMEMODE_NORMAL || gPlayState == nullptr)
        return "";
    switch (gSaveContext.ship.quest.id) {
        case QUEST_NORMAL:
            return "vanilla";
        case QUEST_MASTER:
            return "mq";
        case QUEST_RANDOMIZER: {
            std::string hash;
            if (OTRGlobals::Instance != nullptr && OTRGlobals::Instance->gRandoContext != nullptr)
                hash = OTRGlobals::Instance->gRandoContext->GetHash();
            return "rando:" + hash;
        }
        case QUEST_BOSSRUSH:
            return "bossrush";
        default:
            return "other";
    }
}

std::string CoopPolish::HostGame() {
    return S.hostGame;
}

void CoopPolish::SetHostGame(const std::string& game) {
    if (game.size() > 64)
        return;
    S.hostGame = game;
}

std::string CoopPolish::DescribeGame(const std::string& game) {
    if (game == "vanilla")
        return "a regular save";
    if (game == "mq")
        return "a Master Quest save";
    if (game == "bossrush")
        return "Boss Rush";
    if (game.rfind("rando:", 0) == 0)
        return game.size() > 6 ? "a randomizer save (seed " + game.substr(6) + ")" : "a randomizer save";
    return "a different kind of save";
}

std::string CoopPolish::PartnerLabel() {
    ZeldaOnlineClient* c = Client();
    if (c == nullptr || ZeldaOnlineRoomWindow::Instance == nullptr)
        return "Your party";
    std::string only;
    int others = 0;
    for (const PlayerEntry& entry : ZeldaOnlineRoomWindow::Instance->Players()) {
        if (!entry.isInParty || entry.networkID == (uint32_t)c->m_myNetworkID)
            continue;
        others++;
        only = entry.name;
    }
    return others == 1 && !only.empty() ? only : std::string("Your party");
}

} // namespace ZeldaOnline
