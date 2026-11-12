#pragma once
// Shared between the updater (Updater.cpp) and its file select prompt (UpdaterFileSelect.cpp).

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "UpdaterCore.h"

namespace UpdaterGame {

enum class State {
    Off,         // not checked (a source build, unsupported system, or turned off)
    Checking,    // asking GitHub
    Idle,        // up to date, skipped, ignored, or the check failed quietly
    Available,   // a newer release: Update / Skip / Ignore until next update
    Downloading, // downloading and checking it
    Ready,       // downloaded and unpacked: installs when the game closes
    Failed,      // the download or its check failed; nothing was changed
};

enum class Choice { Update, Skip, Ignore, InstallNow, Later, Ok };

struct Shared {
    std::mutex mutex;
    State state = State::Off;
    Updater::Release release;
    bool promptOpen = false;
    int cursor = 0;
    std::string message;                // why it failed
    std::string notice;                 // the outcome of the last install, shown once
    std::filesystem::path downloadPart; // the file being downloaded, for the progress
};

Shared& S();
void Choose(Choice choice);
uint64_t DownloadedBytes(const std::filesystem::path& part); // the size of the file being downloaded so far
std::string CurrentVersionText();

} // namespace UpdaterGame
