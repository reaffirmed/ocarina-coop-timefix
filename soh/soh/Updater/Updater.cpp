#include "Updater.h"
#include "UpdaterCore.h"
#include "StoryCoopVersion.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "UpdaterInternal.h"

namespace fs = std::filesystem;
using namespace Updater;

namespace UpdaterGame {

// Never destroyed: the background threads may still be running while the game shuts down.
Shared& S() {
    static Shared* s = new Shared();
    return *s;
}

namespace {

fs::path sRoot;        // the game folder
std::string sGameFile; // soh.exe / soh.appimage
bool sActive = false;  // installed from a release package and the check is on
bool sRestartAfter = false;

fs::path UpdatesDir() {
    return sRoot / "updates";
}
fs::path PendingFile() {
    return UpdatesDir() / "pending.json";
}
fs::path ResultFile() {
    return UpdatesDir() / "result.json";
}

Version Current() {
    return ParseVersion(STORY_COOP_VERSION);
}

void SaveCVars() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx && ctx->GetWindow() && ctx->GetWindow()->GetGui())
        ctx->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

bool WriteJson(const fs::path& file, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << j.dump(2);
        if (!out)
            return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

nlohmann::json ReadJson(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return nlohmann::json();
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}

void SetFailed(const std::string& what, const std::string& why) {
    SPDLOG_WARN("[Updater] {}: {}", what, why);
    std::lock_guard<std::mutex> lock(S().mutex);
    S().state = State::Failed;
    S().message = what + ": " + why + ".";
    S().promptOpen = true;
    S().cursor = 0;
}

void CheckThread() {
    std::string text, error;
    if (!DownloadText(kLatestReleaseApi, UpdatesDir() / "latest.json", text, error)) {
        SPDLOG_INFO("[Updater] no update check: {}", error);
        std::lock_guard<std::mutex> lock(S().mutex);
        S().state = State::Idle;
        return;
    }
    Release release;
    if (!ParseRelease(text, CurrentPlatform(), release, error)) {
        SPDLOG_INFO("[Updater] newest release not usable: {}", error);
        std::lock_guard<std::mutex> lock(S().mutex);
        S().state = State::Idle;
        return;
    }
    const bool newer = CompareVersions(release.version, Current()) > 0;
    const std::string ignored = CVarGetString(CVAR_UPDATER_IGNORED, "");
    SPDLOG_INFO("[Updater] running {}, newest release {}{}", STORY_COOP_VERSION, release.tag,
                newer && ignored == release.tag ? " (ignored)" : "");
    std::lock_guard<std::mutex> lock(S().mutex);
    S().release = release;
    if (newer && ignored != release.tag) {
        S().state = State::Available;
        S().promptOpen = true;
        S().cursor = 0;
    } else {
        S().state = State::Idle;
    }
}

void DownloadThread(Release release) {
    const fs::path dir = UpdatesDir() / "download";
    const fs::path archive = dir / release.package.name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        S().downloadPart = archive;
        S().downloadPart += ".part";
    }

    std::string sums, error;
    if (!DownloadText(release.sums.url, dir / kSumsName, sums, error))
        return SetFailed("The update could not be downloaded", error);
    const std::string expected = FindSum(sums, release.package.name);
    if (expected.empty())
        return SetFailed("The update could not be checked", "SHA256SUMS.txt does not list " + release.package.name);

    // the size GitHub gives, with room to spare; never more than 1 GB
    const uint64_t limit = release.package.size > 0 ? release.package.size + 4096 : (1ull << 30);
    if (!Download(release.package.url, archive, std::min<uint64_t>(limit, 1ull << 30), 60 * 60, error))
        return SetFailed("The update could not be downloaded", error);
    const std::string actual = Sha256File(archive);
    if (actual != expected) {
        fs::remove(archive, ec);
        return SetFailed("The update was not installed", "the download does not match its published checksum");
    }
    SPDLOG_INFO("[Updater] {} downloaded, SHA-256 {} matches", release.package.name, actual);

    fs::path packageRoot;
    const fs::path staging = UpdatesDir() / "staging";
    if (!UnpackPackage(archive, staging, sGameFile, packageRoot, error)) {
        fs::remove_all(staging, ec);
        return SetFailed("The update was not installed", error);
    }
    fs::remove_all(dir, ec); // the archive is not needed any more

    nlohmann::json pending = { { "tag", release.tag },
                               { "package", ToRel(fs::relative(packageRoot, sRoot, ec)) },
                               { "attempts", 0 } };
    if (!WriteJson(PendingFile(), pending))
        return SetFailed("The update was not installed", "could not write " + PathUtf8(PendingFile()));

    std::lock_guard<std::mutex> lock(S().mutex);
    S().state = State::Ready;
    S().promptOpen = true;
    S().cursor = 0;
}

// What an earlier start left: the outcome of the last install, and an update that is waiting to be installed.
void ReadEarlierRun() {
    std::error_code ec;
    nlohmann::json result = ReadJson(ResultFile());
    if (result.is_object()) {
        const bool ok = result.value("ok", false);
        const std::string tag = result.value("tag", "");
        std::lock_guard<std::mutex> lock(S().mutex);
        if (ok && CompareVersions(ParseVersion(tag), Current()) == 0) {
            S().notice = "Updated to " + tag + ".";
            fs::remove_all(UpdatesDir() / "backup", ec); // the old files: this start shows the new ones work
        } else if (!ok) {
            S().notice = "The update to " + tag + " was not installed: " + result.value("error", "unknown error");
        }
        fs::remove(ResultFile(), ec);
    }

    nlohmann::json pending = ReadJson(PendingFile());
    const Version tag = pending.is_object() ? ParseVersion(pending.value("tag", "")) : Version{};
    if (tag.valid && CompareVersions(tag, Current()) > 0 &&
        fs::exists(sRoot / PathFromUtf8(pending.value("package", "")), ec)) {
        std::lock_guard<std::mutex> lock(S().mutex);
        S().release.tag = pending.value("tag", "");
        S().release.version = tag;
        S().state = State::Ready;
        S().promptOpen = false; // a short line on the file select reminds of it
        return;
    }
    // nothing usable is waiting (or it is this version already): clear the leftovers
    fs::remove(PendingFile(), ec);
    fs::remove_all(UpdatesDir() / "staging", ec);
    fs::remove_all(UpdatesDir() / "download", ec);
}

} // namespace

