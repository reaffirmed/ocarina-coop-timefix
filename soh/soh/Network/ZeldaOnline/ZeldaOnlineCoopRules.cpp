// Co-op rules for Story Co-op: friendly fire, the downed state and revives (Zelda's Lullaby, bottles), the death marker
// and the Dark Link that guards it, the ready check at dungeon entrances and boss doors, the respawn cooldown, the
// in-game Co-op menu (teleport to a teammate) and the summary of what a joining player received.
//
// Every rule is a party setting (PartySettings.hpp) that the host picks per playthrough. All of them are off by
// default and only work while Story Co-op is on, so a party that never sets them plays exactly as before.
//
// Messages between teammates are scene triggers, which the server only hands to the players in the same scene:
//  co_down   <seconds left>              this player is down and can be revived
//  co_up     <0 gave up, 1 revived>      this player is no longer down
//  co_revive <target> <kind> <health>    a teammate revives the target (kind: COOP_REVIVE_*)
//  co_give   <target> <kind>             a teammate gives the target a bottle (kind: COOP_GIVE_*)
//  co_ready                              this player's held transition is a ready check (the vote follows)

#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "Packet.hpp"
#include "PacketTypes.hpp"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/Notification/Notification.h"
#include "soh/SaveManager.h"
#include "soh/ShipUtils.h"
#include "soh/util.h"
#include <ship/Context.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "functions.h"
#include "macros.h"

extern PlayState* gPlayState;
extern SaveContext gSaveContext;
extern s16 gGameOverTimer;
float OTRGetRectDimensionFromLeftEdge(float v);
void Player_ZoRevive(PlayState* play, Player* player, s16 health);
void Player_ZoStartGameOver(PlayState* play, Player* player);
}

void DrawScreenText(GraphicsContext* gfxCtx, const char* text, s16 x, s16 y, Color_RGBA8 color, f32 scale);

namespace ZeldaOnline {

namespace {

enum : uint8_t { COOP_REVIVE_SONG = 0, COOP_REVIVE_FAIRY = 1, COOP_REVIVE_RED = 2, COOP_REVIVE_BLUE = 3 };
enum : uint8_t { COOP_GIVE_RED = 0, COOP_GIVE_BLUE = 1, COOP_GIVE_GREEN = 2, COOP_GIVE_MILK = 3 };

constexpr int kFps = 20;
constexpr s16 kSongMagicCost = 48;        // the whole small meter, half of the upgraded one
constexpr s16 kSongReviveHealth = 3 * 16; // 3 hearts
constexpr s16 kFairyReviveHealth = 8 * 16;
constexpr s16 kMilkHealth = 5 * 16;
constexpr float kSongReach = 160.0f;
constexpr float kBottleReviveReach = 110.0f;
constexpr float kBottleGiveReach = 100.0f;
constexpr float kMarkerPickupReach = 40.0f;
constexpr float kDarkLinkWakeReach = 300.0f;
constexpr int kAloneGameOverFrames = 3 * kFps;
constexpr int kGiveUpFrames = kFps;
constexpr int kReadyPassFrames = 20 * kFps;
constexpr int kJoinSummaryFrames = 10 * kFps;

std::string Wrap(const std::string& text, size_t maxLine = 44) {
    std::string wrapped;
    size_t lineLen = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string word = text.substr(pos, end - pos);
        if (lineLen != 0 && lineLen + 1 + word.size() > maxLine) {
            wrapped += '\n';
            lineLen = 0;
        } else if (lineLen != 0) {
            wrapped += ' ';
            lineLen++;
        }
        wrapped += word;
        lineLen += word.size();
        pos = end + 1;
    }
    return wrapped;
}

int LineCount(const std::string& text) {
    return 1 + (int)std::count(text.begin(), text.end(), '\n');
}

float TextWidth(const std::string& text, float scale) {
    float width = 0.0f, best = 0.0f;
    for (char c : text) {
        if (c == '\n') {
            best = std::max(best, width);
            width = 0.0f;
            continue;
        }
        width += Ship_GetCharFontWidth((u8)c) * scale;
    }
    return std::max(best, width);
}

void FillRect(GraphicsContext* gfxCtx, s16 x, s16 y, s16 w, s16 h, Color_RGBA8 color) {
    GraphicsContext* __gfxCtx = gfxCtx;
    Gfx_SetupDL_39Overlay(gfxCtx);
    gDPSetRenderMode(OVERLAY_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, color.r, color.g, color.b, color.a);
    s16 x0 = (s16)OTRGetRectDimensionFromLeftEdge((f32)x);
    gDPFillWideRectangle(OVERLAY_DISP++, x0, y, x0 + w, y + h);
}

const PlayerEntry* FindMember(uint32_t networkID) {
    if (ZeldaOnlineRoomWindow::Instance == nullptr)
        return nullptr;
    for (const PlayerEntry& entry : ZeldaOnlineRoomWindow::Instance->Players())
        if (entry.networkID == networkID)
            return &entry;
    return nullptr;
}

bool IsPartyMember(uint32_t networkID) {
    const PlayerEntry* entry = FindMember(networkID);
    return entry != nullptr && entry->isInParty;
}

std::string MemberName(uint32_t networkID) {
    const PlayerEntry* entry = FindMember(networkID);
    return (entry != nullptr && !entry->name.empty()) ? entry->name : std::string("A teammate");
}

float DistXZ(const Vec3f& a, const Vec3f& b) {
    return sqrtf(SQ(a.x - b.x) + SQ(a.z - b.z));
}

float Dist3(const Vec3f& a, const Vec3f& b) {
    return sqrtf(SQ(a.x - b.x) + SQ(a.y - b.y) + SQ(a.z - b.z));
}

bool IsDungeonScene(int scene) {
    return (scene >= SCENE_DEKU_TREE && scene <= SCENE_INSIDE_GANONS_CASTLE && scene != SCENE_THIEVES_HIDEOUT);
}

bool IsBossScene(int scene) {
    return scene >= SCENE_DEKU_TREE_BOSS && scene <= SCENE_GANONDORF_BOSS;
}

bool ActorStillLoaded(PlayState* play, Actor* target) {
    if (play == nullptr || target == nullptr)
        return false;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++)
        for (Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next)
            if (a == target)
                return true;
    return false;
}

// Moments when teammates should not be able to hurt each other: text boxes, cutscenes, the votes.
bool InQuietMoment(PlayState* play, Player* player) {
    return play->msgCtx.msgMode != MSGMODE_NONE || play->csCtx.state != CS_STATE_IDLE || player->csAction != 0 ||
           (player->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE) || play->transitionTrigger != TRANS_TRIGGER_OFF ||
           play->transitionMode != TRANS_MODE_OFF;
}

std::filesystem::path MarkerFile() {
    return std::filesystem::path(Ship::Context::GetPathRelativeToAppDirectory("Save")) / "coop" / "markers.json";
}

std::string MarkerKeyForLoadedSave() {
    if (gSaveContext.fileNum == SaveManager::CoopFileNum)
        return "coop:" + SaveManager::Instance->CoopFile().filename().string();
    return "slot:" + std::to_string((int)gSaveContext.fileNum);
}

