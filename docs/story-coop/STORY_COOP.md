# Story Co-op

Story Co-op is an opt-in party setting for ShipwrightOnline. It turns a party into a shared playthrough of Ocarina of Time: the party shares progression, story flags and time travel, while each player keeps their own consumables. With the setting off (the default), the game and server behave exactly as before.

## Enabling it

Open the party window, then tick **Story Co-op** in the party settings (party admin only). A second option, **Share Ammo, Rupees & HP**, is available once Story Co-op is on and is off by default.

## Co-op Lobby (host and join from the game)

Press **R** on the file select screen: the window turns to the **Co-op** menu (the same turn as the Options menu),
drawn with the game's window, font, cursor and menu sounds. Use the control stick (or the D-pad, with D-pad in text
on), A to choose, left / right to change a setting, and B to go back. The top right of the file select shows
"R: Co-op" and what the lobby is doing. **Layout** on the Co-op menu switches between Classic (the list on the file
select window) and Modern (a full screen lobby: the choices on the left, a card with the playthrough and its progress
icons, and the party on the right). The same lobby is also a window in Network > Zelda Online > Co-op Lobby.
It needs `OOTSteamLink` and `OOTServer` from the download next to the game, and Steam running (Windows and Linux).

- **Host a game** lists your playthroughs, newest first, with the date you last played, the save file you used and who
  played with you. Choosing one opens its story settings; **Start hosting** (in the window: **Host this playthrough**) starts the server and a friends-only
  Steam lobby in the background, connects you, and creates a Story Co-op party with that playthrough's story settings
  and team code, so the server loads its team save. **Start new** adds a new playthrough with a fresh team code and
  selects it, so you can set it up before hosting. In the window version, right-click a
  playthrough to rename it, copy its team code or remove it from the list (the team save itself stays on the server).
- **Story settings** are kept per playthrough and shown under the selected one: share ammo, rupees and health,
  private dungeons, extra enemies and tougher enemies (with their amounts). A new playthrough starts from the party
  settings you used last. While hosting, **Story settings** changes them for the whole party right
  away (slider changes apply when you let go). The team code itself cannot change once the party exists.
- A playthrough is hosted under the nickname it was first hosted with, because the server keys the team save by the
  host's nickname and the team code. The lobby switches back to that name and says so.
- Everybody who connects to the host's server is invited to the party by itself.
- **Join a friend** lists the Steam friends who are hosting. Click one to join; the host's invite is accepted by
  itself. Accepting a Steam invite, or Join Game in the Steam friends list, works too while the list is open.
- Then everyone chooses **Choose a save file** (or presses B): the window turns back to the files while the session
  keeps running, and R returns to the Co-op menu. The lobby points at the file you used for this
  playthrough last time; any file works, because the team progress is merged into it.
- **Stop hosting** saves the team progress and stops the server. Closing the game does the same.
- The lobby's playthrough list is `coop-playthroughs.json` in the game's folder. A team code that was in use before the
  lobby existed is added to it as "Earlier playthrough".
- `Host over Steam` / `Join over Steam` and connecting by address still work as before.

How it works: the lobby starts `OOTSteamLink host --no-game` or `join --no-launch` hidden, with `--status-file` and
`--parent-pid` (see `tools/steamlink/README.md`), and connects the online client to the local port the helper reports.
Code: `soh/soh/Network/ZeldaOnline/ZeldaOnlineLobby.*`, `ZeldaOnlineLobbyFileSelect.*` (the file select menu), `ZeldaOnlineLobbyHelper.cpp`, and `LobbyConnect` /
`LobbyAfterAuth` / `LobbyInviteEveryone` / `LobbyAutoAccept` in `ZeldaOnlineClient.cpp`.

## What is shared

