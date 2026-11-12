// Co-op Lobby: starts OOTSteamLink hidden and follows its status file (see the --status-file comment in
// tools/steamlink/ootsteamlink.c). Kept apart from the game headers because it needs windows.h.
#include "ZeldaOnlineLobby.hpp"

#include <ship/Context.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ZeldaOnline {

#ifdef _WIN32
static const char* kHelperName = "OOTSteamLink.exe";
#else
static const char* kHelperName = "OOTSteamLink";
#endif

bool CoopLobbyHelper::Supported() {
#if defined(_WIN32) || defined(__linux__)
    return true;
#else
    return false; // OOTSteamLink is built for Windows and Linux only
#endif
}

// The release puts OOTSteamLink next to the game (soh.exe, or soh.appimage on Linux).
std::string CoopLobbyHelper::FindExecutable() {
    if (!Supported())
        return "";
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    dirs.push_back(std::filesystem::current_path(ec));
    if (const char* appImage = std::getenv("APPIMAGE"))
        dirs.push_back(std::filesystem::path(appImage).parent_path());
    dirs.push_back(std::filesystem::path(Ship::Context::GetAppBundlePath()));
    for (const auto& dir : dirs) {
        for (const char* sub : { "", "steamlink" }) {
            std::filesystem::path candidate = dir / sub / kHelperName;
            if (std::filesystem::is_regular_file(candidate, ec))
                return std::filesystem::absolute(candidate, ec).string();
        }
    }
    return "";
}

CoopLobbyHelper::~CoopLobbyHelper() {
    // the helper also stops by itself when the game is gone (--parent-pid)
    Stop(false);
#ifdef _WIN32
    if (m_process != nullptr)
        CloseHandle((HANDLE)m_process);
#endif
}

bool CoopLobbyHelper::Running() const {
#ifdef _WIN32
    return m_process != nullptr && WaitForSingleObject((HANDLE)m_process, 0) == WAIT_TIMEOUT;
#else
    if (m_pid <= 0)
        return false;
    return waitpid(m_pid, nullptr, WNOHANG) == 0;
#endif
}

bool CoopLobbyHelper::Start(const std::vector<std::string>& args) {
    if (Running())
        Stop(true);

    state = State::Starting;
    hosting = false;
    port = 0;
    hostName.clear();
    error.clear();
    hostGone = false;
    friends.clear();
    peers.clear();
    m_friendsBuilding.clear();
    m_friendsExpected = -1;
    m_readOffset = 0;
    m_partialLine.clear();

    const std::string exe = FindExecutable();
    if (exe.empty()) {
        state = State::Failed;
        error = Supported() ? std::string(kHelperName) + " was not found next to the game. It comes with the co-op "
                                                         "download; put it in the game's folder."
                            : "Hosting and joining over Steam is not available on this system. Use Connect by address.";
        return false;
    }

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(Ship::Context::GetPathRelativeToAppDirectory("coop-lobby"));
    std::filesystem::create_directories(dir, ec);
    m_statusPath = (dir / "status.txt").string();
    for (const char* suffix : { "", ".pick", ".stop" })
        std::filesystem::remove(m_statusPath + suffix, ec);
    { std::ofstream create(m_statusPath, std::ios::trunc); }

    std::vector<std::string> argv = args;
    argv.push_back("--status-file");
    argv.push_back(m_statusPath);
    argv.push_back("--parent-pid");
#ifdef _WIN32
    argv.push_back(std::to_string(GetCurrentProcessId()));
#else
    argv.push_back(std::to_string(getpid()));
#endif
    const std::string helperDir = std::filesystem::path(exe).parent_path().string();
    const std::string logPath = (dir / "OOTSteamLink.log").string();

#ifdef _WIN32
    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : argv)
        cmd += " \"" + a + "\"";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = si.hStdError = log;
    }
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    // CREATE_NO_WINDOW: a hidden console, which OOTServer shares so the helper can stop it cleanly with Ctrl+Break
    BOOL ok = CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
                             CREATE_NO_WINDOW, nullptr, helperDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (!ok) {
        state = State::Failed;
        error = "Could not start " + std::string(kHelperName) + " (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    if (m_process != nullptr)
        CloseHandle((HANDLE)m_process);
    m_process = pi.hProcess;
#else
    // everything the child needs is prepared before fork(): the game has other threads, so the child must not allocate
    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>(exe.c_str()));
    for (auto& a : argv)
        cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t child = fork();
    if (child < 0) {
        state = State::Failed;
        error = "Could not start " + std::string(kHelperName) + ".";
        return false;
    }
    if (child == 0) {
        setsid(); // not in the game's process group: a Ctrl+C in the game's terminal is not for the helper
        int null = open("/dev/null", O_RDONLY);
        int out = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (null >= 0)
            dup2(null, 0);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
        }
        for (int fd = 3; fd < 1024; fd++)
            close(fd); // the game's sockets and files stay with the game
        signal(SIGPIPE, SIG_DFL);
        if (chdir(helperDir.c_str()) == 0)
            execv(exe.c_str(), cargv.data());
        _exit(127);
    }
    m_pid = child;
