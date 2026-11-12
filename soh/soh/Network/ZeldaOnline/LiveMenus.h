#ifndef ZELDA_ONLINE_LIVE_MENUS_H
#define ZELDA_ONLINE_LIVE_MENUS_H

#include <stdint.h>

// "Live menus": while connected to Zelda Online, the pause menu and the ocarina no longer stop the local world.
// Enemies, other players and the rest of the scene keep running, Dark Souls style, so a player in a menu never
// freezes the world for anyone, themselves included. Link stands still while the pause menu is open, and the menu
// closes by itself when something needs the screen (death, a scene change, a cutscene or a textbox).
//
// Turned off with the "gZeldaOnline.LiveMenus" CVar, or by not being connected; the game then pauses as in vanilla.

#ifdef __cplusplus
extern "C" {
#endif

// True while connected and the setting is on. Checked when a menu opens.
uint8_t ZeldaOnline_LiveMenusEnabled(void);

// True while the local player is playing the ocarina and that should not freeze the world.
uint8_t ZeldaOnline_LiveOcarina(void);

// Set when the pause menu opened as a live menu, until it is closed again. The world keeps updating and drawing
// under it instead of showing a captured frame. Defined in z_kaleido_setup.c.
extern uint8_t gZeldaOnlineLivePause;

#ifdef __cplusplus
}
#endif

#endif
