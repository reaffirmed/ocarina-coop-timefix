#include "UpdaterCore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Updater {

// ------------------------------------------------------------------------------------------------- versions

std::string Version::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

Version ParseVersion(const std::string& text) {
    Version v;
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V'))
        i++;
    int parts[3] = { 0, 0, 0 };
    for (int p = 0; p < 3; p++) {
        if (p > 0) {
            if (i >= text.size() || text[i] != '.')
                return Version{};
            i++;
        }
        size_t start = i;
        long long value = 0;
        while (i < text.size() && std::isdigit((unsigned char)text[i]) && i - start < 6)
            value = value * 10 + (text[i++] - '0');
        if (i == start || (i < text.size() && std::isdigit((unsigned char)text[i])))
            return Version{};
        parts[p] = (int)value;
    }
    if (i != text.size())
        return Version{};
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.valid = true;
    return v;
}

int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    return 0;
}

Platform CurrentPlatform() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return Platform::Windows;
#elif defined(__linux__) && defined(__x86_64__)
    return Platform::Linux;
#else
    return Platform::Unsupported;
#endif
}

// ------------------------------------------------------------------------------------------------- release

namespace {

bool EndsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}
bool StartsWith(const std::string& s, const std::string& start) {
    return s.compare(0, start.size(), start) == 0;
}

// a plain file name: letters, digits, . - _ (what the release workflow makes)
bool PlainName(const std::string& name) {
    if (name.empty() || name.size() > 128 || name[0] == '.')
        return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_')
            return false;
    return true;
}

bool PackageNameFor(Platform platform, const std::string& name) {
    if (!StartsWith(name, "OcarinaStoryCoop-"))
        return false;
    switch (platform) {
        case Platform::Windows:
            return EndsWith(name, "-windows.zip");
        case Platform::Linux:
            return EndsWith(name, "-linux.tar.gz");
        default:
            return false;
    }
}

} // namespace

