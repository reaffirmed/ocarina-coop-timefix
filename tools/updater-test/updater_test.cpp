// Tests for the game independent part of the auto updater (soh/soh/Updater/UpdaterCore.cpp).
//   sh tools/updater-test/run.sh            offline tests
//   sh tools/updater-test/run.sh --network  also downloads the newest real release and checks it
// Linux only (it builds test packages with tar).
#include "../../soh/soh/Updater/UpdaterCore.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace Updater;

static int sFailures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED: " #cond "\n";                                      \
            sFailures++;                                                                                               \
        }                                                                                                              \
    } while (0)

static void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string ReleaseJson(const std::string& tag, const std::string& base, bool prerelease = false) {
    return std::string("{\"tag_name\":\"") + tag + "\",\"name\":\"" + tag + "\",\"draft\":false,\"prerelease\":" +
           (prerelease ? "true" : "false") + ",\"assets\":[" + "{\"name\":\"OcarinaStoryCoop-windows.zip\",\"size\":10," +
           "\"browser_download_url\":\"" + base + tag + "/OcarinaStoryCoop-windows.zip\"}," +
           "{\"name\":\"OcarinaStoryCoop-1.1.0-linux.tar.gz\",\"size\":20,\"browser_download_url\":\"" + base + tag +
           "/OcarinaStoryCoop-1.1.0-linux.tar.gz\"}," + "{\"name\":\"SHA256SUMS.txt\",\"size\":30," +
           "\"browser_download_url\":\"" + base + tag + "/SHA256SUMS.txt\"}]}";
}

static void TestVersions() {
    CHECK(ParseVersion("v1.2.3").valid);
    CHECK(ParseVersion("1.2.3").valid);
    CHECK(ParseVersion("v10.20.30").minor == 20);
    CHECK(!ParseVersion("v1.2").valid);
    CHECK(!ParseVersion("v1.2.3-beta").valid);
    CHECK(!ParseVersion("v1.2.3.4").valid);
    CHECK(!ParseVersion("").valid);
    CHECK(!ParseVersion("dev-b62412a").valid);
    CHECK(CompareVersions(ParseVersion("1.0.0"), ParseVersion("v1.0.1")) < 0);
    CHECK(CompareVersions(ParseVersion("1.10.0"), ParseVersion("v1.9.9")) > 0);
    CHECK(CompareVersions(ParseVersion("2.0.0"), ParseVersion("v2.0.0")) == 0);
}

static void TestParseRelease() {
    Release r;
    std::string err;
    CHECK(ParseRelease(ReleaseJson("v1.1.0", kDownloadPrefix), Platform::Windows, r, err));
    CHECK(r.package.name == "OcarinaStoryCoop-windows.zip" && r.sums.name == "SHA256SUMS.txt");
    CHECK(ParseRelease(ReleaseJson("v1.1.0", kDownloadPrefix), Platform::Linux, r, err));
    CHECK(r.package.name == "OcarinaStoryCoop-1.1.0-linux.tar.gz" && r.package.size == 20);
    CHECK(!ParseRelease(ReleaseJson("v1.1.0", kDownloadPrefix), Platform::Unsupported, r, err));
    // another repo, or plain http: nothing is taken from it
    CHECK(!ParseRelease(ReleaseJson("v1.1.0", "https://github.com/someone/else/releases/download/"),
                        Platform::Windows, r, err));
    CHECK(!ParseRelease(ReleaseJson("v1.1.0", "http://github.com/reaffirmed/ocarina-coop-timefix/releases/download/"),
                        Platform::Windows, r, err));
    CHECK(!ParseRelease(ReleaseJson("v1.1.0", kDownloadPrefix, true), Platform::Windows, r, err));
    CHECK(!ParseRelease(ReleaseJson("nightly", kDownloadPrefix), Platform::Windows, r, err));
    CHECK(!ParseRelease("not json", Platform::Windows, r, err));
    CHECK(!ParseRelease("{\"message\":\"API rate limit exceeded\"}", Platform::Windows, r, err));
}