void Choose(Choice choice) {
    std::unique_lock<std::mutex> lock(S().mutex);
    switch (choice) {
        case Choice::Update: {
            S().state = State::Downloading;
            S().promptOpen = false;
            Release release = S().release;
            lock.unlock();
            SPDLOG_INFO("[Updater] downloading {}", release.tag);
            std::thread(DownloadThread, release).detach();
            break;
        }
        case Choice::Skip:
            S().state = State::Idle;
            S().promptOpen = false;
            break;
        case Choice::Ignore: {
            const std::string tag = S().release.tag;
            S().state = State::Idle;
            S().promptOpen = false;
            lock.unlock();
            CVarSetString(CVAR_UPDATER_IGNORED, tag.c_str());
            SaveCVars();
            SPDLOG_INFO("[Updater] ignoring {} until a newer release", tag);
            break;
        }
        case Choice::InstallNow: {
            S().promptOpen = false;
            lock.unlock();
            sRestartAfter = true;
            auto ctx = Ship::Context::GetRawInstance();
            if (ctx && ctx->GetWindow())
                ctx->GetWindow()->Close();
            break;
        }
        case Choice::Later:
        case Choice::Ok:
            S().promptOpen = false;
            if (S().state == State::Failed)
                S().state = State::Idle;
            break;
    }
}

uint64_t DownloadedBytes(const fs::path& part) {
    std::error_code ec;
    uint64_t size = part.empty() ? 0 : fs::file_size(part, ec);
    return ec ? 0 : size;
}

std::string CurrentVersionText() {
    return STORY_COOP_VERSION;
}

} // namespace UpdaterGame

using namespace UpdaterGame;

