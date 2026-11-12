#pragma once
// The Co-op screen of the file select: host or join a Story Co-op game, pick the playthrough and its story settings,
// drawn with the game's own window, font and sounds. The logic behind it is CoopLobbyWindow (ZeldaOnlineLobby.hpp).

#ifdef __cplusplus
extern "C" {
#endif
struct GameState;
struct FileChooseContext;

// Main menu: R opens the Co-op screen. Returns 1 when it did (the rest of the main menu update is skipped).
int CoopLobby_FileSelectCheckOpen(struct FileChooseContext* fileChooseContext);
// Main menu: the "R: Co-op" prompt, with what the lobby is doing.
void CoopLobby_FileSelectDrawPrompt(struct FileChooseContext* fileChooseContext);

void FileChoose_RotateToCoop(struct GameState* gameState);
void FileChoose_StartCoopMenu(struct GameState* gameState);
void FileChoose_UpdateCoopMenu(struct GameState* gameState);
void FileChoose_DrawCoopMenu(struct FileChooseContext* fileChooseContext);
// 1 when the full screen (modern) lobby is chosen: the file select window is not drawn behind it
int CoopLobby_ModernLayout(void);
#ifdef __cplusplus
};
#endif
