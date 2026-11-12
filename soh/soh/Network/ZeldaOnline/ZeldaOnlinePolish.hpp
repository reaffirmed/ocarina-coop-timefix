#ifndef ZELDAONLINE_POLISH_HPP
#define ZELDAONLINE_POLISH_HPP

// Co-op presentation: what the players see and hear around the network play, kept apart from the sync code.
//
// - Connection notices with the other notifications: "Connection lost, reconnecting..." and "Back online", in place of
//   the old "Connected" / "Disconnected" corner text.
// - Messages that used to open a system dialog (kicked, wrong server version, a game that does not match the party)
//   are shown in the game's own text box while playing, or in a panel on the title and file select screens.
// - The time travel vote, the "continue the story" vote and the cutscene countdown are one short framed line at the
//   bottom centre, with the button icons of the game's font.
// - "Zelda found the Hookshot" with the item's icon, in place of "Party progress received".
// - Sounds from the game for co-op moments: a friend joins or leaves, says ready, everyone is ready, the countdown.
// - The party admin's game (vanilla, Master Quest, or which randomizer seed) travels in the party settings, and a
//   member whose loaded save does not match is told so.

#include <cstdint>
#include <string>
#include "StoryProgress.hpp"

struct GraphicsContext;

namespace ZeldaOnline {

class ZeldaOnlineClient;

struct CoopPolish {
    // Hooks of its own (overlay drawing, text box, per frame), registered once. They stay on while disconnected, so the
    // reconnect notices and a kick message still show.
    static void EnsureHooks();

    // Connection
    static void OnConnected(bool reconnected);
    static void OnDisconnected(bool willReconnect);
    static void OnConnectFailed();

    // A message for the player: the game's text box while playing, a panel elsewhere. `title` is a short heading.
    static void ShowMessage(const std::string& title, const std::string& body);

    // Story Co-op progress arrived from the party: tells the player what is new and who found it.
    static void AnnounceProgress(const StoryProgress& before, const StoryProgress& after);
    // An item another member found was already announced (opening a shared chest): do not announce it twice.
    static void NoteAnnounced(int itemId);

    // Party membership
    static void OnMemberJoined(uint32_t networkID);
    static void OnMemberLeft(uint32_t networkID);

    // The admin's game, carried in the party settings JSON (key "hostGame")
    static std::string LocalGame();
    static std::string HostGame();
    static void SetHostGame(const std::string& game);
    static std::string DescribeGame(const std::string& game);

    // Names a party member for messages: the one other member's name, or "Your party" with more
    static std::string PartnerLabel();

    // One framed prompt: a yellow heading, wrapped text, an optional bar, and button hints along the bottom
    struct PromptText {
        std::string title;
        std::string count; // "1/2 ready"
        std::string hints; // with the font's button icons
        float bar = -1.0f; // 0..1, -1 for none
    };

  private:
    static bool BuildPrompt(ZeldaOnlineClient* c, PromptText& p);
    static void UpdateConnectionNotes();
    static void ClearConnectionNotes();
    static void UpdateCues(ZeldaOnlineClient* c);
    static void UpdateGameCheck(ZeldaOnlineClient* c);
    static void OnFrame();
    static void OnDraw(GraphicsContext* gfxCtx);
};

} // namespace ZeldaOnline

#endif
