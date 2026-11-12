#pragma once
// The game independent half of the Story Co-op auto updater: what the newest release is, downloading it, checking
// it against the release's SHA256SUMS.txt, unpacking it next to the game and swapping the files in with a way back.
// No game headers here, so tools/updater-test can build and test it on its own.
//
// What it promises:
//   - It only talks to this repo's GitHub releases, over HTTPS (curl with --proto =https), and sends nothing but
//     the request itself: no ids, no settings, no save data. The user agent is a fixed "OcarinaStoryCoop-Updater".
//   - A package is only used if its SHA-256 matches the SHA256SUMS.txt published with the same release.
//   - Only files that are in the new package are replaced, and never the player's own files: saves, the ROM and the
//     game data built from it (oot.o2r, oot-mq.o2r), the settings (shipofharkinian.json, imgui.ini, the server's
//     config.json), mods, logs and the co-op playthrough list. See IsProtectedPath.
//   - Every replaced file is moved to updates/backup first. If anything fails, everything is put back.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Updater {

constexpr const char* kRepo = "reaffirmed/ocarina-coop-timefix";
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/reaffirmed/ocarina-coop-timefix/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/reaffirmed/ocarina-coop-timefix/releases/download/";
constexpr const char* kReleasePagePrefix = "https://github.com/reaffirmed/ocarina-coop-timefix/releases/tag/";
constexpr const char* kSumsName = "SHA256SUMS.txt";

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
    std::string ToString() const;
};
// "v1.2.3" or "1.2.3". Anything else (pre-release suffixes, missing parts) is not valid.
Version ParseVersion(const std::string& text);
// <0, 0, >0
int CompareVersions(const Version& a, const Version& b);

enum class Platform { Windows, Linux, Unsupported };
Platform CurrentPlatform();

struct Asset {
    std::string name;
    std::string url;
    uint64_t size = 0;
};

struct Release {
    std::string tag; // vX.Y.Z
    Version version;
    std::string title;
    Asset package; // the zip / tarball for this platform
    Asset sums;    // SHA256SUMS.txt
};

// Reads the GitHub API answer for /releases/latest. Fails (with a reason) for drafts, pre-releases, tags that are not
// vX.Y.Z, a release without a package for `platform` or without SHA256SUMS.txt, and download URLs that are not
// exactly kDownloadPrefix + tag + "/" + name.
bool ParseRelease(const std::string& json, Platform platform, Release& out, std::string& error);

// SHA-256 of a file as lower case hex; empty on a read error.
std::string Sha256File(const std::filesystem::path& file);
std::string Sha256Hex(const void* data, size_t size);
// Finds `fileName` in a sha256sum style list ("<hex>  <name>" or "<hex> *<name>"). Lower case hex, or empty.
std::string FindSum(const std::string& sumsText, const std::string& fileName);

// Runs a program (args[0] is its full path) without a console window and waits for it. Returns its exit code,
// or -1 if it could not be started.
int RunProcess(const std::vector<std::string>& args);
// Starts a program and does not wait (to start the game again after an update).
bool StartDetached(const std::vector<std::string>& args, const std::filesystem::path& workingDir);

// Downloads an https URL to `dest` (written to dest + ".part" first). Fails for anything that is not https, a
// redirect to something that is not https, more than maxBytes, or after maxSeconds.
bool Download(const std::string& url, const std::filesystem::path& dest, uint64_t maxBytes, int maxSeconds,
              std::string& error);
// Same, into memory (for the small API answer and the checksum list).
bool DownloadText(const std::string& url, const std::filesystem::path& scratch, std::string& out, std::string& error);

// ------------------------------------------------------------------------------------------------- install

// Where the game is installed and what its game file is called: soh.exe next to the running exe on Windows, the
// soh.appimage that $APPIMAGE points to on Linux. False for a build that was not installed from a release package
// (a source build): the updater then stays off.
bool FindInstall(std::filesystem::path& root, std::string& gameFile);

// True for paths (relative to the game folder, '/' separated) that belong to the player and must never be written.
bool IsProtectedPath(const std::string& relPath);

// Unpacks a verified package into `stagingDir` (emptied first) and finds the folder in it that holds `gameFile`
// (the tarball has one top folder, the zip none). Rejects links and anything outside stagingDir.
bool UnpackPackage(const std::filesystem::path& archive, const std::filesystem::path& stagingDir,
                   const std::string& gameFile, std::filesystem::path& packageRoot, std::string& error);

// The files of an unpacked package that an install would write (relative, '/' separated). Protected paths and
// files the player already has a copy of that must be kept (server/config.json) are left out.
std::vector<std::string> PlanInstall(const std::filesystem::path& packageRoot,
                                     const std::filesystem::path& installRoot);

// Moves each planned file of packageRoot into installRoot. Files it replaces go to backupDir first (emptied before).
// On any failure everything done so far is undone and the reason is returned.
bool InstallFiles(const std::filesystem::path& packageRoot, const std::filesystem::path& installRoot,
                  const std::vector<std::string>& files, const std::filesystem::path& backupDir, std::string& error);

// Paths as UTF-8 text and back (std::filesystem's string() is the ANSI code page on Windows).
std::string PathUtf8(const std::filesystem::path& p);
std::filesystem::path PathFromUtf8(const std::string& s);
std::string ToRel(const std::filesystem::path& p); // relative, '/' separated, UTF-8

} // namespace Updater