static void TestSha() {
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string million(1000000, 'a');
    CHECK(Sha256Hex(million.data(), million.size()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const std::string sums = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  OcarinaStoryCoop-windows.zip\r\n"
                             "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb *other.tar.gz\n";
    CHECK(FindSum(sums, "OcarinaStoryCoop-windows.zip") == std::string(64, 'a'));
    CHECK(FindSum(sums, "other.tar.gz") == std::string(64, 'b'));
    CHECK(FindSum(sums, "missing.zip").empty());
    CHECK(FindSum(sums, "OcarinaStoryCoop").empty());
}

static void TestProtected() {
    for (const char* p : { "Save/file1.sav", "save/global.sav", "oot.o2r", "oot-mq.o2r", "mods/x.o2r", "baserom.z64",
                           "shipofharkinian.json", "imgui.ini", "server/story/team.json", "logs/soh.log",
                           "updates/pending.json", "../soh.exe", "/etc/passwd", "C:/x", "a/../b", "server/x.o2r",
                           "coop-playthroughs.json", "Ocarina.N64" })
        if (!IsProtectedPath(p)) {
            std::cerr << "not protected: " << p << "\n";
            sFailures++;
        }
    for (const char* p : { "soh.exe", "soh.appimage", "soh.o2r", "server/OOTServer.exe", "OOTSteamLink.exe",
                           "Host over Steam.bat", "gamecontrollerdb.txt", "readme.txt", "steam_api64.dll" })
        if (IsProtectedPath(p)) {
            std::cerr << "protected by mistake: " << p << "\n";
            sFailures++;
        }
}

// A fake release folder, a fake newer package, and an install of one into the other.
static void TestInstall(const fs::path& work) {
    fs::remove_all(work);
    const fs::path game = work / "game";
    Write(game / "soh.appimage", "old game");
    Write(game / "soh.o2r", "old soh.o2r");
    Write(game / "oot.o2r", "MY ROM DATA");
    Write(game / "Save" / "file1.sav", "MY SAVE");
    Write(game / "shipofharkinian.json", "{\"my\":\"settings\"}");
    Write(game / "server" / "OOTServer", "old server");
    Write(game / "server" / "config.json", "{\"port\":12345}");
    Write(game / "server" / "story" / "team.json", "TEAM SAVE");

    // the new package, the way package.sh makes it (one top folder), with files it must not be allowed to write
    const fs::path src = work / "src" / "OcarinaStoryCoop-1.1.0-linux";
    Write(src / "soh.appimage", "new game");
    Write(src / "soh.o2r", "new soh.o2r");
    Write(src / "server" / "OOTServer", "new server");
    Write(src / "server" / "config.json", "{\"port\":21050}");
    Write(src / "README.txt", "new readme");
    Write(src / "oot.o2r", "SHOULD NEVER BE WRITTEN");
    Write(src / "Save" / "file1.sav", "SHOULD NEVER BE WRITTEN");
    Write(src / "shipofharkinian.json", "SHOULD NEVER BE WRITTEN");
    const fs::path archive = work / "pkg.tar.gz";
    CHECK(std::system(("tar -C '" + (work / "src").string() + "' -czf '" + archive.string() +
                       "' OcarinaStoryCoop-1.1.0-linux")
                          .c_str()) == 0);

    fs::path root;
    std::string err;
    const fs::path staging = game / "updates" / "staging";
    CHECK(UnpackPackage(archive, staging, "soh.appimage", root, err));
    CHECK(root.filename() == "OcarinaStoryCoop-1.1.0-linux");
    std::vector<std::string> files = PlanInstall(root, game);
    for (const auto& f : files)
        std::cout << "  would install " << f << "\n";
    CHECK(files.size() == 4); // README.txt, server/OOTServer, soh.appimage, soh.o2r

    // a failure part way through puts everything back: make the 3rd file a folder in the game folder
    fs::create_directories(game / "soh.appimage.dir");
    std::vector<std::string> bad = files;
    bad.insert(bad.begin() + 2, "soh.appimage.dir");
    Write(root / "soh.appimage.dir", "x");
    CHECK(!InstallFiles(root, game, bad, game / "updates" / "backup", err));
    std::cout << "  failed as it should: " << err << "\n";
    CHECK(Read(game / "README.txt").empty() && !fs::exists(game / "README.txt"));
    CHECK(Read(game / "server" / "OOTServer") == "old server");
    CHECK(Read(game / "soh.appimage") == "old game");
    fs::remove(root / "soh.appimage.dir");
    fs::remove(game / "soh.appimage.dir");

    // and the real install
    CHECK(InstallFiles(root, game, files, game / "updates" / "backup", err));
    CHECK(Read(game / "soh.appimage") == "new game");
    CHECK(Read(game / "soh.o2r") == "new soh.o2r");
    CHECK(Read(game / "server" / "OOTServer") == "new server");
    CHECK(Read(game / "README.txt") == "new readme");
    // the player's files
    CHECK(Read(game / "oot.o2r") == "MY ROM DATA");
    CHECK(Read(game / "Save" / "file1.sav") == "MY SAVE");
    CHECK(Read(game / "shipofharkinian.json") == "{\"my\":\"settings\"}");
    CHECK(Read(game / "server" / "config.json") == "{\"port\":12345}");
    CHECK(Read(game / "server" / "story" / "team.json") == "TEAM SAVE");
    // the old files are kept
    CHECK(Read(game / "updates" / "backup" / "soh.appimage") == "old game");

    // a package with a link in it is refused
    fs::remove_all(work / "src2");
    fs::create_directories(work / "src2" / "pkg");
    Write(work / "src2" / "pkg" / "soh.appimage", "x");
    fs::create_symlink("/etc/passwd", work / "src2" / "pkg" / "evil");
    CHECK(std::system(("tar -C '" + (work / "src2").string() + "' -czf '" + (work / "evil.tar.gz").string() + "' pkg")
                          .c_str()) == 0);
    CHECK(!UnpackPackage(work / "evil.tar.gz", staging, "soh.appimage", root, err));
    std::cout << "  refused as it should: " << err << "\n";
}

// The real thing: the newest release of the repo, downloaded and checked like the game does.
static void TestNetwork(const fs::path& work) {
    std::string text, err;
    CHECK(DownloadText(kLatestReleaseApi, work / "latest.json", text, err));
    Release r;
    if (!ParseRelease(text, Platform::Linux, r, err)) {
        std::cerr << "release: " << err << "\n";
        sFailures++;
        return;
    }
    std::cout << "  newest release " << r.tag << ": " << r.package.name << " (" << r.package.size << " bytes)\n";
    std::string sums;
    CHECK(DownloadText(r.sums.url, work / "sums.txt", sums, err));
    const std::string expected = FindSum(sums, r.package.name);
    CHECK(expected.size() == 64);
    const fs::path archive = work / r.package.name;
    CHECK(Download(r.package.url, archive, r.package.size + 4096, 600, err));
    CHECK(Sha256File(archive) == expected);
    std::cout << "  SHA-256 matches SHA256SUMS.txt: " << expected << "\n";
    // too small a limit is refused
    CHECK(!Download(r.package.url, work / "small", 1000, 600, err));
    std::cout << "  size limit refused as it should: " << err << "\n";
    // not https
    CHECK(!Download("http://github.com/", work / "http", 1000, 60, err));
    fs::path root;
    CHECK(UnpackPackage(archive, work / "staging", "soh.appimage", root, err));
    for (const auto& f : PlanInstall(root, work / "nothing-installed"))
        std::cout << "  would install " << f << "\n";
}

int main(int argc, char** argv) {
    const fs::path work = fs::temp_directory_path() / "story-coop-updater-test";
    TestVersions();
    TestParseRelease();
    TestSha();
    TestProtected();
    TestInstall(work);
    if (argc > 1 && std::string(argv[1]) == "--network")
        TestNetwork(work);
    std::cout << (sFailures == 0 ? "all updater tests passed\n" : "updater tests FAILED\n");
    return sFailures == 0 ? 0 : 1;
}
