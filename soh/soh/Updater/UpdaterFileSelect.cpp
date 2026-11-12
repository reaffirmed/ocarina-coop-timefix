// The update prompt on the file select: a panel over the main menu in the style of the Co-op screen (dark glass with
// a blue edge, the message font, the cursor arrow and the file select sounds), used with the stick / D-pad, A and B.
// While it is closed, a short line at the top left says what the updater is doing (downloading, ready to install).
#include "Updater.h"
#include "UpdaterInternal.h"

#include <algorithm>
#include <cstdlib>
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
#include "soh/frame_interpolation.h"

extern "C" {
void FileChoose_DrawTextRec(GraphicsContext* gfxCtx, s32 r, s32 g, s32 b, s32 a, f32 x, f32 y, f32 z, s32 s, s32 t,
                            f32 dx, f32 dy);
}

using namespace UpdaterGame;

namespace {

constexpr float kTextScale = 0.8f;
constexpr int kPanelX = 44;
constexpr int kPanelW = 232;
constexpr int kTextX = kPanelX + 14;
constexpr float kTextW = kPanelW - 28;
constexpr int kRowStep = 16;

struct Row {
    std::string label;
    Choice choice;
};

struct Prompt {
    std::string title;
    std::vector<std::string> body;
    std::vector<Row> rows;
};

int16_t sAlpha = 255;
bool sNoticeOpen = false; // the outcome of the last install is shown first, once
bool sNoticeTaken = false;

void PlaySfx(u16 sfx) {
    Audio_PlaySoundGeneral(sfx, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                           &gSfxDefaultReverb);
}

float TextWidth(const std::string& text, float scale) {
    float w = 0.0f;
    for (unsigned char c : text)
        w += Ship_GetCharFontWidth(c) * (R_TEXT_CHAR_SCALE / 100.0f) * scale;
    return w;
}

std::vector<std::string> Wrap(const std::string& text, float width, float scale) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&]() {
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
            flush();
        else
            word += c;
    }
    flush();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

u8 A(int a) {
    return (u8)std::clamp(a * sAlpha / 255, 0, 255);
}

} // namespace

// OPEN_DISPS declares the frame interpolation hooks where it is used: these must not be in a namespace
static void Text(FileChooseContext* ctx, const std::string& text, int x, int y, u8 r, u8 g, u8 b, float scale) {
    OPEN_DISPS(ctx->state.gfxCtx);
    // the file buttons leave a combiner that ignores the primitive colour: set the one the text needs
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);
    CLOSE_DISPS(ctx->state.gfxCtx);
    Interface_DrawTextLine(ctx->state.gfxCtx, (char*)text.c_str(), x, y, r, g, b, sAlpha, scale, true);
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

// the Co-op screen's panel: dark glass with a thin blue edge (darker, it sits over the file buttons)
static void Panel(FileChooseContext* ctx, int x, int y, int w, int h) {
    Rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 140);
    Rect(ctx, x, y, w, h, 0, 8, 32, 245);
    Rect(ctx, x, y, w, 1, 100, 150, 255, 220);
    Rect(ctx, x, y + h - 1, w, 1, 100, 150, 255, 140);
    Rect(ctx, x, y, 1, h, 100, 150, 255, 140);
    Rect(ctx, x + w - 1, y, 1, h, 100, 150, 255, 140);
}