extern "C" void Updater_Init(void) {
    if (CurrentPlatform() == Platform::Unsupported || !ParseVersion(STORY_COOP_VERSION).valid)
        return;
    if (!FindInstall(sRoot, sGameFile)) {
        SPDLOG_INFO("[Updater] not a release install, the update check is off");
        return;
    }
    ReadEarlierRun();
    if (CVarGetInteger(CVAR_UPDATER_ENABLED, 1) == 0) {
        SPDLOG_INFO("[Updater] the update check is turned off");
        sActive = true; // a waiting update is still installed
        return;
    }
    sActive = true;
    std::lock_guard<std::mutex> lock(S().mutex);
    if (S().state == State::Ready)
        return; // already downloaded
    S().state = State::Checking;
    std::thread(CheckThread).detach();
}

// Installs the update that is waiting, if there is a usable one. Returns true when it was installed. Writes the
// outcome for the next start to show. Safe to call before the game's logging is up (it only logs through spdlog
// when `log` is set).
static bool InstallPending(bool log) {
    std::error_code ec;
    nlohmann::json pending = ReadJson(PendingFile());
    if (!pending.is_object())
        return false;
    const std::string tag = pending.value("tag", "");
    const Version version = ParseVersion(tag);
    const fs::path staging = fs::weakly_canonical(UpdatesDir() / "staging", ec);
    const fs::path packageRoot = fs::weakly_canonical(sRoot / PathFromUtf8(pending.value("package", "")), ec);
    const std::string inside = ToRel(fs::relative(packageRoot, staging, ec));
    if (!version.valid || CompareVersions(version, Current()) <= 0 || ec || inside.empty() ||
        inside.rfind("..", 0) == 0 || !fs::is_regular_file(packageRoot / sGameFile, ec)) {
        fs::remove(PendingFile(), ec);
        fs::remove_all(UpdatesDir() / "staging", ec);
        return false;
    }

    const std::vector<std::string> files = PlanInstall(packageRoot, sRoot);
    std::string error;
    if (log)
        SPDLOG_INFO("[Updater] installing {} ({} files)", tag, files.size());
    const bool ok = InstallFiles(packageRoot, sRoot, files, UpdatesDir() / "backup", error);
    if (ok) {
        fs::remove(PendingFile(), ec);
        fs::remove_all(UpdatesDir() / "staging", ec);
        WriteJson(ResultFile(), { { "ok", true }, { "tag", tag } });
        if (log)
            SPDLOG_INFO("[Updater] installed {}", tag);
        return true;
    }
    // try again later (a file may have been in use), three times at most
    const int attempts = pending.value("attempts", 0) + 1;
    if (log)
        SPDLOG_ERROR("[Updater] install of {} failed (try {}): {}", tag, attempts, error);
    if (attempts >= 3) {
        fs::remove(PendingFile(), ec);
        fs::remove_all(UpdatesDir() / "staging", ec);
    } else {
        pending["attempts"] = attempts;
        WriteJson(PendingFile(), pending);
    }
    WriteJson(ResultFile(), { { "ok", false }, { "tag", tag }, { "error", error } });
    return false;
}

extern "C" void Updater_EarlyStart(int argc, char* argv[]) {
    // An update that was downloaded but not installed when the game closed (it was killed, or crashed on the way
    // out): install it now, before any game file is opened, and start the new version in place of this one.
    // Not when the Steam helper started the game: it waits for this very process, so the update waits for a start
    // without it.
    if (CurrentPlatform() == Platform::Unsupported || !ParseVersion(STORY_COOP_VERSION).valid ||
        std::getenv("OOT_SERVER") != nullptr || !FindInstall(sRoot, sGameFile))
        return;
    std::error_code ec;
    if (!fs::exists(PendingFile(), ec) || !InstallPending(false))
        return;
    std::vector<std::string> args = { PathUtf8(sRoot / sGameFile) };
    for (int i = 1; i < argc; i++)
        args.push_back(argv[i]);
    if (StartDetached(args, sRoot))
        std::exit(0);
    // could not start the new version: carry on with this one (the next start is the new version)
}

extern "C" void Updater_Shutdown(void) {
    if (!sActive)
        return;
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        if (S().state == State::Downloading)
            return; // not finished: the next start clears it
    }
    InstallPending(true);

    // "Install now": start the game again. Not when the Steam helper started it: it needs the helper's connection,
    // so the prompt asked the player to start it from "Host / Join over Steam" again.
    if (sRestartAfter && std::getenv("OOT_SERVER") == nullptr) {
        StartDetached({ PathUtf8(sRoot / sGameFile) }, sRoot);
    }
}