- **Items:** equipment, upgrades, quest items, songs, medallions, stones, heart containers and pieces, magic, dungeon items, small keys, boss keys, bottles themselves, Ruto's Letter and the child trade items up to Zelda's Letter (Weird Egg, Chicken, Zelda's Letter). When one member gets one, every member has it.
- **Stay per player (unless "Share Ammo, Rupees & HP" is on):** ammo counts, rupees, health refills and bottle contents.
- **Story flags:** event check flags, item get flags, info tables, shared event inf bits, scene flags (switch, chest, clear, collect) and gold skulltula tokens.
- **Dungeon progress:** the permanent switch and clear flags of dungeon and boss scenes (doors opened, rooms cleared) are part of the team save too, kept as they were when each member left the scene. They are added to the scene whenever a member loads it, including after the dungeon instance on the server has been reset or recreated, and chests stay open. Temporary switches and room flags are never saved by the game and are not shared. The Water Temple water level (switches 0x1C-0x1E) and the Forest Temple elevator (switch 0x1B) are saved by the game and rebuilt from those switches when the scene loads, so they are shared too, as last writer wins values instead of plain flags (see below). A member who is inside a dungeon when someone else's progress arrives sees it the next time they load that scene. A switch that is turned back off again (a crystal switch or other toggle) is dropped from the team save when the member who switched it off is in a party dungeon: the server removes it and tells the other members, so it does not come back on the next visit. A member who was offline or on the title screen at that moment can still bring the old state back when they reconnect, because the team save is merged by union.
- **Lon Lon Ranch and Epona:** Epona obtained, the rented horse, Talon, Malon and the race prizes are story flags and are shared. Where Epona waits (her parking spot) is shared too: whoever parks her last decides where she is for the party. The race itself (the Ingo and Malon events, the Cucco game, the shooting gallery, Richard's walk) is run state that the game never saves, so it is never forced on another player.
- **Scarecrow songs:** the spawn song and the long song are shared. The last one recorded is the one every member hears.
- **Records and counters:** Big Poe points add up across all members (both players' deliveries count toward the 1000 point reward), the horseback archery best score is the highest, the Lon Lon and Dampe race records are the lowest times, and the fishing pond record (longest catches, prize and visit bits) is merged. The Running Man's score and the pond owner's hat stay personal.
- **Map progress:** map areas visited, and the rooms and floors visited in every scene, are shared along with the map and compass.
- **Water Temple and Forest Temple:** the last water level any member set (or the elevator position) is what the next dungeon instance starts with, and two members inside at once always see the same level.
- **Stay personal:** which item is on which button and what is equipped, bottle contents, the adult trade chain, the mask loan chain, Talon's milk, Bombchu shop purchases, deaths and statistics.
- **Gift flags stay per player too:** flags that mean an NPC already gave you an item that is not shared (the adult trade chain, the mask loans, Talon's milk, Bombchu shop purchases) are not shared, so every member can still receive those items.
- **Zelda's Letter and the Weird Egg:** the Malon egg flag and the courtyard flag (Zelda's Letter) are story gates for Saria, the owl, Mido, the Kokiri, Talon and the Kakariko gate, so they are shared together with the child trade slot. A member who never saw the courtyard gets the flag, the letter and Zelda's Lullaby, so the NPCs act the same for everyone. The letter and its flag only travel once Zelda's Lullaby is owned, because the game takes the letter back from a save that has the flag but no Lullaby. A member further along the mask chain counts as having the letter and keeps their masks, and the trade items stay per player in the randomizer.
- **NPCs that move aside:** when another member moves Mido (sword and shield, or Saria's Song in the Lost Woods), King Zora or the Kakariko gate to Death Mountain while you are in the same area, they move for you right away instead of on the next area load. Mido in Kokiri Forest walks aside, the others are reloaded in their new spot.
- **Loading a save:** a team snapshot is only applied to the save that was loaded when it was requested. Going to the title screen or loading another save drops anything pending, and the newly loaded save is uploaded again, which brings back what the team has that it lacks.
- **Team save:** the server keeps the party's merged progress (team save format 2; files written by earlier servers still load, but client and server must be updated together because an older server ignores format 2 snapshots). A member who joins or reconnects is caught up on arrival, and the merge keeps their own consumables. The team save is stored with the server's other persistent data.

Sharing is state based: clients send a compact progress snapshot, the server merges it (union / max) into the team save and relays the result. Echoes are harmless and nobody loses progress.

## Time travel together

- Pulling the Master Sword (child) or placing it back (adult) at the pedestal puts you on hold: `Waiting for the others (n/N): <names>. Press B to cancel.`
- B cancels and withdraws your vote. Leaving the scene also withdraws it.
- When every party member has voted, all of them travel together and each client runs the game's own sequence (the first pull plays the Chamber of Sages).
- Members who disconnect or leave the party are removed from the count, and nobody is left on hold.
- A member who joins while the party is in a different era is moved to the party's era.
- Other age changes (the SwitchAge cheat or hotkey, the Ocarina Time Travel enhancement) are blocked while Story Co-op is active, with a notification pointing to the pedestal.
- A remote Song of Time never changes another player's age. Remote ocarinas are audio only, and only the local player's own age changes locally.

The party list shows each member's era (Adult or Child).

## Story cutscenes together

- When a story cutscene starts for one member, every member in the same scene watches it too: area intros, Zelda, Impa, Sheik, song lessons, the Sages, the Deku Tree, Kakariko burning and the rest.
- It works for both ways the game starts a cutscene: a cutscene script played in the current scene, and a transition into a scene's cutscene layer (blue warps, the Chamber of Sages). The few story scripts built into their actor instead of the game data (meeting the Great Deku Tree, and his mouth opening after you answer) are shared by name, and only with members in the same room, since they move Link around that actor. Members elsewhere in the scene just get the resulting flags. There is no per cutscene list; only what belongs to one player stays with that player: fence jumps on Epona, being caught by the Gerudo, and the races and horseback archery you start yourself.
- A cutscene that takes everybody to another scene (blue warps, the Chamber of Sages, Kakariko burning and the other transitions into a scene's cutscene layer) waits until the members in the same scene are ready, like the Master Sword pedestal. The member whose game started it is held with `Waiting for the others to be ready (n/N): <names>. Press L to go now.` The others see `<name> is ready to continue the story (n/N). Press L when you are ready.` and keep playing until they press L; then they are held too and can press B to take it back. When everybody in the scene is ready (or the member who started it presses L), the transition runs and the others are taken along. With nobody else in the scene there is no wait. Members in other scenes are not asked. Leaving the scene drops a member out of the vote, and the held member never stays stuck: a disconnect, leaving the party, switching Story Co-op off or a server that does not answer (an older server) lets the transition go ahead. Pressing L also toggles the minimap, as it always does.
- A cutscene another member starts in your scene does not take over right away: `Story scene with the party in 3. Press L to skip.` counts down first. Do nothing and you watch it with them; press L if you are busy and you keep playing (the story flags still reach you, and an area intro you skip still plays when you next enter the area). There is no countdown for the next part of a sequence you are already watching, or for a warp you said ready for.
- A member who is busy with a text box gets a short moment to finish it first. A member who is already in a cutscene or a transition is watching it already.
- Members in other scenes are not pulled in. The first visit intro of an area is kept per player, so they still see it when they first get there.
- If the member who started the cutscene disconnects, the others finish it and play on.
- Key story conversations that are plain text boxes are shared the same way: talking to the Great Deku Tree, Zelda, Saria, Impa, Darunia, King Zora, Ruto, Nabooru or Sheik shows the same text to the members in the same scene, at the speaker's pace. Their Link is held while the text is up. Questions are answered by the speaker. Talking to any other NPC stays personal. The others are let go when the speaker closes the text, leaves or disconnects.

## Gear you lose

A shield or tunic eaten by a Like Like, or a Deku Shield that burns, is lost for you only. Your teammates keep theirs, and the team save does not give it back to you: buy or find it again, like in single player.

## Server save

The team save is written to disk, so it survives a server restart. Clients still keep their own progress in their own saves and still re-seed the team save by union when they join, so nothing depends on the file being present.

- **Where:** `<dataDir>/story/<name>-<hash>.bin`. `dataDir` is set in `config.json` (default `data`, relative to where the server is started, empty disables saving).
- **Which file:** the key is the admin's nickname plus the party's team code (up to 32 characters, the `storyCode` setting). The code is always set while Story Co-op is on: the party window shows it under **Team code** and fills in a random one when you tick Story Co-op (you can type your own, or paste the code of an earlier playthrough to resume it). A client that sends none gets one from the server, so two unrelated parties whose admins share a nickname can never load each other's save. The key is chosen when Story Co-op is switched on and the matching file is loaded then, merged by union with anything the party already holds. Changing the code while Story Co-op stays on does nothing, switch it off and on again. To resume a playthrough after a server restart, create the party again with the same admin nickname and the same code (the admin's client remembers the last code). If another running party already uses the key, the second one is simply not persisted.
- **When:** a few seconds after the last change, when Story Co-op is switched off, when the party is dropped and on a clean shutdown (Ctrl+C or SIGTERM).
- **Safety:** each write goes to a temporary file, is flushed to disk and renamed over the old one. The file has a magic, a format version and a checksum. Up to three rotating backups (`.bak1` newest) are kept, rotated at most every 10 minutes.
- **Bad files:** a missing, truncated, corrupt or unknown version file is logged and skipped, falling back to the newest good backup, then to an empty team save. The server never stops because of it.

## Audit

`STORY_COOP_SYNC_AUDIT.md` lists every part of the save file and whether it is shared, personal or run state, with the reason and the code that shows it.

## Code map

- Client: `soh/soh/Network/ZeldaOnline/ZeldaOnlineStoryCoop.cpp`, plus additions in `ZeldaOnlineClient.*`, `PartySettings.hpp` and `ZeldaOnlineRoomWindow.hpp`.
- Server: `server/src` (party team save, vote and era state machine, story packets).
- Vanilla hooks: `VB_MASTER_SWORD_PEDESTAL_USE` (`z_bg_toki_swd.c`) and `VB_SWITCH_AGE` (`SwitchAge.cpp`).

## Credits

- ShipwrightOnline by lukegrahamSydney, the netcode and server this builds on.
- Ship of Harkinian (HarbourMasters), the PC port underneath.
- The Anchor based ocarina-coop project, whose ideas for shared items, flag sync and team save catch-up are ported here.
