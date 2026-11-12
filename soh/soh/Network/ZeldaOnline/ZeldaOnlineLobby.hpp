#ifndef ZELDAONLINELOBBYH
#define ZELDAONLINELOBBYH
// Co-op Lobby: host or join a Story Co-op game from inside the game, and pick which team playthrough to continue.
//
// The lobby drives OOTSteamLink (tools/steamlink) in its --status-file mode: the helper runs hidden, starts OOTServer
// and the Steam lobby (host) or the relay to a friend (join), and reports what it does through a text file. Once the
// helper is ready the game connects to it, and ZeldaOnlineClient forms the party by itself: the host creates a Story
// Co-op party with the playthrough's team code and invites everybody who connects, the others accept that invite.
#include <ship/window/gui/GuiWindow.h>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace ZeldaOnline {

// One team playthrough: the team code picks the server side team save (together with the host's nickname).
struct CoopPlaythrough {
    std::string name;
    std::string code;
    int64_t created = 0;
    int64_t lastPlayed = 0;
    int lastFile = -1;       // save file used last time (0 based), -1 unknown
    std::string lastPlayers; // who played last time, for display only
    std::string hostNick;    // the nickname it was hosted under: the team save is stored under it
};

// Playthroughs this player hosted, plus the save file they used last with each friend they joined.
class CoopPlaythroughList {
  public:
    std::vector<CoopPlaythrough> hosted;
    struct Joined {
        std::string host;
        int lastFile = -1;
        int64_t lastPlayed = 0;
    };
    std::vector<Joined> joined;

    void Load();
    void Save() const;
    CoopPlaythrough* FindByCode(const std::string& code);
    Joined& JoinedEntry(const std::string& host);
    void SortByLastPlayed();
};

// The hidden OOTSteamLink process and its status file.
class CoopLobbyHelper {
  public:
    struct Friend {
        uint64_t lobby = 0;
        std::string name;
    };

    enum class State { Idle, Starting, Picking, Ready, Failed, Stopped };

    ~CoopLobbyHelper();

    static std::string FindExecutable();
    static bool Supported();

    bool Start(const std::vector<std::string>& args);
    void Pick(uint64_t lobby);
    void Stop(bool wait);
    void Poll();
    bool Running() const;

    State state = State::Idle;
    bool hosting = false;
    int port = 0;
    std::string steamName;
    std::string hostName; // join: the friend who hosts
    std::string error;
    bool hostGone = false;
    std::vector<Friend> friends;
    std::vector<std::string> peers; // host: friends connected through Steam

  private:
    std::string m_statusPath;
    size_t m_readOffset = 0;
    std::string m_partialLine;
    std::vector<Friend> m_friendsBuilding;
    int m_friendsExpected = -1;
#ifdef _WIN32
    void* m_process = nullptr;
#else
    int m_pid = 0;
#endif
    void HandleLine(const std::string& line);
};

class CoopLobbyWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    inline static CoopLobbyWindow* Instance;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override {
    }
    void Draw() override;

    void Open();
    // the client lost or ended its lobby connection
    void OnClientLeftLobby();
    // stops hosting or joining (the server saves the team progress)
    void StopAll();

  private:
    enum class Page { Home, HostPick, Session };
    Page m_page = Page::Home;
    CoopLobbyHelper m_helper;
    CoopPlaythroughList m_list;
    bool m_listLoaded = false;
    int m_selected = -1;
    char m_newName[48] = "";
    char m_renameBuffer[48] = "";
    int m_renaming = -1;
    int m_confirmForget = -1;
    char m_nameBuffer[33] = "";
    std::string m_activeCode;  // host: the playthrough being played
    std::string m_connectedTo; // join: the friend's name
    bool m_clientStarted = false;
    bool m_autoShown = false; // shown by the title screen check, hidden again when the game starts
    bool m_wasInGame = false;
    bool m_seenTitle = false; // the title screen check already ran for this visit
    bool m_stopping = false;
    double m_lastPoll = 0.0;
    int m_lastRecordedFile = -2;
    std::string m_message;

    void EnsureListLoaded();
    void DrawHome();
    void DrawHostPick();
    void DrawSession();
    void DrawJoinPick();
    void DrawNicknameField();
    void DrawPlaythroughRow(int index);

    void StartHosting(int index);
    void StartJoining();
    void ConnectClient();
    void RecordSaveFile();
    void UpdateAutoShow();
};

} // namespace ZeldaOnline
#endif