static void Cursor(FileChooseContext* ctx, int x, int y) {
    OPEN_DISPS(ctx->state.gfxCtx);
    Gfx_SetupDL_39Opa(ctx->state.gfxCtx);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPLoadTextureBlock(POLY_OPA_DISP++, gArrowCursorTex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 24, 0,
                        G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    FileChoose_DrawTextRec(ctx->state.gfxCtx, ctx->stickRightPrompt.arrowColorR, ctx->stickRightPrompt.arrowColorG,
                           ctx->stickRightPrompt.arrowColorB, sAlpha, (f32)x, (f32)y, 0.42f, 0, 0, 1.0f, 1.0f);
    CLOSE_DISPS(ctx->state.gfxCtx);
}

namespace {

bool FromSteamHelper() {
    return std::getenv("OOT_SERVER") != nullptr;
}

// What the open prompt shows. Called with the lock held.
Prompt Build(Shared& s) {
    Prompt p;
    const std::string have = "v" + CurrentVersionText();
    if (sNoticeOpen) {
        p.title = "Story Co-op";
        p.body.push_back(s.notice);
        p.rows = { { "OK", Choice::Ok } };
        return p;
    }
    switch (s.state) {
        case State::Available:
            p.title = "Update available";
            p.body.push_back("Story Co-op " + s.release.tag + " is out. You have " + have + ".");
            p.body.push_back("Everyone in a co-op game needs the same version. Your saves and settings are kept.");
            p.rows = { { "Update", Choice::Update },
                       { "Skip", Choice::Skip },
                       { "Ignore until next update", Choice::Ignore } };
            break;
        case State::Ready:
            p.title = "Update ready";
            p.body.push_back(s.release.tag + " is downloaded and checked. It is installed when you close the game.");
            if (FromSteamHelper()) {
                p.body.push_back("Then start it from Host or Join over Steam again.");
                p.rows = { { "Close the game and install", Choice::InstallNow },
                           { "Install when I close the game", Choice::Later } };
            } else {
                p.rows = { { "Restart and install now", Choice::InstallNow },
                           { "Install when I close the game", Choice::Later } };
            }
            break;
        case State::Failed:
            p.title = "Update not installed";
            p.body.push_back(s.message);
            p.body.push_back("Nothing was changed. The game asks again the next time it starts.");
            p.rows = { { "OK", Choice::Ok } };
            break;
        default:
            break;
    }
    return p;
}

// The short line at the top left while the prompt is closed.
std::string StatusLine(Shared& s) {
    switch (s.state) {
        case State::Downloading: {
            const uint64_t total = s.release.package.size;
            const uint64_t have = DownloadedBytes(s.downloadPart);
            if (total > 0 && have > 0)
                return "Downloading update " + std::to_string((int)std::min<uint64_t>(99, have * 100 / total)) + "%";
            return "Downloading update";
        }
        case State::Ready:
            return "Update " + s.release.tag + " installs when you close the game";
        default:
            return "";
    }
}

} // namespace

extern "C" int Updater_FileSelectUpdate(FileChooseContext* ctx) {
    Shared& s = S();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!sNoticeTaken && !s.notice.empty()) {
        sNoticeTaken = true;
        sNoticeOpen = true;
        s.cursor = 0;
    }
    if (!sNoticeOpen && !s.promptOpen)
        return 0;
    Prompt p = Build(s);
    if (p.rows.empty()) {
        s.promptOpen = false;
        return 0;
    }
    Input* input = &ctx->state.input[0];
    const bool dpad = CVarGetInteger(CVAR_SETTING("DpadInText"), 0);
    const int count = (int)p.rows.size();
    s.cursor = std::clamp(s.cursor, 0, count - 1);
    if (count > 1 &&
        (std::abs(ctx->stickRelY) > 30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DDOWN | BTN_DUP)))) {
        const bool down = ctx->stickRelY < -30 || (dpad && CHECK_BTN_ANY(input->press.button, BTN_DDOWN));
        s.cursor = (s.cursor + (down ? 1 : -1) + count) % count;
        PlaySfx(NA_SE_SY_FSEL_CURSOR);
    }
    Choice choice;
    bool chosen = false;
    if (CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_START)) {
        choice = p.rows[s.cursor].choice;
        chosen = true;
        PlaySfx(choice == Choice::Update || choice == Choice::InstallNow ? NA_SE_SY_FSEL_DECIDE_L
                                                                         : NA_SE_SY_FSEL_CLOSE);
    } else if (CHECK_BTN_ALL(input->press.button, BTN_B)) {
        // B is the gentle answer: Skip, Later or OK
        choice = sNoticeOpen || s.state == State::Failed ? Choice::Ok
                 : s.state == State::Ready               ? Choice::Later
                                                         : Choice::Skip;
        chosen = true;
        PlaySfx(NA_SE_SY_FSEL_CLOSE);
    }
    if (chosen) {
        s.cursor = 0;
        if (sNoticeOpen) {
            sNoticeOpen = false;
            s.notice.clear();
        } else {
            lock.unlock();
            Choose(choice);
        }
    }
    return 1; // the menu behind does not move while the prompt is open
}

extern "C" void Updater_FileSelectDraw(FileChooseContext* ctx) {
    Shared& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    sAlpha = std::min<int16_t>(ctx->windowAlpha, 255);

    if (!sNoticeOpen && !s.promptOpen) {
        const std::string line = StatusLine(s);
        if (!line.empty())
            Text(ctx, line, 28, 20, 170, 220, 255, 0.7f);
        return;
    }
    Prompt p = Build(s);
    if (p.rows.empty())
        return;

    std::vector<std::string> lines;
    for (const auto& para : p.body)
        for (const auto& l : Wrap(para, kTextW, 0.7f))
            lines.push_back(l);
    const int h = 18 + 16 + (int)lines.size() * 12 + 8 + (int)p.rows.size() * kRowStep + 10;
    const int y0 = std::max(16, (SCREEN_HEIGHT - h) / 2);
    Panel(ctx, kPanelX, y0, kPanelW, h);

    int y = y0 + 10;
    Text(ctx, p.title, (int)(SCREEN_WIDTH / 2 - TextWidth(p.title, 0.9f) / 2), y, 255, 255, 255, 0.9f);
    y += 20;
    for (const auto& l : lines) {
        Text(ctx, l, kTextX, y, 200, 200, 200, 0.7f);
        y += 12;
    }
    y += 8;
    for (int i = 0; i < (int)p.rows.size(); i++) {
        const bool selected = i == s.cursor;
        Text(ctx, p.rows[i].label, kTextX + 12, y, 255, 255, selected ? 80 : 255, kTextScale);
        if (selected)
            Cursor(ctx, kTextX + 4, y + 5);
        y += kRowStep;
    }
}