#endif
    return true;
}

void CoopLobbyHelper::Pick(uint64_t lobby) {
    if (m_statusPath.empty())
        return;
    std::ofstream pick(m_statusPath + ".pick", std::ios::trunc);
    pick << lobby;
}

void CoopLobbyHelper::Stop(bool wait) {
    if (!m_statusPath.empty() && Running()) {
        std::ofstream stop(m_statusPath + ".stop", std::ios::trunc);
    }
    if (wait) {
        // the host's server writes the team save before it exits
        for (int i = 0; i < 150 && Running(); i++) {
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100 * 1000);
#endif
        }
    }
    if (state != State::Failed)
        state = State::Stopped;
}

void CoopLobbyHelper::Poll() {
    if (m_statusPath.empty() || state == State::Idle)
        return;
    const bool alive = Running(); // checked before reading, so the last lines of a helper that just exited are seen
    std::ifstream in(m_statusPath, std::ios::binary);
    if (in) {
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size > (std::streamoff)m_readOffset) {
            in.seekg((std::streamoff)m_readOffset);
            std::string chunk((size_t)(size - (std::streamoff)m_readOffset), '\0');
            in.read(&chunk[0], (std::streamsize)chunk.size());
            chunk.resize((size_t)in.gcount());
            m_readOffset += chunk.size();
            m_partialLine += chunk;
            size_t nl;
            while ((nl = m_partialLine.find('\n')) != std::string::npos) {
                std::string line = m_partialLine.substr(0, nl);
                m_partialLine.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                HandleLine(line);
            }
        }
    }
    if (state != State::Failed && state != State::Stopped && !alive) {
        // gone without saying why (killed, or crashed)
        state = State::Failed;
        if (error.empty())
            error = std::string(kHelperName) + " stopped unexpectedly. Is Steam running and logged in?";
    }
}

static std::string Rest(const std::string& line, size_t words) {
    size_t pos = 0;
    for (size_t i = 0; i < words; i++) {
        pos = line.find(' ', pos);
        if (pos == std::string::npos)
            return "";
        pos++;
    }
    return line.substr(pos);
}

void CoopLobbyHelper::HandleLine(const std::string& line) {
    auto starts = [&](const char* prefix) { return line.rfind(prefix, 0) == 0; };
    if (starts("steam ")) {
        steamName = Rest(line, 1);
    } else if (starts("friends ")) {
        m_friendsExpected = std::atoi(Rest(line, 1).c_str());
        m_friendsBuilding.clear();
        if (m_friendsExpected <= 0) {
            friends.clear();
            m_friendsExpected = -1;
        }
        if (state == State::Starting)
            state = State::Picking;
    } else if (starts("friend ")) {
        Friend f;
        f.lobby = std::strtoull(Rest(line, 1).c_str(), nullptr, 10);
        f.name = Rest(line, 2);
        m_friendsBuilding.push_back(f);
        if (m_friendsExpected >= 0 && (int)m_friendsBuilding.size() >= m_friendsExpected) {
            friends = m_friendsBuilding;
            m_friendsExpected = -1;
        }
    } else if (starts("ready host ")) {
        hosting = true;
        port = std::atoi(Rest(line, 2).c_str());
        state = State::Ready;
    } else if (starts("ready join ")) {
        hosting = false;
        port = std::atoi(Rest(line, 2).c_str());
        hostName = Rest(line, 3);
        state = State::Ready;
    } else if (starts("peer+ ")) {
        peers.push_back(Rest(line, 1));
    } else if (starts("peer- ")) {
        const std::string name = Rest(line, 1);
        for (auto it = peers.begin(); it != peers.end(); ++it) {
            if (*it == name) {
                peers.erase(it);
                break;
            }
        }
    } else if (starts("hostgone")) {
        hostGone = true;
    } else if (starts("error ")) {
        error = Rest(line, 1);
        state = State::Failed;
    } else if (starts("stopped")) {
        if (state != State::Failed)
            state = State::Stopped;
    }
}

} // namespace ZeldaOnline