const char* RupeeShareName(int share) {
    switch (share) {
        case RUPEES_LOST_QUARTER:
            return "a quarter";
        case RUPEES_LOST_HALF:
            return "half";
        case RUPEES_LOST_ALL:
            return "all";
        default:
            return "none";
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Common
// ---------------------------------------------------------------------------------------------------------------

bool ZeldaOnlineClient::CoopRulesActive() const {
    return StoryCoopActive() && gPlayState != nullptr && gSaveContext.fileNum != 0xFF &&
           gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool ZeldaOnlineClient::HitsTeammates() const {
    if (CVarGetInteger("gZeldaOnline.Pvp", 0) != 0)
        return true;
    return StoryCoopActive() && m_partySettings.friendlyFire != FRIENDLY_FIRE_OFF;
}

void ZeldaOnlineClient::CoopShowNotice(const std::string& text, int seconds) {
    m_coopNotice = text;
    m_coopNoticeFrames = seconds * kFps;
}

// A party member in this scene who is up (not down) and could revive this player.
bool ZeldaOnlineClient::CoopMateCanHelp() const {
    for (auto& entry : m_networkedActors) {
        AbstractActorController* c = entry.second;
        if (c == nullptr || !c->IsPlayer() || c->GetActor() == nullptr)
            continue;
        uint32_t id = (uint32_t)c->NetworkID();
        if (!IsPartyMember(id) || m_coopDowned.count(id) != 0)
            continue;
        return true;
    }
    return false;
}

bool ZeldaOnlineClient::CoopOtherMemberInScene() const {
    for (auto& entry : m_networkedActors) {
        AbstractActorController* c = entry.second;
        if (c != nullptr && c->IsPlayer() && c->GetActor() != nullptr && IsPartyMember((uint32_t)c->NetworkID()))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Friendly fire
// ---------------------------------------------------------------------------------------------------------------

// Called when something hit the local player's body. Returns false to drop the hit.
bool ZeldaOnlineClient::CoopFriendlyFireHit(Player* player, Actor* attacker) {
    m_coopFfHit = false;
    if (attacker == nullptr || gPlayState == nullptr)
        return true;

    const bool fromTeammate = (attacker->id == ACTOR_PLAYER && attacker != &player->actor) ||
                              (attacker->id == ACTOR_EN_ARROW && (attacker->flags & ACTOR_FLAG_ZO_USER1)) ||
                              (attacker->id == ACTOR_EN_BOOM && attacker != player->boomerangActor);
    if (!fromTeammate)
        return true;

    // the old PvP switch keeps working as it did, outside of the co-op rules
    if (!StoryCoopActive() || m_partySettings.friendlyFire == FRIENDLY_FIRE_OFF)
        return true;

    if (InQuietMoment(gPlayState, player) || m_storyHold || m_storyWarpHold || m_storyWarpVoting || m_coopIsDowned ||
        m_coopMenuOpen)
        return false;

    m_coopFfHit = true;
    if (m_partySettings.friendlyFire == FRIENDLY_FIRE_LIGHT) {
        // half damage, and a teammate never takes the last heart
        s32 damage = std::max(1, (s32)player->actor.colChkInfo.damage / 2);
        damage = std::min(damage, (s32)gSaveContext.health - 16);
        if (damage <= 0) {
            m_coopFfHit = false;
            return false;
        }
        player->actor.colChkInfo.damage = (u8)damage;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Downed state and revives
// ---------------------------------------------------------------------------------------------------------------

bool ZeldaOnlineClient::CoopEnterDowned(PlayState* play, Player* player) {
    (void)player;
    if (!CoopRulesActive() || !m_partySettings.DownedEnabled() || play != gPlayState)
        return false;
    if (m_coopIsDowned || !CoopMateCanHelp())
        return false;

    m_coopIsDowned = true;
    m_coopDownedFrames = m_partySettings.downedSeconds * kFps;
    m_coopGiveUpFrames = 0;
    m_coopAloneFrames = 0;
    m_coopMenuOpen = false;

    ByteStream p;
    p << PackedUInt1((unsigned int)m_partySettings.downedSeconds);
    SendSceneTrigger("co_down", p);
    return true;
}

void ZeldaOnlineClient::CoopEndDowned(bool gameOver) {
    if (!m_coopIsDowned)
        return;
    m_coopIsDowned = false;
    m_coopGiveUpFrames = 0;

    if (gPlayState != nullptr) {
        ByteStream p;
        p << PackedUInt1(gameOver ? 0u : 1u);
        SendSceneTrigger("co_up", p);
        if (gameOver) {
            Player* player = GET_PLAYER(gPlayState);
            if (player != nullptr)
                Player_ZoStartGameOver(gPlayState, player);
        }
    }
}

void ZeldaOnlineClient::CoopUpdateDowned(PlayState* play, Player* player) {
    // teammates who stopped saying they are down (revived, left, game over) are dropped
    for (auto it = m_coopDowned.begin(); it != m_coopDowned.end();) {
        it->second.quiet++;
        AbstractActorController* c = GetNetworkController((int)it->first);
        if (it->second.quiet > 3 * kFps || c == nullptr) {
            if (c != nullptr && c->GetActor() != nullptr)
                NameTag_RemoveAllByActorTag(c->GetActor(), "co_down");
            it = m_coopDowned.erase(it);
        } else {
            ++it;
        }
    }

    if (!m_coopIsDowned)
        return;

    // revived by something else (a fairy picked up, a scene change), or the Game Over took over
    if (!(player->stateFlags1 & PLAYER_STATE1_DEAD) || play->gameOverCtx.state != GAMEOVER_INACTIVE) {
        m_coopIsDowned = false;
        return;
    }

    if (m_coopDownedFrames > 0)
        m_coopDownedFrames--;
    if ((m_coopFrame % kFps) == 0) {
        ByteStream p;
        p << PackedUInt1((unsigned int)((m_coopDownedFrames + kFps - 1) / kFps));
        SendSceneTrigger("co_down", p);
    }

    if (play->state.input[0].cur.button & BTN_B)
        m_coopGiveUpFrames++;
    else
        m_coopGiveUpFrames = 0;

    m_coopAloneFrames = CoopMateCanHelp() ? 0 : m_coopAloneFrames + 1;

    if (m_coopDownedFrames <= 0 || m_coopGiveUpFrames >= kGiveUpFrames || m_coopAloneFrames >= kAloneGameOverFrames ||
        !isConnected)
        CoopEndDowned(true);
}

// A teammate's revive reached this player.
void ZeldaOnlineClient::CoopRevive(uint32_t target, uint8_t kind, int16_t health) {
    (void)target;
    if (!m_coopIsDowned || gPlayState == nullptr)
        return;
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr)
        return;

    m_coopIsDowned = false;
    Player_ZoRevive(gPlayState, player, health);
    if (kind == COOP_REVIVE_BLUE)
        Magic_Fill(gPlayState);

    ByteStream p;
    p << PackedUInt1(1u);
    SendSceneTrigger("co_up", p);
}

// Zelda's Lullaby next to a downed teammate.
static void CoopOnSongPlayed() {
    auto* client = ZeldaOnlineClient::Instance;
    if (client != nullptr)
        client->CoopTrySongRevive();
}

void ZeldaOnlineClient::CoopTrySongRevive() {
    if (!CoopRulesActive() || !m_partySettings.songRevive)
        return;
    PlayState* play = gPlayState;
    if (play->msgCtx.lastPlayedSong != OCARINA_SONG_LULLABY ||
        play->msgCtx.ocarinaAction != OCARINA_ACTION_FREE_PLAY_DONE)
        return;
    Player* player = GET_PLAYER(play);
    if (player == nullptr || m_coopIsDowned)
        return;

    uint32_t best = 0;
    float bestDist = kSongReach;
    for (auto& mate : m_coopDowned) {
        AbstractActorController* c = GetNetworkController((int)mate.first);
        if (c == nullptr || c->GetActor() == nullptr)
            continue;
        float d = Dist3(player->actor.world.pos, c->GetActor()->world.pos);
        if (d <= bestDist) {
            bestDist = d;
            best = mate.first;
        }
    }
    if (best == 0)
        return;

    if (!gSaveContext.isMagicAcquired || gSaveContext.magic < kSongMagicCost) {
        CoopShowNotice("Not enough magic. Reviving takes a full small magic meter (half of the big one).");
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }

    Magic_RequestChange(play, kSongMagicCost, MAGIC_CONSUME_NOW);
    ByteStream p;
    p << PackedUInt2(best) << PackedUInt1(COOP_REVIVE_SONG) << PackedInt2(kSongReviveHealth);
    SendSceneTrigger("co_revive", p);
    m_coopDowned.erase(best);
    if (AbstractActorController* c = GetNetworkController((int)best))
        if (c->GetActor() != nullptr)
            NameTag_RemoveAllByActorTag(c->GetActor(), "co_down");
    CoopShowNotice("You revived " + MemberName(best) + ".");
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
}

// A fairy, potion or milk used next to a teammate goes to them. Returns true when the bottle was given.
bool ZeldaOnlineClient::CoopGiveBottle(PlayState* play, Player* player, s32 itemAction) {
    if (!CoopRulesActive() || !m_partySettings.bottleRevive || play != gPlayState || m_coopIsDowned)
        return false;

    // a downed teammate close by comes first: fairies and red or blue potions revive
    uint32_t target = 0;
    bool revive = false;
    if (itemAction == PLAYER_IA_BOTTLE_FAIRY || itemAction == PLAYER_IA_BOTTLE_POTION_RED ||
        itemAction == PLAYER_IA_BOTTLE_POTION_BLUE) {
        float bestDist = kBottleReviveReach;
        for (auto& mate : m_coopDowned) {
            AbstractActorController* c = GetNetworkController((int)mate.first);
            if (c == nullptr || c->GetActor() == nullptr)
                continue;
            float d = Dist3(player->actor.world.pos, c->GetActor()->world.pos);
            if (d <= bestDist) {
                bestDist = d;
                target = mate.first;
                revive = true;
            }
        }
    }

    // otherwise a teammate who is up, right in front of this player
    if (target == 0 && itemAction != PLAYER_IA_BOTTLE_FAIRY) {
        float bestDist = kBottleGiveReach;
        for (auto& entry : m_networkedActors) {
            AbstractActorController* c = entry.second;
            if (c == nullptr || !c->IsPlayer() || c->GetActor() == nullptr)
                continue;
            uint32_t id = (uint32_t)c->NetworkID();
            if (!IsPartyMember(id) || m_coopDowned.count(id) != 0)
                continue;
            Actor* mate = c->GetActor();
            float d = Dist3(player->actor.world.pos, mate->world.pos);
            s16 yaw = (s16)(Math_Vec3f_Yaw(&player->actor.world.pos, &mate->world.pos) - player->actor.shape.rot.y);
            if (d <= bestDist && ABS(yaw) <= 0x2AAA) {
                bestDist = d;
                target = id;
            }
        }
    }
    if (target == 0)
        return false;

    const std::string name = MemberName(target);
    std::string what;
    s32 emptied = ITEM_BOTTLE;
    if (revive) {
        uint8_t kind = COOP_REVIVE_FAIRY;
        s16 health = kFairyReviveHealth;
        what = "a fairy";
        if (itemAction == PLAYER_IA_BOTTLE_POTION_RED) {
            kind = COOP_REVIVE_RED;
            health = 0x140; // clamped to the teammate's hearts
            what = "a red potion";
        } else if (itemAction == PLAYER_IA_BOTTLE_POTION_BLUE) {
            kind = COOP_REVIVE_BLUE;
            health = 0x140;
            what = "a blue potion";
        }
        ByteStream p;
        p << PackedUInt2(target) << PackedUInt1(kind) << PackedInt2(health);
        SendSceneTrigger("co_revive", p);
        m_coopDowned.erase(target);
        if (AbstractActorController* c = GetNetworkController((int)target))
            if (c->GetActor() != nullptr)
                NameTag_RemoveAllByActorTag(c->GetActor(), "co_down");
        CoopShowNotice("You revived " + name + " with " + what + ".");
    } else {
        uint8_t kind;
        switch (itemAction) {
            case PLAYER_IA_BOTTLE_POTION_RED:
                kind = COOP_GIVE_RED;
                what = "a red potion";
                break;
            case PLAYER_IA_BOTTLE_POTION_BLUE:
                kind = COOP_GIVE_BLUE;
                what = "a blue potion";
                break;
            case PLAYER_IA_BOTTLE_POTION_GREEN:
                kind = COOP_GIVE_GREEN;
                what = "a green potion";
                break;
            case PLAYER_IA_BOTTLE_MILK_FULL:
            case PLAYER_IA_BOTTLE_MILK_HALF:
                kind = COOP_GIVE_MILK;
                what = "milk";
                if (itemAction == PLAYER_IA_BOTTLE_MILK_FULL)
                    emptied = ITEM_MILK_HALF;
                break;
            default:
                return false;
        }
        ByteStream p;
        p << PackedUInt2(target) << PackedUInt1(kind);
        SendSceneTrigger("co_give", p);
        CoopShowNotice("You gave " + name + " " + what + ".");
    }

    Player_UpdateBottleHeld(play, player, emptied,
                            emptied == ITEM_MILK_HALF ? PLAYER_IA_BOTTLE_MILK_HALF : PLAYER_IA_BOTTLE);
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Game Over: the death marker, the respawn cooldown
// ---------------------------------------------------------------------------------------------------------------

s16 ZeldaOnlineClient::CoopRespawnCooldownFrames() const {
    if (!StoryCoopActive())
        return 0;
    return (s16)(m_partySettings.respawnCooldown * kFps);
}

void ZeldaOnlineClient::CoopOnGameOver(PlayState* play) {
    m_coopIsDowned = false;
    m_coopMenuOpen = false;
    if (!CoopRulesActive() || m_partySettings.deathPenalty == DEATH_PENALTY_OFF || play != gPlayState)
        return;

    int rupees = gSaveContext.rupees;
    int lost = 0;
    switch (m_partySettings.rupeesLost) {
        case RUPEES_LOST_QUARTER:
            lost = rupees / 4;
            break;
        case RUPEES_LOST_HALF:
            lost = rupees / 2;
            break;
        case RUPEES_LOST_ALL:
            lost = rupees;
            break;
        default:
            break;
    }
    if (lost <= 0)
        return;

    Player* player = GET_PLAYER(play);
    Vec3f pos = player != nullptr ? player->actor.world.pos : Vec3f{ 0, 0, 0 };
    const bool onGround =
        player != nullptr && (player->actor.bgCheckFlags & 1) && !(player->stateFlags1 & PLAYER_STATE1_IN_WATER);
    if (!onGround && m_coopGroundScene == play->sceneNum)
        pos = { m_coopGroundX, m_coopGroundY, m_coopGroundZ };

    // a new death replaces the old marker, and its rupees are gone with it
    Rupees_ChangeBy((s16)-lost);
    m_coopMarker.has = true;
    m_coopMarker.scene = play->sceneNum;
    m_coopMarker.age = gSaveContext.linkAge;
    m_coopMarker.x = pos.x;
    m_coopMarker.y = pos.y;
    m_coopMarker.z = pos.z;
    m_coopMarker.rupees = lost;
    m_coopDarkLinkSpawned = false;
    CoopSaveMarker();
}

void ZeldaOnlineClient::CoopLoadMarker() {
    m_coopMarker = CoopMarker();
    m_coopMarkerKey = MarkerKeyForLoadedSave();
    try {
        std::ifstream in(MarkerFile());
        if (!in)
            return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains(m_coopMarkerKey))
            return;
        const nlohmann::json& m = j[m_coopMarkerKey];
        if (!m.is_object())
            return;
        m_coopMarker.has = true;
        m_coopMarker.scene = m.value("scene", -1);
        m_coopMarker.age = m.value("age", 0);
        m_coopMarker.x = m.value("x", 0.0f);
        m_coopMarker.y = m.value("y", 0.0f);
        m_coopMarker.z = m.value("z", 0.0f);
        m_coopMarker.rupees = m.value("rupees", 0);
        if (m_coopMarker.scene < 0 || m_coopMarker.rupees <= 0)
            m_coopMarker = CoopMarker();
    } catch (...) { m_coopMarker = CoopMarker(); }
}

void ZeldaOnlineClient::CoopSaveMarker() {
    if (m_coopMarkerKey.empty())
        return;
    try {
        std::filesystem::path file = MarkerFile();
        std::filesystem::create_directories(file.parent_path());
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream in(file);
            if (in) {
                j = nlohmann::json::parse(in, nullptr, false);
                if (!j.is_object())
                    j = nlohmann::json::object();
            }
        }
        if (m_coopMarker.has) {
            j[m_coopMarkerKey] = { { "scene", m_coopMarker.scene }, { "age", m_coopMarker.age },
                                   { "x", m_coopMarker.x },         { "y", m_coopMarker.y },
                                   { "z", m_coopMarker.z },         { "rupees", m_coopMarker.rupees } };
        } else {
            j.erase(m_coopMarkerKey);
        }
        std::ofstream out(file);
        out << j.dump(2);
    } catch (const std::exception& e) { SPDLOG_WARN("[ZeldaOnline] could not save the death marker: {}", e.what()); }
}

void ZeldaOnlineClient::CoopUpdateMarker(PlayState* play, Player* player) {
    // remember where Link last stood on firm ground, for a death in water, lava or mid-air
    if ((player->actor.bgCheckFlags & 1) && !(player->stateFlags1 & (PLAYER_STATE1_IN_WATER | PLAYER_STATE1_DEAD))) {
        m_coopGroundX = player->actor.world.pos.x;
        m_coopGroundY = player->actor.world.pos.y;
        m_coopGroundZ = player->actor.world.pos.z;
        m_coopGroundScene = play->sceneNum;
    }

    if (m_coopDarkLink != nullptr && !ActorStillLoaded(play, m_coopDarkLink))
        m_coopDarkLink = nullptr;

    if (!m_coopMarker.has || m_coopMarker.scene != play->sceneNum || m_coopMarker.age != gSaveContext.linkAge)
        return;
    if (player->stateFlags1 & PLAYER_STATE1_DEAD)
        return;

    Vec3f markerPos = { m_coopMarker.x, m_coopMarker.y, m_coopMarker.z };
    const float dist = Dist3(player->actor.world.pos, markerPos);

    if ((m_coopFrame % 3) == 0 && dist < 2500.0f) {
        Vec3f pos = { markerPos.x + Rand_CenteredFloat(20.0f), markerPos.y + 10.0f + Rand_ZeroFloat(30.0f),
                      markerPos.z + Rand_CenteredFloat(20.0f) };
        Vec3f velocity = { 0.0f, 0.6f, 0.0f };
        Vec3f accel = { 0.0f, 0.0f, 0.0f };
        Color_RGBA8 prim = { 255, 255, 160, 255 };
        Color_RGBA8 env = { 60, 255, 60, 0 };
        EffectSsKiraKira_SpawnDispersed(play, &pos, &velocity, &accel, &prim, &env, 2000, 18);
    }

    const bool guarded = m_partySettings.deathPenalty == DEATH_PENALTY_DARK_LINK && LINK_IS_ADULT;

    if (guarded) {
        if (m_coopDarkLink != nullptr) {
            if (m_coopDarkLink->colChkInfo.health == 0) {
                // beaten: the rupees come back
                Rupees_ChangeBy((s16)m_coopMarker.rupees);
                CoopShowNotice("You beat Dark Link and got your " + std::to_string(m_coopMarker.rupees) +
                               " rupees back.");
                m_coopMarker = CoopMarker();
                m_coopDarkLink = nullptr;
                m_coopDarkLinkSpawned = false;
                CoopSaveMarker();
            }
            return;
        }
        if (dist < kDarkLinkWakeReach && !m_coopDarkLinkSpawned) {
            // only one Dark Link at a time (the Water Temple has its own)
            for (Actor* a = play->actorCtx.actorLists[ACTORCAT_BOSS].head; a != nullptr; a = a->next)
                if (a->id == ACTOR_EN_TORCH2)
                    return;
            for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next)
                if (a->id == ACTOR_EN_TORCH2)
                    return;
            s16 yaw = Math_Vec3f_Yaw(&markerPos, &player->actor.world.pos);
            // a direct spawn: this Dark Link exists only in this player's game
            m_coopDarkLink =
                Actor_SpawnDirect(&play->actorCtx, play, ACTOR_EN_TORCH2, markerPos.x, markerPos.y, markerPos.z, 0, yaw,
                                  0, markerPos.x, markerPos.y, markerPos.z, 0, yaw, 0, 0, 0);
            if (m_coopDarkLink != nullptr) {
                m_coopDarkLinkSpawned = true;
                if (m_partySettings.darkLinkStrength == 0)
                    m_coopDarkLink->colChkInfo.health = (u8)std::max(1, (int)m_coopDarkLink->colChkInfo.health / 2);
                CoopShowNotice("Dark Link guards your rupees. Beat him to get them back.");
            }
        }
        return;
    }

    if (DistXZ(player->actor.world.pos, markerPos) < kMarkerPickupReach &&
        fabsf(player->actor.world.pos.y - markerPos.y) < 80.0f) {
        Rupees_ChangeBy((s16)m_coopMarker.rupees);
        Sfx_PlaySfxCentered(NA_SE_SY_GET_RUPY);
        CoopShowNotice("You got your " + std::to_string(m_coopMarker.rupees) + " rupees back.");
        m_coopMarker = CoopMarker();
        CoopSaveMarker();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Ready check: dungeon entrances and boss doors wait for the members in the same scene
// ---------------------------------------------------------------------------------------------------------------
//
// It runs through the warp vote (see ZeldaOnlineStoryCoop.cpp): the transition is held, the members in the scene are
// asked to press L, and it goes ahead when they are ready or when the holder presses L again. A "co_ready" message
// goes first so the others see the ready check text instead of the story text.

void ZeldaOnlineClient::CoopWatchReadyCheck() {
    PlayState* play = gPlayState;
    if (m_coopReadyPass > 0)
        m_coopReadyPass--;

    const bool transitioning = play->transitionTrigger == TRANS_TRIGGER_START;
    if (!transitioning) {
        m_coopReadySeen = false;
        return;
    }
    if (m_coopReadySeen)
        return;
    m_coopReadySeen = true;

    if (!m_partySettings.readyCheck || m_storyWarpReleased || m_storyWarpHold || m_storyHold || m_coopReadyPass > 0)
        return;
    if (play->transitionMode != TRANS_MODE_OFF || gSaveContext.respawnFlag != 0 ||
        gSaveContext.nextCutsceneIndex >= 0xFFF0 || play->linkAgeOnLoad != gSaveContext.linkAge)
        return;
    const s32 entrance = play->nextEntranceIndex;
    if (entrance < 0 || entrance >= ENTR_MAX)
        return;
    const int dest = gEntranceTable[entrance].scene;
    const int here = play->sceneNum;
    const bool intoDungeon = IsDungeonScene(dest) && !IsDungeonScene(here) && !IsBossScene(here);
    const bool intoBoss = IsBossScene(dest) && dest != here;
    if (!intoDungeon && !intoBoss)
        return;
    Player* player = GET_PLAYER(play);
    if (player == nullptr || gSaveContext.health <= 0 || !CoopOtherMemberInScene())
        return;

    SendSceneTrigger("co_ready", ByteStream());
    m_coopReadyHold = true;
    StoryStartWarpHold(play);
    m_storyCsTransitionSeen = false;
}

std::string ZeldaOnlineClient::CoopReadyText() const {
    std::string names;
    for (size_t i = 0; i < m_storyWarpMissing.size(); i++) {
        if (i != 0)
            names += ", ";
        names += m_storyWarpMissing[i];
    }
    const std::string count = "(" + std::to_string(m_storyWarpVoted) + "/" + std::to_string(m_storyWarpTotal) + ")";
    std::string text;
    if (m_storyWarpHold) {
        text = "Ready check: waiting for the others " + count;
        if (!names.empty())
            text += ": " + names;
        text += m_storyWarpHeard ? ". Press L to go in now." : ".";
    } else if (m_storyWarpReady) {
        text = "Ready " + count;
        if (!names.empty())
            text += ", waiting for " + names;
        text += ". Press B to cancel.";
    } else {
        text = (m_storyWarpStarter.empty() ? std::string("A party member") : m_storyWarpStarter) +
               " is ready to go on " + count + ". Press L when you are ready.";
    }
    return Wrap(text);
}

// ---------------------------------------------------------------------------------------------------------------
// Messages from teammates
// ---------------------------------------------------------------------------------------------------------------

void ZeldaOnlineClient::CoopOnSceneTrigger(AbstractActorController* sendingPlayer, const std::string& name,
                                           ByteStream& data) {
    if (sendingPlayer == nullptr || !StoryCoopActive() || gPlayState == nullptr)
        return;
    const uint32_t from = (uint32_t)sendingPlayer->NetworkID();
    if (!IsPartyMember(from))
        return;

    if (name == "co_down") {
        if (data.BytesLeft() < 1)
            return;
        int seconds = (int)data.Read<PackedUInt1>().value();
        bool isNew = m_coopDowned.count(from) == 0;
        CoopDownedMate& mate = m_coopDowned[from];
        mate.seconds = seconds;
        mate.quiet = 0;
        if (Actor* actor = sendingPlayer->GetActor()) {
            NameTag_RemoveAllByActorTag(actor, "co_down");
            std::string tag = "DOWN " + std::to_string(seconds);
            NameTagOptions options = { "co_down", 30, { 255, 90, 90, 255 }, true, 1.0f };
            NameTag_RegisterForActorWithOptions(actor, tag.c_str(), options);
        }
        if (isNew) {
            Sfx_PlaySfxCentered(NA_SE_SY_WARNING_COUNT_N);
            CoopShowNotice(MemberName(from) + " is down!", 3);
        }
    } else if (name == "co_up") {
        uint8_t revived = data.BytesLeft() >= 1 ? (uint8_t)data.Read<PackedUInt1>().value() : 0;
        if (m_coopDowned.erase(from) != 0) {
            if (Actor* actor = sendingPlayer->GetActor())
                NameTag_RemoveAllByActorTag(actor, "co_down");
            if (!revived)
                CoopShowNotice(MemberName(from) + " went back to the last entrance.", 3);
        }
    } else if (name == "co_revive") {
        if (data.BytesLeft() < 5)
            return;
        uint32_t target = data.Read<PackedUInt2>().value();
        uint8_t kind = (uint8_t)data.Read<PackedUInt1>().value();
        int16_t health = (int16_t)data.Read<PackedInt2>().value();
        if (target == (uint32_t)m_myNetworkID) {
            if (m_coopIsDowned) {
                CoopRevive(target, kind, health);
                CoopShowNotice(MemberName(from) + " revived you.");
            }
        } else if (m_coopDowned.erase(target) != 0) {
            if (AbstractActorController* c = GetNetworkController((int)target))
                if (c->GetActor() != nullptr)
                    NameTag_RemoveAllByActorTag(c->GetActor(), "co_down");
        }
    } else if (name == "co_give") {
        if (data.BytesLeft() < 3)
            return;
        uint32_t target = data.Read<PackedUInt2>().value();
        uint8_t kind = (uint8_t)data.Read<PackedUInt1>().value();
        if (target != (uint32_t)m_myNetworkID || m_coopIsDowned || gSaveContext.health <= 0)
            return;
        std::string what;
        switch (kind) {
            case COOP_GIVE_RED:
                gSaveContext.healthAccumulator = gSaveContext.healthCapacity;
                what = "a red potion";
                break;
            case COOP_GIVE_BLUE:
                gSaveContext.healthAccumulator = gSaveContext.healthCapacity;
                Magic_Fill(gPlayState);
                what = "a blue potion";
                break;
            case COOP_GIVE_GREEN:
                Magic_Fill(gPlayState);
                what = "a green potion";
                break;
            case COOP_GIVE_MILK:
                gSaveContext.healthAccumulator =
                    std::min<s16>(gSaveContext.healthCapacity, (s16)(gSaveContext.healthAccumulator + kMilkHealth));
                what = "milk";
                break;
            default:
                return;
        }
        CoopShowNotice(MemberName(from) + " gave you " + what + ".");
    } else if (name == "co_ready") {
        m_coopReadyVote = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The Co-op menu: L on the pause screen
// ---------------------------------------------------------------------------------------------------------------

namespace {
struct CoopMenuRow {
    std::string label;
    uint32_t networkID = 0; // 0: Close
    bool enabled = true;
};

std::vector<CoopMenuRow> BuildMenuRows() {
    std::vector<CoopMenuRow> rows;
    const uint8_t myAge = LINK_IS_ADULT ? 0 : 1;
    if (ZeldaOnlineRoomWindow::Instance != nullptr) {
        for (const PlayerEntry& entry : ZeldaOnlineRoomWindow::Instance->Players()) {
            if (!entry.isInParty)
                continue;
            CoopMenuRow row;
            row.networkID = entry.networkID;
            row.label = "Teleport to " + entry.name;
            if (entry.age < 2 && entry.age != myAge) {
                row.label += " (other era)";
                row.enabled = false;
            } else if (entry.age >= 2) {
                row.label += " (not in game)";
                row.enabled = false;
            }
            rows.push_back(row);
        }
    }
    CoopMenuRow close;
    close.label = "Close";
    rows.push_back(close);
    return rows;
}

bool PauseMenuIdle(PlayState* play) {
    return play->pauseCtx.state >= 4 && play->pauseCtx.state <= 7 && play->pauseCtx.debugState == 0;
}

void ClosePauseMenu(PlayState* play) {
    if (play->pauseCtx.state < 4 || play->pauseCtx.state > 7)
        return;
    Interface_SetDoAction(play, DO_ACTION_NONE);
    play->pauseCtx.state = 0x12;
    WREG(2) = -6240;
    func_800F64E0(0);
}
} // namespace

// Runs before the frame: the menu takes the controller while it is open.
void ZeldaOnlineClient::CoopMenuInput() {
    PlayState* play = gPlayState;
    if (play == nullptr || m_partyID == 0 || !isConnected || gSaveContext.fileNum == 0xFF) {
        m_coopMenuOpen = false;
        return;
    }
    Input* input = &play->state.input[0];
    const u16 pressed = input->press.button;

    if (!PauseMenuIdle(play)) {
        m_coopMenuOpen = false;
        return;
    }

    if (!m_coopMenuOpen) {
        // L also answers a vote, a ready check and a story scene prompt: those come first
        const bool lTaken = m_storyWarpHold || m_storyWarpVoting || m_storyHold || m_coopIsDowned ||
                            m_storyCsPromptFrames > 0 || play->msgCtx.msgMode != MSGMODE_NONE;
        if ((pressed & BTN_L) && !lTaken) {
            m_coopMenuOpen = true;
            m_coopMenuCursor = 0;
            m_coopMenuStickHeld = 0;
            Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
            input->press.button &= ~BTN_L;
            input->cur.button &= ~BTN_L;
        }
        return;
    }

    std::vector<CoopMenuRow> rows = BuildMenuRows();
    const int count = (int)rows.size();
    m_coopMenuCursor = CLAMP(m_coopMenuCursor, 0, count - 1);

    int move = 0;
    if (pressed & (BTN_DUP | BTN_CUP))
        move = -1;
    else if (pressed & (BTN_DDOWN | BTN_CDOWN))
        move = 1;
    else if (input->rel.stick_y > 30 || input->rel.stick_y < -30) {
        // the stick moves once, then repeats while it is held
        if (m_coopMenuStickHeld == 0 || (m_coopMenuStickHeld > 8 && (m_coopMenuStickHeld % 3) == 0))
            move = input->rel.stick_y > 0 ? -1 : 1;
        m_coopMenuStickHeld++;
    } else {
        m_coopMenuStickHeld = 0;
    }
    if (move != 0) {
        m_coopMenuCursor = (m_coopMenuCursor + move + count) % count;
        Sfx_PlaySfxCentered(NA_SE_SY_CURSOR);
    }

    if (pressed & (BTN_B | BTN_L | BTN_START)) {
        m_coopMenuOpen = false;
        Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
    } else if (pressed & BTN_A) {
        const CoopMenuRow& row = rows[m_coopMenuCursor];
        if (row.networkID == 0) {
            m_coopMenuOpen = false;
            Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
        } else if (!row.enabled) {
            Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        } else {
            WritePacket(newPacket(CLIENT_PACKET_TELEPORT_TO_PARTY_MEMBER) << PackedUInt2(row.networkID));
            m_coopMenuOpen = false;
            Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
            ClosePauseMenu(play);
            CoopShowNotice("Teleporting to " + MemberName(row.networkID) + "...", 3);
        }
    }

    // the pause screen does not see the buttons the menu used
    input->press.button = 0;
    input->cur.button &= ~(BTN_A | BTN_B | BTN_L | BTN_R | BTN_Z | BTN_START | BTN_CUP | BTN_CDOWN | BTN_CLEFT |
                           BTN_CRIGHT | BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT);
    input->rel.stick_x = input->rel.stick_y = 0;
    input->cur.stick_x = input->cur.stick_y = 0;
    input->press.stick_x = input->press.stick_y = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// What a joining player received
// ---------------------------------------------------------------------------------------------------------------

ZeldaOnlineClient::CoopJoinSnapshot ZeldaOnlineClient::CoopCaptureJoin() const {
    CoopJoinSnapshot s;
    s.has = true;
    for (int i = 0; i < 24; i++)
        s.items[i] = gSaveContext.inventory.items[i];
    s.equipment = gSaveContext.inventory.equipment;
    s.upgrades = gSaveContext.inventory.upgrades;
    s.questItems = gSaveContext.inventory.questItems;
    s.healthCapacity = gSaveContext.healthCapacity;
    s.magicLevel = gSaveContext.magicLevel;
    return s;
}

bool ZeldaOnlineClient::CoopDiffJoin(const CoopJoinSnapshot& before) {
    std::vector<std::string> got;
    for (int i = 0; i < 24; i++) {
        u8 item = gSaveContext.inventory.items[i];
        if (item != ITEM_NONE && item != before.items[i] && item < ITEM_NONE_FE)
            got.push_back(SohUtils::GetItemName(item));
    }
    for (int type = 0; type < 4; type++) {
        for (int inv = 0; inv < 3; inv++) {
            u16 bit = (u16)(1 << (type * 4 + inv));
            if ((gSaveContext.inventory.equipment & bit) && !(before.equipment & bit))
                got.push_back(SohUtils::GetItemName(ITEM_SWORD_KOKIRI + type * 3 + inv));
        }
    }
    static const char* kUpgradeNames[] = { "Bigger quiver", "Bigger bomb bag", "Stronger grip",    "Better scale",
                                           "Bigger wallet", "Bigger seed bag", "More Deku Sticks", "More Deku Nuts" };
    for (int upg = 0; upg < 8; upg++) {
        u32 now = (gSaveContext.inventory.upgrades & gUpgradeMasks[upg]) >> gUpgradeShifts[upg];
        u32 was = (before.upgrades & gUpgradeMasks[upg]) >> gUpgradeShifts[upg];
        if (now > was && was != 0)
            got.push_back(kUpgradeNames[upg]);
    }
    for (int quest = QUEST_MEDALLION_FOREST; quest <= QUEST_GERUDO_CARD; quest++) {
        u32 bit = 1u << quest;
        if ((gSaveContext.inventory.questItems & bit) && !(before.questItems & bit))
            got.push_back(SohUtils::GetQuestItemName(quest));
    }
    int hearts = (gSaveContext.healthCapacity - before.healthCapacity) / 16;
    if (hearts > 0)
        got.push_back(std::to_string(hearts) + (hearts == 1 ? " heart container" : " heart containers"));
    if (gSaveContext.magicLevel > before.magicLevel)
        got.push_back(before.magicLevel == 0 ? "Magic meter" : "Bigger magic meter");

    if (got.empty())
        return false;
    m_coopJoinList = got;
    m_coopJoinFrames = kJoinSummaryFrames;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame update, scene and save changes
// ---------------------------------------------------------------------------------------------------------------

void ZeldaOnlineClient::CoopOnSaveChanged(bool loaded) {
    m_coopIsDowned = false;
    m_coopDowned.clear();
    m_coopMenuOpen = false;
    m_coopDarkLink = nullptr;
    m_coopDarkLinkSpawned = false;
    m_coopJoinList.clear();
    m_coopJoinFrames = 0;
    m_coopJoinArmed = loaded;
    m_coopJoinArmedFrames = 0;
    m_coopGroundScene = -1;
    if (loaded)
        CoopLoadMarker();
    else
        m_coopMarker = CoopMarker();
}

void ZeldaOnlineClient::CoopOnSceneInit() {
    m_coopDowned.clear();
    m_coopIsDowned = false;
    m_coopDarkLink = nullptr;
    m_coopDarkLinkSpawned = false;
    m_coopMenuOpen = false;
    if (StoryCoopActive() && m_partySettings.deathPenalty != DEATH_PENALTY_OFF && m_coopMarker.has && gPlayState &&
        m_coopMarker.scene == gPlayState->sceneNum && m_coopMarker.age == gSaveContext.linkAge)
        CoopShowNotice("Your " + std::to_string(m_coopMarker.rupees) + " lost rupees are somewhere here. Look for " +
                           "the sparkles.",
                       5);
}

void ZeldaOnlineClient::CoopUpdate() {
    m_coopFfHit = false;
    m_coopHint.clear();
    if (m_coopNoticeFrames > 0)
        m_coopNoticeFrames--;
    if (m_coopJoinFrames > 0)
        m_coopJoinFrames--;
    // joining a party also arms the join summary (the first team progress that adds something is listed)
    if (m_partyID != m_coopJoinParty) {
        m_coopJoinParty = m_partyID;
        if (m_partyID != 0) {
            m_coopJoinArmed = true;
            m_coopJoinArmedFrames = 0;
        }
    }

    PlayState* play = gPlayState;
    if (!CoopRulesActive()) {
        if (m_coopIsDowned && play != nullptr && !isConnected)
            CoopEndDowned(true);
        m_coopIsDowned = false;
        return;
    }
    m_coopFrame++;

    if (m_coopJoinArmed && ++m_coopJoinArmedFrames > 120 * kFps)
        m_coopJoinArmed = false;

    // a ready check vote this player was asked in is over
    if (!m_storyWarpVoting && !m_storyWarpHold) {
        if (m_coopReadyVote && m_coopReadyWasVoting && m_storyWarpJoinFrames > 0) {
            // ready and the leader went in: walking in after them does not ask again
            m_coopReadyPass = kReadyPassFrames;
            m_storyWarpJoinFrames = 0;
        }
        m_coopReadyVote = false;
    }
    m_coopReadyWasVoting = m_storyWarpVoting;
    if (!m_storyWarpHold && m_coopReadyHold) {
        m_coopReadyHold = false;
        m_coopReadyPass = kReadyPassFrames;
    }

    Player* player = GET_PLAYER(play);
    if (player == nullptr)
        return;

    CoopUpdateDowned(play, player);
    if (m_partySettings.deathPenalty != DEATH_PENALTY_OFF)
        CoopUpdateMarker(play, player);

    // what the player can do for a downed teammate close by
    if (!m_coopIsDowned && !m_coopDowned.empty()) {
        uint32_t nearest = 0;
        float nearestDist = 1e9f;
        for (auto& mate : m_coopDowned) {
            AbstractActorController* c = GetNetworkController((int)mate.first);
            if (c == nullptr || c->GetActor() == nullptr)
                continue;
            float d = Dist3(player->actor.world.pos, c->GetActor()->world.pos);
            if (d < nearestDist) {
                nearestDist = d;
                nearest = mate.first;
            }
        }
        if (nearest != 0) {
            const int seconds = m_coopDowned[nearest].seconds;
            std::string how;
            if (m_partySettings.songRevive && m_partySettings.bottleRevive)
                how = "play Zelda's Lullaby or use a fairy or potion";
            else if (m_partySettings.songRevive)
                how = "play Zelda's Lullaby";
            else
                how = "use a fairy or potion";
            m_coopHint = MemberName(nearest) + " is down (" + std::to_string(seconds) + "s). " +
                         (nearestDist <= kSongReach ? "Revive: " + how + " now." : "Get close, then " + how + ".");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------------------------

void ZeldaOnlineClient::CoopDrawOverlay(GraphicsContext* gfxCtx) {
    PlayState* play = gPlayState;
    if (play == nullptr || gSaveContext.fileNum == 0xFF)
        return;

    const Color_RGBA8 white = { 255, 255, 255, 255 };
    const Color_RGBA8 dim = { 150, 150, 150, 255 };
    const Color_RGBA8 yellow = { 255, 230, 110, 255 };
    const Color_RGBA8 red = { 255, 110, 110, 255 };
    const Color_RGBA8 panel = { 0, 0, 0, 170 };
    const float small = 0.45f;
    const s16 lineH = (s16)(16.0f * small);

    // this player is down
    if (m_coopIsDowned) {
        const int seconds = (m_coopDownedFrames + kFps - 1) / kFps;
        std::string text =
            Wrap("You are down. A teammate can revive you: " + std::to_string(seconds) + "s left. Hold B to give up.");
        const s16 h = (s16)(LineCount(text) * lineH + 8);
        FillRect(gfxCtx, 8, 136, 304, h, panel);
        DrawScreenText(gfxCtx, text.c_str(), 14, 140, red, small);
    } else if (!m_coopHint.empty()) {
        std::string text = Wrap(m_coopHint);
        const s16 h = (s16)(LineCount(text) * lineH + 8);
        FillRect(gfxCtx, 8, 136, 304, h, panel);
        DrawScreenText(gfxCtx, text.c_str(), 14, 140, yellow, small);
    }

    // the respawn cooldown after a Game Over
    if (play->gameOverCtx.state == GAMEOVER_DEATH_DELAY_MENU && CoopRespawnCooldownFrames() > 0 && gGameOverTimer > 0) {
        std::string text = "Respawn in " + std::to_string((gGameOverTimer + kFps - 1) / kFps);
        float w = TextWidth(text, 0.7f);
        DrawScreenText(gfxCtx, text.c_str(), (s16)(160 - w / 2), 150, white, 0.7f);
    }

    // a short message
    if (m_coopNoticeFrames > 0 && !m_coopNotice.empty()) {
        std::string text = Wrap(m_coopNotice);
        u8 alpha = (u8)std::min(255, m_coopNoticeFrames * 25);
        const s16 h = (s16)(LineCount(text) * lineH + 8);
        const s16 y = (s16)(118 - h);
        FillRect(gfxCtx, 8, y, 304, h, { 0, 0, 0, (u8)(alpha * 2 / 3) });
        DrawScreenText(gfxCtx, text.c_str(), 14, (s16)(y + 4), { 255, 255, 255, alpha }, small);
    }

    // what a joining player received
    if (m_coopJoinFrames > 0 && !m_coopJoinList.empty()) {
        const size_t maxRows = 9;
        std::vector<std::string> rows;
        for (size_t i = 0; i < m_coopJoinList.size() && i < maxRows; i++)
            rows.push_back(m_coopJoinList[i]);
        if (m_coopJoinList.size() > maxRows)
            rows.back() = "and " + std::to_string(m_coopJoinList.size() - maxRows + 1) + " more";
        const s16 rowH = 9;
        const s16 h = (s16)(16 + rows.size() * rowH + 6);
        const s16 x = 196, y = 64, w = 118; // below the C buttons, above the minimap
        FillRect(gfxCtx, x, y, w, h, panel);
        DrawScreenText(gfxCtx, "You got from the team:", x + 5, y + 4, yellow, small);
        for (size_t i = 0; i < rows.size(); i++) {
            std::string row = rows[i];
            while (row.size() > 4 && TextWidth(row, small) > w - 10)
                row = row.substr(0, row.size() - 4) + "...";
            DrawScreenText(gfxCtx, row.c_str(), x + 5, (s16)(y + 16 + i * rowH), white, small);
        }
    }

    // the Co-op menu and its hint on the pause screen
    if (m_partyID != 0 && isConnected && PauseMenuIdle(play)) {
        if (!m_coopMenuOpen) {
            const char* hint = "L: Co-op menu";
            // above the rupee counter
            FillRect(gfxCtx, 8, 186, (s16)(TextWidth(hint, 0.5f) + 10), 13, panel);
            DrawScreenText(gfxCtx, hint, 13, 188, yellow, 0.5f);
        } else {
            std::vector<CoopMenuRow> rows = BuildMenuRows();
            std::vector<std::string> status;
            for (auto& mate : m_coopDowned)
                status.push_back(MemberName(mate.first) + " is down (" + std::to_string(mate.second.seconds) + "s)");

            const float scale = 0.55f;
            const s16 rowH = 13;
            const s16 w = 220;
            const s16 h = (s16)(26 + rows.size() * rowH + (status.empty() ? 0 : 4 + status.size() * 10) + 18);
            const s16 x = 50;
            const s16 y = (s16)std::max(20, 120 - h / 2);
            FillRect(gfxCtx, x, y, w, h, { 0, 0, 0, 205 });
            FillRect(gfxCtx, x, y, w, 2, { 255, 230, 110, 255 });
            DrawScreenText(gfxCtx, "Co-op", x + 8, y + 6, yellow, 0.65f);
            s16 rowY = (s16)(y + 24);
            for (size_t i = 0; i < rows.size(); i++) {
                const bool selected = (int)i == m_coopMenuCursor;
                if (selected)
                    FillRect(gfxCtx, x + 4, (s16)(rowY - 2), w - 8, rowH, { 80, 80, 140, 220 });
                std::string label = rows[i].label;
                while (label.size() > 4 && TextWidth(label, scale) > w - 30)
                    label = label.substr(0, label.size() - 4) + "...";
                DrawScreenText(gfxCtx, selected ? ">" : " ", x + 8, rowY, white, scale);
                DrawScreenText(gfxCtx, label.c_str(), x + 18, rowY, rows[i].enabled ? white : dim, scale);
                rowY += rowH;
            }
            if (rows.size() == 1)
                DrawScreenText(gfxCtx, "No teammates online yet.", x + 18, (s16)(rowY - rowH * 2 + 2), dim, small);
            if (!status.empty()) {
                rowY += 4;
                for (const std::string& s : status) {
                    DrawScreenText(gfxCtx, s.c_str(), x + 18, rowY, red, small);
                    rowY += 10;
                }
            }
            DrawScreenText(gfxCtx, "A: choose   B or L: back", x + 8, (s16)(y + h - 13), dim, small);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

void ZeldaOnlineClient::CoopRegisterHooks(bool enabled) {
    COND_HOOK(OnOcarinaSongAction, enabled, [] { CoopOnSongPlayed(); });
    COND_HOOK(OnGameStateMainStart, enabled, [this] { CoopMenuInput(); });
}

} // namespace ZeldaOnline

// ---------------------------------------------------------------------------------------------------------------
// Entry points for the game's C code
// ---------------------------------------------------------------------------------------------------------------

using ZeldaOnline::ZeldaOnlineClient;

extern "C" s32 ZeldaOnline_FriendlyFireHit(Player* player, Actor* attacker) {
    auto* client = ZeldaOnlineClient::Instance;
    if (client == nullptr || !client->isConnected)
        return 1;
    return client->CoopFriendlyFireHit(player, attacker) ? 1 : 0;
}

extern "C" s32 ZeldaOnline_EnterDowned(PlayState* play, Player* player) {
    auto* client = ZeldaOnlineClient::Instance;
    if (client == nullptr || !client->isConnected)
        return 0;
    return client->CoopEnterDowned(play, player) ? 1 : 0;
}

extern "C" s32 ZeldaOnline_GiveBottle(PlayState* play, Player* player, s32 itemAction) {
    auto* client = ZeldaOnlineClient::Instance;
    if (client == nullptr || !client->isConnected)
        return 0;
    return client->CoopGiveBottle(play, player, itemAction) ? 1 : 0;
}

extern "C" void ZeldaOnline_OnGameOver(PlayState* play) {
    auto* client = ZeldaOnlineClient::Instance;
    if (client != nullptr && client->isConnected)
        client->CoopOnGameOver(play);
}

extern "C" s16 ZeldaOnline_RespawnCooldownFrames(void) {
    auto* client = ZeldaOnlineClient::Instance;
    if (client == nullptr || !client->isConnected)
        return 0;
    return client->CoopRespawnCooldownFrames();
}