bool ParseRelease(const std::string& text, Platform platform, Release& out, std::string& error) {
    out = Release{};
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "GitHub sent an answer that is not a release";
        return false;
    }
    if (j.value("draft", false) || j.value("prerelease", false)) {
        error = "the newest release is a draft or pre-release";
        return false;
    }
    const auto tagIt = j.find("tag_name");
    if (tagIt == j.end() || !tagIt->is_string()) {
        error = "the release has no tag";
        return false;
    }
    out.tag = tagIt->get<std::string>();
    out.version = ParseVersion(out.tag);
    if (!out.version.valid || out.tag[0] != 'v') {
        error = "the release tag " + out.tag + " is not a version like v1.2.3";
        return false;
    }
    const auto nameIt = j.find("name");
    out.title = (nameIt != j.end() && nameIt->is_string()) ? nameIt->get<std::string>() : out.tag;

    const auto assets = j.find("assets");
    if (assets == j.end() || !assets->is_array()) {
        error = "the release has no files";
        return false;
    }
    const std::string prefix = std::string(kDownloadPrefix) + out.tag + "/";
    int packages = 0;
    for (const auto& a : *assets) {
        if (!a.is_object() || !a.contains("name") || !a["name"].is_string() || !a.contains("browser_download_url") ||
            !a["browser_download_url"].is_string())
            continue;
        Asset asset;
        asset.name = a["name"].get<std::string>();
        asset.url = a["browser_download_url"].get<std::string>();
        if (a.contains("size") && a["size"].is_number_unsigned())
            asset.size = a["size"].get<uint64_t>();
        if (!PlainName(asset.name) || asset.url != prefix + asset.name)
            continue; // only files of this release of this repo
        if (asset.name == kSumsName) {
            out.sums = asset;
        } else if (PackageNameFor(platform, asset.name)) {
            out.package = asset;
            packages++;
        }
    }
    if (packages == 0) {
        error = "the release has no download for this system";
        return false;
    }
    if (packages > 1) {
        error = "the release has more than one download for this system";
        return false;
    }
    if (out.sums.name.empty()) {
        error = "the release has no SHA256SUMS.txt to check the download with";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------- SHA-256

namespace {

struct Sha256 {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t buf[64];
    size_t used = 0;
    uint64_t bits = 0;

    static uint32_t Rot(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void Block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Rot(w[i - 15], 7) ^ Rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rot(w[i - 2], 17) ^ Rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = Rot(e, 6) ^ Rot(e, 11) ^ Rot(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = Rot(a, 2) ^ Rot(a, 13) ^ Rot(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void Add(const uint8_t* p, size_t n) {
        bits += (uint64_t)n * 8;
        while (n > 0) {
            size_t take = std::min(n, sizeof(buf) - used);
            std::memcpy(buf + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == 64) {
                Block(buf);
                used = 0;
            }
        }
    }

    std::string Hex() {
        uint64_t total = bits;
        uint8_t pad = 0x80;
        Add(&pad, 1);
        uint8_t zero = 0;
        while (used != 56)
            Add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++)
            len[i] = (uint8_t)(total >> (56 - i * 8));
        Add(len, 8);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h)
            for (int i = 28; i >= 0; i -= 4)
                out += digits[(v >> i) & 0xf];
        return out;
    }
};

} // namespace

std::string Sha256Hex(const void* data, size_t size) {
    Sha256 s;
    s.Add((const uint8_t*)data, size);
    return s.Hex();
}

std::string Sha256File(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return "";
    Sha256 s;
    std::vector<char> chunk(1 << 16);
    while (in) {
        in.read(chunk.data(), (std::streamsize)chunk.size());
        std::streamsize got = in.gcount();
        if (got > 0)
            s.Add((const uint8_t*)chunk.data(), (size_t)got);
    }
    if (in.bad())
        return "";
    return s.Hex();
}

std::string FindSum(const std::string& sumsText, const std::string& fileName) {
    std::istringstream lines(sumsText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 66)
            continue;
        std::string hex = line.substr(0, 64);
        bool ok = true;
        for (char& c : hex) {
            c = (char)std::tolower((unsigned char)c);
            if (!std::isxdigit((unsigned char)c))
                ok = false;
        }
        if (!ok || line[64] != ' ')
            continue;
        std::string name = line.substr(65);
        if (!name.empty() && (name[0] == ' ' || name[0] == '*'))
            name = name.substr(1);
        if (name == fileName)
            return hex;
    }
    return "";
}

// ------------------------------------------------------------------------------------------------- processes

namespace {

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// one argument, quoted the way CommandLineToArgvW reads it back
void AppendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty())
        cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            slashes++;
        } else if (c == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
            cmd += c;
            slashes = 0;
        } else {
            cmd.append(slashes, L'\\');
            cmd += c;
            slashes = 0;
        }
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

bool Spawn(const std::vector<std::string>& args, const fs::path* workingDir, bool wait, int& exitCode) {
    std::wstring cmd;
    for (const auto& a : args)
        AppendQuoted(cmd, Wide(a));
    std::wstring exe = Wide(args[0]);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    DWORD flags = wait ? CREATE_NO_WINDOW : (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
    std::wstring cwd = workingDir ? workingDir->wstring() : L"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        workingDir ? cwd.c_str() : nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        exitCode = (int)code;
    }
    CloseHandle(pi.hProcess);
    return true;
}

std::string SystemTool(const char* name) {
    // the full System32 path: never a curl.exe / tar.exe that happens to sit next to the game
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return "";
    const fs::path path = fs::path(std::wstring(dir, n)) / (std::string(name) + ".exe");
    return fs::exists(path) ? PathUtf8(path) : "";
}
#else
bool Spawn(const std::vector<std::string>& args, const fs::path* workingDir, bool wait, int& exitCode) {
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (!wait) {
            setsid();
            if (fork() != 0)
                _exit(0); // the grandchild runs on its own, nothing waits for it
        }
        // Inside the AppImage the game may run with its bundled libraries first in the search path: the system's
        // curl and tar must use the system's own.
        if (std::getenv("APPDIR") != nullptr) {
            unsetenv("LD_LIBRARY_PATH");
            unsetenv("LD_PRELOAD");
        }
        if (workingDir != nullptr && chdir(workingDir->c_str()) != 0)
            _exit(127);
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return !(wait && exitCode == 127);
}

