#pragma once
// The Story Co-op auto updater. On start it asks GitHub for the newest release (in the background); when that is
// newer than this build, the file select shows a prompt with three choices:
//   Update                  download it now, check it, and install it when the game closes (or right away)
//   Skip                    not now: the prompt comes back the next time the game starts
//   Ignore until next update  this version is not offered again; the prompt only comes back for a newer one
// The files are swapped when the game shuts down (end of DeinitOTR), or, if it did not get that far, at the start of
// the next run before any game file is opened (Updater_EarlyStart), with every old file kept until it worked.
// Settings > General > "Check for Story Co-op updates" turns the check off. See UpdaterCore.h for the rules it keeps.

#ifdef __cplusplus
extern "C" {
#endif
struct FileChooseContext;

// First thing on start: installs an update that is still waiting from the last run and starts the new version.
void Updater_EarlyStart(int argc, char* argv[]);
// After the console variables are loaded: cleans up after an earlier update and starts the check.
void Updater_Init(void);
// At the very end of the shutdown: installs a downloaded update.
void Updater_Shutdown(void);

// File select main menu. Returns 1 while the prompt is open (it took the input).
int Updater_FileSelectUpdate(struct FileChooseContext* fileChooseContext);
void Updater_FileSelectDraw(struct FileChooseContext* fileChooseContext);
#ifdef __cplusplus
};
#endif

#define CVAR_UPDATER_ENABLED "gStoryCoop.Updater.Enabled"
#define CVAR_UPDATER_IGNORED "gStoryCoop.Updater.IgnoredVersion"
