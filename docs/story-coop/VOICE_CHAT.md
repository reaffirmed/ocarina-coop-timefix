# Proximity voice chat

Talk to the players near you while playing online. A voice gets quieter the further away that player's Link is,
fades out completely at the hearing range, and comes from their side of the screen. Only players in the same scene
(and, with a party, the same party) can hear you.

## Using it

**Settings > Network > Zelda Online**, in the **Voice Chat** column. Everything is per player and saved with your
other settings.

| Setting | Default | What it does |
| --- | --- | --- |
| Proximity voice chat | on | Off: you hear nobody and nobody hears you. |
| Use my microphone | on | Off: you only listen. |
| Microphone | System default | Which input device to use. |
| Talk with | Push to talk | **Push to talk**: hold the key (default **V**) or the controller button (default **right stick click**) while you speak. **Voice activation**: sends whenever you're louder than the activation level. |
| Activation level | -45 dB | Voice activation only. Watch the meter while you talk and set it just below your speaking level. |
| Microphone boost | 100% | Makes a quiet microphone louder. |
| Hear myself (mic test) | off | Plays your own voice back to you, to check the microphone and level. Not saved. |
| Voice volume | 100% | How loud other players are. |
| Hearing range | 1500 | How far away (game units) a voice fades out. Within 250 (about four Links apart) it is at full volume. |
| Mute | | Appears once someone has talked near you. Muting is by player name and is remembered. |

While you're sending, **Talking** shows at the top of your screen. Players who are talking near you have a green
**( talking )** tag above their name.

Use headphones with voice activation. Without them your microphone picks up the other players from your speakers and
they hear themselves echo. Push to talk avoids most of that.

On macOS the system asks for microphone permission the first time you go online with voice chat on. If you said no,
allow it in System Settings > Privacy & Security > Microphone.

## How it works

- **Capture**: SDL opens the microphone at 48 kHz mono (SDL converts from whatever the device does). The game takes
  whole 20 ms frames once per game frame.
- **Codec**: Opus (already linked for the game's own music), voice mode, 24 kbit/s, so a frame is 60 to 90 bytes.
- **Transport**: `CLIENT_PACKET_VOICE` over the game's existing server connection, so it works by address and through
  OOTSteamLink with nothing extra to open. Frames read in the same game frame go out together: flags (end of a talk
  spurt), the sequence number of the first frame, the frame count, then each frame's length and bytes.
- **Server**: checks the framing, limits each player to 12 KB/s (with a 24 KB burst), and passes it on as
  `SERVER_PACKET_VOICE` (sender id first) to the other players in the sender's scene. A receiver whose connection
  is more than 32 KB behind is skipped for voice, so voice never holds up the game's own packets. Older servers
  ignore the packet, older clients ignore the relay.
- **Playback**: each speaker has an Opus decoder and a small jitter buffer: playback starts once 100 ms are queued,
  and a queue that grows past 300 ms is cut back so the delay stays short. Voices are mixed on a separate SDL output
  device, so the game's own audio code is untouched. Volume and panning are worked out from the two Links' positions
  and the camera each game frame and glide to the new value over one audio buffer, so movement doesn't click.

The code is in `soh/soh/Network/ZeldaOnline/ZeldaOnlineVoice.cpp` (devices, codec, game side, menu) and
`VoiceCore.hpp` (packet format, volume by distance, panning, voice gate, jitter buffer), and the relay is the
`CLIENT_PACKET_VOICE` case in `server/src/OOTServer.cpp`.

## Limits

- No echo cancellation or noise suppression beyond the voice gate. Use headphones, or push to talk.
- Walls don't block sound: distance is a straight line, through floors and walls.
- Everyone in the same scene of the same party (or everyone outside any party in that scene) is in range of each
  other; there is no team or whisper channel.

## Testing

`tools/coop-test/unit/voice_core_test.cpp` checks the packet format (including malformed input), the distance and
pan curves, the voice gate, the jitter buffer and an Opus round trip. Build line at the top of the file.

`tools/coop-test/scenarios/voice.json` runs two real clients with no microphone at all: client B sends a 440 Hz test
tone (`gZeldaOnline.Voice.TestTone`) and client A measures what it plays for B. B walks to several spots around A, and
the scenario checks full volume up close, quieter further away, which side the voice comes from, silence out of range
and when muted, the talking tag, and that A (microphone off) sends nothing. Each observation has a `voice` block
(frames sent, and per speaker the frames received and played, gain, left/right, distance and output level in dBFS).
It needs an audio system that lets a program open a second output device: any desktop is fine, and on a headless
Linux machine a PulseAudio null sink works (`pulseaudio -D --exit-idle-time=-1 --load="module-null-sink sink_name=out"`).
SDL's `dummy` driver allows only one device, which the game itself takes, so voices can't be played there.

Two test steps came with it: `set_cvar` (`name` and `int` or `string`, changes a setting the way its menu would) and
`move_link` (`dx`/`dy`/`dz`; `mark` records where Link stands, `from_mark` moves relative to that spot instead of the
current one).

What the automated tests can't cover: real microphones, how it sounds, echo, and Windows/macOS audio devices. Try
it with two people on two machines.