std::string SystemTool(const char* name) {
    for (const char* dir : { "/usr/bin/", "/bin/", "/usr/local/bin/" }) {
        std::string path = std::string(dir) + name;
        if (access(path.c_str(), X_OK) == 0)
            return path;
    }
    return "";
}
#endif

} // namespace

int RunProcess(const std::vector<std::string>& args) {
    int code = -1;
    if (args.empty() || !Spawn(args, nullptr, true, code))
        return -1;
    return code;
}

bool StartDetached(const std::vector<std::string>& args, const fs::path& workingDir) {
    int code = 0;
    return !args.empty() && Spawn(args, &workingDir, false, code);
}

// ------------------------------------------------------------------------------------------------- downloads

bool Download(const std::string& url, const fs::path& dest, uint64_t maxBytes, int maxSeconds, std::string& error) {
    if (!StartsWith(url, "https://")) {
        error = "not an https address";
        return false;
    }
    const std::string curl = SystemTool("curl");
    if (curl.empty()) {
        error = "curl was not found on this system";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::path part = dest;
    part += ".part";
    fs::remove(part, ec);
    // -q first: ignore any .curlrc. Only https, also for redirects (GitHub sends the file from its download host).
    std::vector<std::string> args = { curl,
                                      "-q",
                                      "--silent",
                                      "--show-error",
                                      "--fail",
                                      "--location",
                                      "--proto",
                                      "=https",
                                      "--proto-redir",
                                      "=https",
                                      "--max-redirs",
                                      "5",
                                      "--connect-timeout",
                                      "20",
                                      "--max-time",
                                      std::to_string(maxSeconds),
                                      "--max-filesize",
                                      std::to_string(maxBytes),
                                      "--user-agent",
                                      "OcarinaStoryCoop-Updater",
                                      "--header",
                                      "Accept: application/vnd.github+json, application/octet-stream",
                                      "--output",
                                      PathUtf8(part),
                                      url };
    int code = RunProcess(args);
    if (code != 0) {
        fs::remove(part, ec);
        switch (code) {
            case -1:
                error = "curl could not be started";
                break;
            case 6:
            case 7:
                error = "no connection to GitHub";
                break;
            case 22:
                error = "GitHub answered with an error";
                break;
            case 28:
                error = "the download took too long";
                break;
            case 63:
                error = "the download is bigger than expected";
                break;
            default:
                error = "the download failed (curl " + std::to_string(code) + ")";
        }
        return false;
    }
    uint64_t size = fs::file_size(part, ec);
    if (ec || size > maxBytes) {
        fs::remove(part, ec);
        error = "the download is bigger than expected";
        return false;
    }
    fs::remove(dest, ec);
    fs::rename(part, dest, ec);
    if (ec) {
        error = "the download could not be saved: " + ec.message();
        return false;
    }
    return true;
}

bool DownloadText(const std::string& url, const fs::path& scratch, std::string& out, std::string& error) {
    if (!Download(url, scratch, 1u << 20, 30, error))
        return false;
    std::ifstream in(scratch, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    in.close();
    std::error_code ec;
    fs::remove(scratch, ec);
    return true;
}

// ------------------------------------------------------------------------------------------------- install

std::string PathUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string ToRel(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

bool FindInstall(fs::path& root, std::string& gameFile) {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n == 0 || n >= buf.size())
        return false;
    buf.resize(n);
    fs::path exe(buf);
    gameFile = "soh.exe";
    if (exe.filename() != fs::path(gameFile))
        return false;
    root = exe.parent_path();
#elif defined(__linux__)
    // The release runs as soh.appimage; the AppImage runtime says where the file is.
    const char* appimage = std::getenv("APPIMAGE");
    if (appimage == nullptr || appimage[0] != '/')
        return false;
    fs::path file(appimage);
    gameFile = "soh.appimage";
    if (file.filename() != fs::path(gameFile))
        return false;
    root = file.parent_path();
#else
    return false;
#endif
    std::error_code ec;
    // a release folder: the game and the Steam helper next to it
    return fs::is_regular_file(root / gameFile, ec) && fs::is_directory(root / "server", ec);
}

namespace {

std::string Lower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> SplitPath(const std::string& rel) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

// player files the package may bring a first copy of, but must never overwrite
bool KeepIfPresent(const std::string& relPath) {
    const std::string ext = Lower(fs::path(relPath).extension().string());
    return ext == ".json" || ext == ".ini" || ext == ".cfg";
}

} // namespace

bool IsProtectedPath(const std::string& relPath) {
    if (relPath.empty() || relPath[0] == '/' || relPath[0] == '\\' || relPath.find(':') != std::string::npos)
        return true;
    const std::vector<std::string> parts = SplitPath(relPath);
    for (const auto& raw : parts) {
        const std::string part = Lower(raw);
        if (part.empty() || part == "." || part == "..")
            return true;
        // saves (the game's and the server's team saves), mods, logs, our own update folder
        static const char* kDirs[] = { "save", "saves", "story", "backup", "backups", "mods", "logs", "updates" };
        if (&raw != &parts.back())
            for (const char* d : kDirs)
                if (part == d)
                    return true;
    }
    const std::string name = Lower(parts.back());
    const std::string ext = Lower(fs::path(name).extension().string());
    // the ROM and anything made from it
    static const char* kRomExts[] = { ".z64", ".n64", ".v64", ".rom", ".otr", ".sav", ".srm" };
    for (const char* e : kRomExts)
        if (ext == e)
            return true;
    if (ext == ".o2r" && !(name == "soh.o2r" && parts.size() == 1))
        return true; // oot.o2r / oot-mq.o2r are built from the player's ROM
    // settings and the player's own lists
    static const char* kNames[] = { "shipofharkinian.json", "imgui.ini", "coop-playthroughs.json", "zo_warp.txt",
                                    "ootsteamlink.log" };
    for (const char* n : kNames)
        if (name == n)
            return true;
    return false;
}

bool UnpackPackage(const fs::path& archive, const fs::path& stagingDir, const std::string& gameFile,
                   fs::path& packageRoot, std::string& error) {
    std::error_code ec;
    fs::remove_all(stagingDir, ec);
    fs::create_directories(stagingDir, ec);
    if (ec) {
        error = "could not make " + PathUtf8(stagingDir);
        return false;
    }
    const std::string tar = SystemTool("tar");
    if (tar.empty()) {
        error = "tar was not found on this system";
        return false;
    }
#if defined(_WIN32)
    // Windows 10 and 11 come with bsdtar, which reads zip files
    std::vector<std::string> args = { tar, "-x", "-f", PathUtf8(archive), "-C", PathUtf8(stagingDir) };
#else
    std::vector<std::string> args = {
        tar, "-x", "-z", "-f", PathUtf8(archive), "-C", PathUtf8(stagingDir), "--no-same-owner"
    };
#endif
    if (RunProcess(args) != 0) {
        error = "the download could not be unpacked";
        return false;
    }
    // nothing but plain files and folders, all inside the staging folder
    const fs::path base = fs::weakly_canonical(stagingDir, ec);
    for (auto it = fs::recursive_directory_iterator(stagingDir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::file_status st = it->symlink_status(ec);
        if (fs::is_symlink(st) || !(fs::is_regular_file(st) || fs::is_directory(st))) {
            error = "the download holds a link or special file: " + PathUtf8(it->path().filename());
            return false;
        }
        const fs::path real = fs::weakly_canonical(it->path(), ec);
        const std::string relCheck = fs::relative(real, base, ec).generic_string();
        if (ec || relCheck.empty() || relCheck.rfind("..", 0) == 0) {
            error = "the download holds a file outside its folder";
            return false;
        }
    }
    if (ec) {
        error = "the unpacked download could not be read";
        return false;
    }
    if (fs::is_regular_file(stagingDir / gameFile, ec)) {
        packageRoot = stagingDir;
        return true;
    }
    // the tarball: one top folder
    fs::path only;
    int dirs = 0;
    for (const auto& e : fs::directory_iterator(stagingDir, ec)) {
        if (e.is_directory(ec)) {
            only = e.path();
            dirs++;
        }
    }
    if (dirs == 1 && fs::is_regular_file(only / gameFile, ec)) {
        packageRoot = only;
        return true;
    }
    error = "the download does not hold " + gameFile;
    return false;
}

std::vector<std::string> PlanInstall(const fs::path& packageRoot, const fs::path& installRoot) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(packageRoot, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::string rel = ToRel(fs::relative(it->path(), packageRoot, ec));
        if (ec || IsProtectedPath(rel))
            continue;
        if (KeepIfPresent(rel) && fs::exists(installRoot / rel, ec))
            continue;
        files.push_back(rel);
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool InstallFiles(const fs::path& packageRoot, const fs::path& installRoot, const std::vector<std::string>& files,
                  const fs::path& backupDir, std::string& error) {
    struct Step {
        fs::path src, dst;
        fs::path backup; // empty: there was no old file
        bool moved;      // the new file was moved (not copied) out of the package: move it back on a rollback
    };
    std::vector<Step> done;
    std::vector<fs::path> madeDirs;
    std::error_code ec;
    fs::remove_all(backupDir, ec);
    fs::create_directories(backupDir, ec);

    auto rollback = [&]() {
        bool clean = true;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code rec;
            if (it->moved) {
                fs::rename(it->dst, it->src, rec); // the package stays whole for the next try
                if (rec)
                    fs::remove(it->dst, rec);
            } else {
                fs::remove(it->dst, rec);
            }
            rec.clear();
            if (!it->backup.empty()) {
                fs::rename(it->backup, it->dst, rec);
                if (rec)
                    clean = false;
            }
        }
        for (auto it = madeDirs.rbegin(); it != madeDirs.rend(); ++it) {
            std::error_code rec;
            fs::remove(*it, rec); // only if empty
        }
        return clean;
    };
    auto fail = [&](const std::string& why) {
        const bool clean = rollback();
        error = clean ? why + ". Nothing was changed."
                      : why + ". Some old files could not be put back; they are in " + PathUtf8(backupDir);
        return false;
    };

    for (const auto& rel : files) {
        if (IsProtectedPath(rel))
            return fail("refused to write " + rel);
        const fs::path src = packageRoot / rel;
        const fs::path dst = installRoot / rel;
        // make missing folders, remembering them for a rollback
        std::vector<fs::path> chain;
        for (fs::path p = dst.parent_path(); !p.empty() && p != installRoot && !fs::exists(p, ec); p = p.parent_path())
            chain.push_back(p);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            fs::create_directory(*it, ec);
            if (ec)
                return fail("could not make the folder " + ToRel(fs::relative(*it, installRoot, ec)));
            madeDirs.push_back(*it);
        }
        Step step{ src, dst, {}, true };
        if (fs::exists(fs::symlink_status(dst, ec))) {
            if (fs::is_directory(fs::symlink_status(dst, ec)))
                return fail(rel + " is a folder in the game folder");
            step.backup = backupDir / rel;
            fs::create_directories(step.backup.parent_path(), ec);
            // a rename also works for the running game's own exe on Windows
            fs::rename(dst, step.backup, ec);
            if (ec)
                return fail(rel + " is in use (" + ec.message() + ")");
        }
        fs::rename(src, dst, ec);
        if (ec) {
            ec.clear();
            step.moved = false;
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                fs::permissions(dst, fs::status(src, ec).permissions(), ec);
        }
        if (ec) {
            if (!step.backup.empty()) {
                std::error_code rec;
                fs::rename(step.backup, dst, rec);
            }
            return fail("could not write " + rel + " (" + ec.message() + ")");
        }
        done.push_back(step);
    }
    return true;
}

} // namespace Updater
