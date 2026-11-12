#ifndef PARTYSETTINGSH
#define PARTYSETTINGSH

#include <string>
#include <random>
#include <nlohmann/json.hpp>

namespace ZeldaOnline {
    struct PartySettings {
        bool privateDungeons = false;
        bool extraEnemies = false;
        float extraEnemyWeight = 1.0f;
        bool healthMultiplier = false;
        float enemyHealthWeight = 1.0f;
        float bossHealthWeight = 1.0f;
        bool damageMultiplier = false;
        float enemyDamageWeight = 1.0f;
        bool storyCoop = false;
        bool shareAmmoRupeesHp = false;
        // Optional label that picks the server side team save together with the admin nickname (max 32 characters).
        std::string storyCode;

        // Co-op rules (ZeldaOnlineCoopRules.cpp). Every one is off by default, so a party that never sets them plays
        // exactly as before. They only take effect while Story Co-op is on.
        int friendlyFire = 0;      // FRIENDLY_FIRE_*: teammates' swords, arrows, seeds and boomerangs
        int deathPenalty = 0;      // DEATH_PENALTY_*: what a Game Over leaves behind
        int rupeesLost = 2;        // RUPEES_LOST_*: the share of rupees left in the death marker
        int darkLinkStrength = 0;  // 0 weaker (less health), 1 as strong as in the Water Temple
        bool songRevive = false;   // a teammate plays Zelda's Lullaby next to a downed player
        bool bottleRevive = false; // a teammate uses a fairy, potion or milk on a downed or hurt player
        int downedSeconds = 30;    // how long a downed player waits for a revive (30 or 60)
        int respawnCooldown = 0;   // seconds before the Game Over menu after a death (0, 5 or 10)
        bool readyCheck = false;   // wait for the others at dungeon entrances and boss doors

        bool DownedEnabled() const {
            return songRevive || bottleRevive;
        }
    };

    enum { FRIENDLY_FIRE_OFF = 0, FRIENDLY_FIRE_LIGHT = 1, FRIENDLY_FIRE_FULL = 2 };
    enum { DEATH_PENALTY_OFF = 0, DEATH_PENALTY_MARKER = 1, DEATH_PENALTY_DARK_LINK = 2 };
    enum { RUPEES_LOST_NONE = 0, RUPEES_LOST_QUARTER = 1, RUPEES_LOST_HALF = 2, RUPEES_LOST_ALL = 3 };

    // Clamps the co-op rules to the values the menus offer (settings come from the network and from disk).
    inline void ClampCoopRules(PartySettings& s) {
        auto clampInt = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
        s.friendlyFire = clampInt(s.friendlyFire, 0, 2);
        s.deathPenalty = clampInt(s.deathPenalty, 0, 2);
        s.rupeesLost = clampInt(s.rupeesLost, 0, 3);
        s.darkLinkStrength = clampInt(s.darkLinkStrength, 0, 1);
        s.downedSeconds = s.downedSeconds >= 45 ? 60 : 30;
        s.respawnCooldown = s.respawnCooldown >= 8 ? 10 : (s.respawnCooldown >= 3 ? 5 : 0);
    }

    // The co-op rules in the party settings JSON (sent to the server, which keeps and relays it, and stored per
    // playthrough in the lobby list). Missing keys keep their defaults, so older settings read as everything off.
    inline void WriteCoopRulesJSON(const PartySettings& s, nlohmann::json& j) {
        j["friendlyFire"] = s.friendlyFire;
        j["deathPenalty"] = s.deathPenalty;
        j["rupeesLost"] = s.rupeesLost;
        j["darkLinkStrength"] = s.darkLinkStrength;
        j["songRevive"] = s.songRevive;
        j["bottleRevive"] = s.bottleRevive;
        j["downedSeconds"] = s.downedSeconds;
        j["respawnCooldown"] = s.respawnCooldown;
        j["readyCheck"] = s.readyCheck;
    }

    inline void ReadCoopRulesJSON(const nlohmann::json& j, PartySettings& s) {
        if (!j.is_object())
            return;
        auto readInt = [&](const char* key, int fallback) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<int>() : fallback;
        };
        auto readBool = [&](const char* key, bool fallback) {
            auto it = j.find(key);
            return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
        };
        PartySettings d;
        s.friendlyFire = readInt("friendlyFire", d.friendlyFire);
        s.deathPenalty = readInt("deathPenalty", d.deathPenalty);
        s.rupeesLost = readInt("rupeesLost", d.rupeesLost);
        s.darkLinkStrength = readInt("darkLinkStrength", d.darkLinkStrength);
        s.songRevive = readBool("songRevive", d.songRevive);
        s.bottleRevive = readBool("bottleRevive", d.bottleRevive);
        s.downedSeconds = readInt("downedSeconds", d.downedSeconds);
        s.respawnCooldown = readInt("respawnCooldown", d.respawnCooldown);
        s.readyCheck = readBool("readyCheck", d.readyCheck);
        ClampCoopRules(s);
    }

    // A fresh random team code (8 characters, no look-alike letters). Story Co-op needs one so that two parties whose
    // admins share a nickname never share a server side team save.
    inline std::string GenerateStoryCode() {
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        std::random_device rd;
        std::string code;
        for (int i = 0; i < 8; i++)
            code += alphabet[rd() % (sizeof(alphabet) - 1)];
        return code;
    }
};
#endif
