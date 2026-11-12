#include "ZeldaOnlineVoice.hpp"
#include "ZeldaOnlineClient.hpp"
#include "ZeldaOnlineRoomWindow.hpp"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "soh/Enhancements/nametag.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/UIWidgets.hpp"

#include <ship/Context.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <imgui.h>
#include <SDL2/SDL.h>
#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

namespace ZeldaOnline {

using namespace Voice;

// Settings (all per player, saved with the other CVars)
static const char* CVAR_ENABLED = "gZeldaOnline.Voice.Enabled";    // 1: hear others and allow the mic
static const char* CVAR_MIC = "gZeldaOnline.Voice.Mic";            // 1: use the microphone
static const char* CVAR_MODE = "gZeldaOnline.Voice.Mode";          // 0 push to talk, 1 voice activation
static const char* CVAR_PTT_KEY = "gZeldaOnline.Voice.PttKey";     // SDL scancode
static const char* CVAR_PTT_PAD = "gZeldaOnline.Voice.PttPad";     // 0 none, 1 left stick click, 2 right stick click
static const char* CVAR_THRESHOLD = "gZeldaOnline.Voice.Threshold"; // voice activation level, dBFS
static const char* CVAR_RANGE = "gZeldaOnline.Voice.Range";        // game units where a voice fades out
static const char* CVAR_VOLUME = "gZeldaOnline.Voice.Volume";      // percent
static const char* CVAR_MIC_GAIN = "gZeldaOnline.Voice.MicGain";   // percent
static const char* CVAR_DEVICE = "gZeldaOnline.Voice.InputDevice"; // "" = system default
static const char* CVAR_MUTED = "gZeldaOnline.Voice.Muted";        // muted names, one per line
static const char* CVAR_TEST_TONE = "gZeldaOnline.Voice.TestTone"; // automated tests: send a sine of this many Hz

static const int DEFAULT_RANGE = 1500;
static const float NEAR_DIST = 250.0f; // full volume this close (about four Links apart)
static const int BITRATE = 24000;
static const uint32_t SPEAKING_HOLD_MS = 300;
static const uint32_t SPEAKER_FORGET_MS = 30000;

static void SaveCVarsSoon() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

VoiceChat& VoiceChat::Instance() {
    static VoiceChat instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Devices

static void SDLCALL CaptureThunk(void* user, Uint8* stream, int len) {
    static_cast<VoiceChat*>(user)->CaptureCallback(reinterpret_cast<const float*>(stream), len / (int)sizeof(float));
}

static void SDLCALL PlaybackThunk(void* user, Uint8* stream, int len) {
    static_cast<VoiceChat*>(user)->PlaybackCallback(reinterpret_cast<float*>(stream), len / (int)(2 * sizeof(float)));
}

void VoiceChat::OpenPlayback() {
    if (m_playDev != 0)
        return;
    SDL_AudioSpec want{}, have{};
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = FRAME_SAMPLES / 2; // 10 ms
    want.callback = PlaybackThunk;
    want.userdata = this;
    // don't retry a refused output device every frame
    if (SDL_GetTicks() < m_playRetryTicks)
        return;
    m_playDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (m_playDev == 0) {
        m_playError = SDL_GetError();
        m_playRetryTicks = SDL_GetTicks() + 5000;
        SPDLOG_WARN("[Voice] could not open the speakers: {}", m_playError);
        return;
    }
    m_playError.clear();
    SDL_PauseAudioDevice(m_playDev, 0);
}

void VoiceChat::ClosePlayback() {
    if (m_playDev == 0)
        return;
    SDL_CloseAudioDevice(m_playDev);
    m_playDev = 0;
}

void VoiceChat::OpenCapture(const std::string& device) {
    if (m_capDev != 0 && device == m_capDevName)
        return;
    CloseCapture();
    // don't hammer a missing or refused microphone every frame
    if (SDL_GetTicks() < m_capRetryTicks)
        return;
    SDL_AudioSpec want{}, have{};
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = FRAME_SAMPLES / 2;
    want.callback = CaptureThunk;
    want.userdata = this;
    m_capDev = SDL_OpenAudioDevice(device.empty() ? nullptr : device.c_str(), 1, &want, &have, 0);
    if (m_capDev == 0) {
        m_capError = SDL_GetError();
        m_capRetryTicks = SDL_GetTicks() + 5000;
        SPDLOG_WARN("[Voice] could not open the microphone: {}", m_capError);
        return;
    }
    m_capError.clear();
    m_capDevName = device;
    {
        std::lock_guard<std::mutex> lock(m_capMutex);
        m_capSamples.clear();
    }
    SDL_PauseAudioDevice(m_capDev, 0);
}

void VoiceChat::CloseCapture() {
    if (m_capDev == 0)
        return;
    SDL_CloseAudioDevice(m_capDev);
    m_capDev = 0;
    m_capDevName.clear();
    std::lock_guard<std::mutex> lock(m_capMutex);
    m_capSamples.clear();
}

void VoiceChat::CaptureCallback(const float* samples, int count) {
    std::lock_guard<std::mutex> lock(m_capMutex);
    m_capSamples.insert(m_capSamples.end(), samples, samples + count);
    // keep at most a second: the game thread was held up, old speech is no use
    if (m_capSamples.size() > (size_t)SAMPLE_RATE)
        m_capSamples.erase(m_capSamples.begin(), m_capSamples.end() - SAMPLE_RATE / 5);
}

void VoiceChat::PlaybackCallback(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * 2 * frames);
    std::lock_guard<std::mutex> lock(m_mixMutex);
    const uint32_t now = SDL_GetTicks();
    for (auto& entry : m_speakers) {
        Speaker& s = *entry.second;
        // gains glide to their targets over this buffer so movement doesn't click
        const float l0 = s.curLeft, r0 = s.curRight;
        const float l1 = RampGain(s.curLeft, s.targetLeft), r1 = RampGain(s.curRight, s.targetRight);
        double power = 0.0;
        bool played = false;
        for (int i = 0; i < frames; i++) {
            if (s.framePos >= FRAME_SAMPLES) {
                s.playing = s.buffer.Pop(s.frame);
                s.framePos = 0;
                if (s.playing)
                    s.framesPlayed++;
            }
            if (!s.playing) {
                // nothing queued: wait for the next callback rather than burning the buffer's start position
                s.framePos = FRAME_SAMPLES;
                break;
            }
            const float t = frames > 1 ? (float)i / (float)(frames - 1) : 1.0f;
            const float v = s.frame[s.framePos++];
            const float l = v * (l0 + (l1 - l0) * t);
            const float r = v * (r0 + (r1 - r0) * t);
            out[2 * i] += l;
            out[2 * i + 1] += r;
            power += 0.5 * ((double)l * l + (double)r * r);
            played = true;
        }
        s.curLeft = l1;
        s.curRight = r1;
        const double framePower = frames > 0 ? power / frames : 0.0;
        s.outPower = s.outPower * 0.9 + framePower * 0.1;
        if (played)
            s.lastPlayedTicks = now;
    }
    for (int i = 0; i < 2 * frames; i++)
        out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
}

// ---------------------------------------------------------------------------------------------------------------
// Settings helpers

static std::vector<std::string> MutedNames() {
    std::vector<std::string> names;
    std::istringstream in(CVarGetString(CVAR_MUTED, ""));
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            names.push_back(line);
    return names;
}

bool VoiceChat::IsMuted(const std::string& name) {
    if (name.empty())
        return false;
    for (const auto& n : MutedNames())
        if (n == name)
            return true;
    return false;
}

void VoiceChat::SetMuted(const std::string& name, bool muted) {
    std::vector<std::string> names = MutedNames();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    if (muted)
        names.push_back(name);
    std::string joined;
    for (const auto& n : names)
        joined += n + "\n";
    CVarSetString(CVAR_MUTED, joined.c_str());
    SaveCVarsSoon();
}

static SDL_GameControllerButton PadButton(int setting) {
    switch (setting) {
        case 1:
            return SDL_CONTROLLER_BUTTON_LEFTSTICK;
        case 2:
            return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
        default:
            return SDL_CONTROLLER_BUTTON_INVALID;
    }
}

bool VoiceChat::PushToTalkHeld() {
    // keyboard, while the game window has focus and no text box is being typed in
    const int key = CVarGetInteger(CVAR_PTT_KEY, SDL_SCANCODE_V);
    if (key > SDL_SCANCODE_UNKNOWN && key < SDL_NUM_SCANCODES && SDL_GetKeyboardFocus() != nullptr &&
        !(ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput)) {
        int numKeys = 0;
        const Uint8* keys = SDL_GetKeyboardState(&numKeys);
        if (keys != nullptr && key < numKeys && keys[key])
            return true;
    }
    // controller button, on any controller the game has open
    const SDL_GameControllerButton button = PadButton(CVarGetInteger(CVAR_PTT_PAD, 2));
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i))
                continue;
            SDL_GameController* pad = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
            if (pad != nullptr && SDL_GameControllerGetButton(pad, button))
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Sending

void VoiceChat::ReadMicrophone(ZeldaOnlineClient* client, bool wantSend) {
    const bool vad = CVarGetInteger(CVAR_MODE, 0) == 1;
    const float threshold = (float)CVarGetInteger(CVAR_THRESHOLD, -45);
    const float micGain = CVarGetInteger(CVAR_MIC_GAIN, 100) / 100.0f;
    const bool ptt = !vad && PushToTalkHeld();

    // take whole 20 ms frames from the microphone, or make them from the test tone at the real rate
    std::vector<float> samples;
    if (m_testTone > 0) {
        const uint32_t now = SDL_GetTicks();
        if (m_toneTicks == 0)
            m_toneTicks = now;
        const int frames = (int)((now - m_toneTicks) / FRAME_MS);
        m_toneTicks += frames * FRAME_MS;
        samples.resize((size_t)std::min(frames, 25) * FRAME_SAMPLES);
        const double step = 2.0 * M_PI * m_testTone / SAMPLE_RATE;
        for (auto& v : samples) {
            v = (float)(0.25 * std::sin(m_tonePhase));
            m_tonePhase = std::fmod(m_tonePhase + step, 2.0 * M_PI);
        }
    } else {
        std::lock_guard<std::mutex> lock(m_capMutex);
        const size_t whole = (m_capSamples.size() / FRAME_SAMPLES) * FRAME_SAMPLES;
        samples.assign(m_capSamples.begin(), m_capSamples.begin() + whole);
        m_capSamples.erase(m_capSamples.begin(), m_capSamples.begin() + whole);
    }

    for (size_t off = 0; off + FRAME_SAMPLES <= samples.size(); off += FRAME_SAMPLES) {
        float* pcm = samples.data() + off;
        if (micGain != 1.0f)
            for (int i = 0; i < FRAME_SAMPLES; i++)
                pcm[i] = std::max(-1.0f, std::min(1.0f, pcm[i] * micGain));
        m_micDb = FrameDb(pcm, FRAME_SAMPLES);

        // the automated test tone always talks; otherwise the key or the voice gate decides
        bool talk = m_testTone > 0 ? true : vad ? m_gate.Process(m_micDb, threshold) : ptt;
        talk = talk && wantSend;

        if (!talk) {
            if (m_transmitting) {
                m_transmitting = false;
                SendFrames(client, true);
            }
            continue;
        }
        m_transmitting = true;

        unsigned char bytes[MAX_OPUS_FRAME_BYTES];
        const int n = opus_encode_float(m_encoder, pcm, FRAME_SAMPLES, bytes, MAX_OPUS_FRAME_BYTES);
        if (n <= 0)
            continue; // can't happen at this bitrate; skip rather than send a broken frame
        m_pendingFrames.emplace_back(bytes, bytes + n);
        if (m_loopback)
            Loopback(pcm);
        if ((int)m_pendingFrames.size() >= MAX_FRAMES_PER_PACKET)
            SendFrames(client, false);
    }
    if (!m_pendingFrames.empty())
        SendFrames(client, false);
    if (!wantSend && m_transmitting) {
        m_transmitting = false;
        SendFrames(client, true);
    }
}

void VoiceChat::SendFrames(ZeldaOnlineClient* client, bool end) {
    if (m_pendingFrames.empty() && !end)
        return;
    VoicePacket p;
    p.flags = end ? VOICE_FLAG_END : 0;
    p.seq = m_sendSeq;
    p.frames.swap(m_pendingFrames);
    m_sendSeq = (uint16_t)(m_sendSeq + p.frames.size());
    std::vector<uint8_t> body;
    EncodeVoiceBody(p, body);
    ByteStream packet = newPacket(CLIENT_PACKET_VOICE);
    packet.Write(reinterpret_cast<const char*>(body.data()), (unsigned int)body.size());
    if (client->WritePacket(packet)) {
        m_framesSent += (uint32_t)p.frames.size();
        m_packetsSent++;
    }
}

void VoiceChat::Loopback(const float* pcm) {
    // "Hear yourself": this player's own voice, centred, through the same playback path
    std::lock_guard<std::mutex> lock(m_mixMutex);
    auto& slot = m_speakers[0];
    if (!slot) {
        slot = std::make_unique<Speaker>();
        slot->name = "(you)";
    }
    slot->targetLeft = slot->targetRight = CVarGetInteger(CVAR_VOLUME, 100) / 100.0f;
    slot->buffer.Push(pcm, false);
    slot->framesReceived++;
    slot->lastPacketTicks = SDL_GetTicks();
}

// ---------------------------------------------------------------------------------------------------------------
// Receiving

void VoiceChat::OnPacket(ZeldaOnlineClient* client, ByteStream& packet) {
    if (packet.BytesLeft() < 2)
        return;
    const int sender = (int)packet.Read<PackedUInt2>().value();
    ByteStream bodyStream = packet.Read(packet.BytesLeft());
    VoicePacket p;
    if (sender == 0 || !DecodeVoiceBody(reinterpret_cast<const uint8_t*>(bodyStream.Text()), bodyStream.Length(), p))
        return;
    if (!CVarGetInteger(CVAR_ENABLED, 1))
        return;

    std::unique_ptr<Speaker> fresh;
    Speaker* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mixMutex);
        auto it = m_speakers.find(sender);
        if (it != m_speakers.end())
            s = it->second.get();
    }
    if (s == nullptr) {
        fresh = std::make_unique<Speaker>();
        int err = 0;
        fresh->decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
        if (fresh->decoder == nullptr)
            return;
        s = fresh.get();
    }

    // decode outside the lock: the playback callback only waits for the queue itself
    std::vector<float> pcm;
    pcm.reserve(p.frames.size() * FRAME_SAMPLES);
    float frame[FRAME_SAMPLES];
    if (s->haveSeq && p.seq != s->nextSeq && !p.frames.empty()) {
        // TCP keeps everything in order, so a gap means frames were dropped on purpose (the server was backed up):
        // nothing to conceal, just note it
        s->framesLost += (uint16_t)(p.seq - s->nextSeq);
    }
    for (const auto& f : p.frames) {
        const int n = opus_decode_float(s->decoder, f.data(), (opus_int32)f.size(), frame, FRAME_SAMPLES, 0);
        if (n != FRAME_SAMPLES)
            continue;
        pcm.insert(pcm.end(), frame, frame + FRAME_SAMPLES);
    }
    s->nextSeq = (uint16_t)(p.seq + p.frames.size());
    s->haveSeq = true;

    std::lock_guard<std::mutex> lock(m_mixMutex);
    if (fresh) {
        s = fresh.get();
        m_speakers[sender] = std::move(fresh);
    }
    for (size_t off = 0; off + FRAME_SAMPLES <= pcm.size(); off += FRAME_SAMPLES)
        s->buffer.Push(pcm.data() + off, false);
    if (p.flags & VOICE_FLAG_END)
        s->buffer.MarkEnd();
    s->framesReceived += (uint32_t)(pcm.size() / FRAME_SAMPLES);
    s->lastPacketTicks = SDL_GetTicks();
}

void VoiceChat::SetSpeakingTag(Speaker& s, Actor* actor, bool speaking) {
    const uint32_t now = SDL_GetTicks();
    // the tag goes with its actor when that is destroyed, and a new puppet can get the same address: put it up
    // again every couple of seconds rather than trusting the pointer
    const bool stale = s.tagActor != nullptr && now - s.tagTicks > 2000;
    if (s.tagActor != nullptr && (s.tagActor != actor || !speaking || stale)) {
        NameTag_RemoveAllByActorTag(s.tagActor, "voice");
        s.tagActor = nullptr;
    }
    if (speaking && actor != nullptr && s.tagActor == nullptr) {
        NameTagOptions options{};
        options.tag = "voice";
        options.textColor = Color_RGBA8{ 120, 255, 140, 255 };
        options.yOffset = 24;
        options.scale = 0.5f;
        NameTag_RegisterForActorWithOptions(actor, "( talking )", options);
        s.tagActor = actor;
        s.tagTicks = now;
    }
}

void VoiceChat::UpdateSpeakers(ZeldaOnlineClient* client) {
    const float range = (float)std::max(NEAR_DIST + 1.0f, (float)CVarGetInteger(CVAR_RANGE, DEFAULT_RANGE));
    const float volume = CVarGetInteger(CVAR_VOLUME, 100) / 100.0f;
    const uint32_t now = SDL_GetTicks();

    Player* me = gPlayState != nullptr ? GET_PLAYER(gPlayState) : nullptr;
    float fx = 0.0f, fz = 1.0f;
    if (gPlayState != nullptr) {
        fx = gPlayState->view.lookAt.x - gPlayState->view.eye.x;
        fz = gPlayState->view.lookAt.z - gPlayState->view.eye.z;
    }

    std::lock_guard<std::mutex> lock(m_mixMutex);
    // a new scene drops every name tag: forget the old actors without touching them
    if ((void*)gPlayState != m_tagPlay) {
        m_tagPlay = gPlayState;
        for (auto& entry : m_speakers)
            entry.second->tagActor = nullptr;
    }
    for (auto it = m_speakers.begin(); it != m_speakers.end();) {
        Speaker& s = *it->second;
        const int id = it->first;
        if (id == 0) {
            // own mic test
            if (!m_loopback || now - s.lastPacketTicks > SPEAKER_FORGET_MS) {
                if (s.decoder != nullptr)
                    opus_decoder_destroy(s.decoder);
                it = m_speakers.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        AbstractActorController* ctl = client->GetNetworkController(id);
        Actor* actor = (ctl != nullptr && ctl->IsPlayer()) ? ctl->GetActor() : nullptr;

        if (ZeldaOnlineRoomWindow::Instance != nullptr) {
            if (const PlayerEntry* e = ZeldaOnlineRoomWindow::Instance->FindPlayerEntry((uint32_t)id))
                s.name = e->name;
        }
        s.muted = IsMuted(s.name);

        float gain = 0.0f, left = 0.0f, right = 0.0f;
        s.distance = -1.0f;
        if (actor != nullptr && me != nullptr && !s.muted) {
            const float dx = actor->world.pos.x - me->actor.world.pos.x;
            const float dy = actor->world.pos.y - me->actor.world.pos.y;
            const float dz = actor->world.pos.z - me->actor.world.pos.z;
            s.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            gain = DistanceGain(s.distance, NEAR_DIST, range) * volume;
            PanGains(dx, dz, fx, fz, NEAR_DIST, left, right);
            left *= gain;
            right *= gain;
        }
        s.gain = gain;
        s.left = left;
        s.right = right;
        s.targetLeft = left;
        s.targetRight = right;

        const bool speaking = gain > 0.01f && s.lastPlayedTicks != 0 && now - s.lastPlayedTicks < SPEAKING_HOLD_MS;
        SetSpeakingTag(s, actor, speaking);

        // gone quiet and no longer around: forget them (their decoder too)
        if (now - s.lastPacketTicks > SPEAKER_FORGET_MS && s.buffer.Queued() == 0) {
            SetSpeakingTag(s, nullptr, false);
            if (s.decoder != nullptr)
                opus_decoder_destroy(s.decoder);
            it = m_speakers.erase(it);
            continue;
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------------------------------------------

void VoiceChat::Update(ZeldaOnlineClient* client) {
    const bool enabled = CVarGetInteger(CVAR_ENABLED, 1) != 0;
    const bool connected = client != nullptr && client->IsConnectedToServer();
    m_testTone = CVarGetInteger(CVAR_TEST_TONE, 0);
    if (m_testTone <= 0)
        m_toneTicks = 0;

    if (!enabled || !connected) {
        if (m_transmitting && connected) {
            m_transmitting = false;
            SendFrames(client, true);
        }
        Reset();
        return;
    }

    OpenPlayback();

    const bool micWanted = CVarGetInteger(CVAR_MIC, 1) != 0;
    if (micWanted && m_testTone <= 0)
        OpenCapture(CVarGetString(CVAR_DEVICE, ""));
    else
        CloseCapture();

    if (m_encoder == nullptr) {
        int err = 0;
        m_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (m_encoder != nullptr) {
            opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(BITRATE));
            opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        }
    }

    if (m_encoder != nullptr && (m_capDev != 0 || m_testTone > 0)) {
        // nothing to send to before the server knows who we are
        ReadMicrophone(client, micWanted || m_testTone > 0);
    } else if (m_transmitting) {
        m_transmitting = false;
        SendFrames(client, true);
    }

    UpdateSpeakers(client);
}

void VoiceChat::Reset() {
    CloseCapture();
    ClosePlayback(); // the callback can't run past this, so the speakers can go
    std::lock_guard<std::mutex> lock(m_mixMutex);
    for (auto& entry : m_speakers) {
        if (entry.second->tagActor != nullptr && gPlayState != nullptr && (void*)gPlayState == m_tagPlay)
            NameTag_RemoveAllByActorTag(entry.second->tagActor, "voice");
        if (entry.second->decoder != nullptr)
            opus_decoder_destroy(entry.second->decoder);
    }
    m_speakers.clear();
    m_pendingFrames.clear();
    m_transmitting = false;
    m_gate.Reset();
    m_micDb = -100.0f;
}

void VoiceChat::OnDisconnected() {
    Reset();
}

nlohmann::json VoiceChat::TestStats() {
    nlohmann::json o;
    o["mic_open"] = MicOpen();
    o["transmitting"] = m_transmitting;
    o["frames_sent"] = m_framesSent;
    o["packets_sent"] = m_packetsSent;
    o["output_open"] = m_playDev != 0;
    nlohmann::json speakers = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(m_mixMutex);
    for (auto& entry : m_speakers) {
        const Speaker& s = *entry.second;
        nlohmann::json sj;
        sj["id"] = entry.first;
        sj["name"] = s.name;
        sj["frames_received"] = s.framesReceived;
        sj["frames_played"] = s.framesPlayed;
        sj["frames_lost"] = s.framesLost;
        sj["queued"] = s.buffer.Queued();
        sj["underruns"] = s.buffer.Underruns();
        sj["gain"] = s.gain;
        sj["left"] = s.left;
        sj["right"] = s.right;
        sj["distance"] = s.distance;
        sj["muted"] = s.muted;
        // level of what actually came out of the speakers for this player, in dBFS
        sj["out_db"] = s.outPower > 1e-10 ? 10.0 * std::log10(s.outPower) : -100.0;
        sj["speaking_tag"] = s.tagActor != nullptr;
        speakers.push_back(sj);
    }
    o["speakers"] = speakers;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings menu (Network > Zelda Online)

static bool sWaitingForKey = false;

void VoiceChat::DrawSettings() {
    ImGui::SeparatorText("Voice Chat");

    UIWidgets::CVarCheckbox("Proximity voice chat", CVAR_ENABLED,
                            UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                                "Talk to the players near you. Their voices get quieter the further away their Link "
                                "is and come from their side of the screen. Only players in the same area can hear "
                                "you."));
    if (!CVarGetInteger(CVAR_ENABLED, 1))
        return;

    UIWidgets::CVarCheckbox("Use my microphone", CVAR_MIC,
                            UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                                "Off: you only listen. Nobody can hear you."));

    if (CVarGetInteger(CVAR_MIC, 1)) {
        // microphone choice
        std::vector<std::string> devices;
        const int count = SDL_GetNumAudioDevices(1);
        for (int i = 0; i < count; i++)
            if (const char* name = SDL_GetAudioDeviceName(i, 1))
                devices.emplace_back(name);
        std::string current = CVarGetString(CVAR_DEVICE, "");
        ImGui::TextUnformatted("Microphone");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##VoiceMic", current.empty() ? "System default" : current.c_str())) {
            if (ImGui::Selectable("System default", current.empty())) {
                CVarSetString(CVAR_DEVICE, "");
                SaveCVarsSoon();
            }
            for (const auto& d : devices) {
                if (ImGui::Selectable(d.c_str(), d == current)) {
                    CVarSetString(CVAR_DEVICE, d.c_str());
                    SaveCVarsSoon();
                }
            }
            ImGui::EndCombo();
        }
        if (!m_capError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Microphone unavailable: %s", m_capError.c_str());

        static const char* modes[] = { "Push to talk", "Voice activation" };
        UIWidgets::CVarCombobox("Talk with", CVAR_MODE, modes,
                                UIWidgets::ComboboxOptions().DefaultIndex(0).Tooltip(
                                    "Push to talk: hold the key or button below while you speak.\nVoice activation: "
                                    "sends whenever you're louder than the level below. Use headphones, or the "
                                    "others will hear themselves through your microphone."));

        if (CVarGetInteger(CVAR_MODE, 0) == 0) {
            const int key = CVarGetInteger(CVAR_PTT_KEY, SDL_SCANCODE_V);
            std::string label = sWaitingForKey ? std::string("Press a key... (Backspace for none)")
                                               : std::string(key > 0 ? SDL_GetScancodeName((SDL_Scancode)key) : "None");
            ImGui::TextUnformatted("Push to talk key");
            UIWidgets::Tooltip("Click, then press the key to hold while you talk.");
            if (ImGui::Button((label + "##VoicePttKey").c_str(), ImVec2(-1.0f, 0.0f)))
                sWaitingForKey = true;
            if (sWaitingForKey) {
                int numKeys = 0;
                const Uint8* keys = SDL_GetKeyboardState(&numKeys);
                for (int k = SDL_SCANCODE_A; keys != nullptr && k < numKeys; k++) {
                    if (!keys[k])
                        continue;
                    CVarSetInteger(CVAR_PTT_KEY, k == SDL_SCANCODE_BACKSPACE ? 0 : k);
                    SaveCVarsSoon();
                    sWaitingForKey = false;
                    break;
                }
            }
            static const char* pads[] = { "None", "Left stick click", "Right stick click" };
            UIWidgets::CVarCombobox("Controller button", CVAR_PTT_PAD, pads,
                                    UIWidgets::ComboboxOptions().DefaultIndex(2).Tooltip(
                                        "Hold this on your controller to talk."));
        } else {
            UIWidgets::CVarSliderInt("Activation level: %d dB", CVAR_THRESHOLD,
                                     UIWidgets::IntSliderOptions().Min(-70).Max(-10).DefaultValue(-45).Tooltip(
                                         "Lower picks up quieter speech, higher ignores more background noise. Watch "
                                         "the meter below while you talk."));
        }

        UIWidgets::CVarSliderInt("Microphone boost: %d%%", CVAR_MIC_GAIN,
                                 UIWidgets::IntSliderOptions().Min(25).Max(400).DefaultValue(100));

        // level meter, with the activation level marked
        const float level = std::max(0.0f, std::min(1.0f, (m_micDb + 70.0f) / 70.0f));
        char meter[64];
        snprintf(meter, sizeof(meter), "%s  %.0f dB", m_transmitting ? "Sending" : "Mic", m_micDb);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              m_transmitting ? ImVec4(0.35f, 0.85f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
        ImGui::ProgressBar(MicOpen() ? level : 0.0f, ImVec2(-1.0f, 0.0f), MicOpen() ? meter : "Not online");
        ImGui::PopStyleColor();

        UIWidgets::Checkbox("Hear myself (mic test)", &m_loopback,
                            UIWidgets::CheckboxOptions().Tooltip("Plays your own microphone back to you. Not saved."));
    }

    if (!m_playError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Can't play voices: %s", m_playError.c_str());
    UIWidgets::CVarSliderInt("Voice volume: %d%%", CVAR_VOLUME,
                             UIWidgets::IntSliderOptions().Min(0).Max(200).DefaultValue(100));
    UIWidgets::CVarSliderInt("Hearing range: %d", CVAR_RANGE,
                             UIWidgets::IntSliderOptions().Min(500).Max(5000).Step(100).DefaultValue(DEFAULT_RANGE).Tooltip(
                                 "How far away (in game units) a voice fades out. Players within about 250 are at "
                                 "full volume. 1500 is roughly across Kokiri Forest's central clearing."));

    // per player mute, for everyone this player can hear right now
    std::vector<std::pair<int, std::string>> people;
    {
        std::lock_guard<std::mutex> lock(m_mixMutex);
        for (auto& entry : m_speakers)
            if (entry.first != 0 && !entry.second->name.empty())
                people.emplace_back(entry.first, entry.second->name);
    }
    std::vector<std::string> muted = MutedNames();
    if (!people.empty() || !muted.empty()) {
        ImGui::TextUnformatted("Mute");
        for (auto& p : people) {
            bool m = IsMuted(p.second);
            if (UIWidgets::Checkbox((p.second + "##VoiceMute" + std::to_string(p.first)).c_str(), &m))
                SetMuted(p.second, m);
        }
        for (auto& name : muted) {
            if (std::any_of(people.begin(), people.end(), [&](auto& p) { return p.second == name; }))
                continue;
            bool m = true;
            if (UIWidgets::Checkbox((name + "##VoiceMuteName").c_str(), &m))
                SetMuted(name, m);
        }
    }
}

static void DrawVoiceMenu(WidgetInfo& info) {
    if (!CVarGetInteger("gZeldaOnline.Enabled", 1))
        return;
    VoiceChat::Instance().DrawSettings();
}

static void RegisterVoiceMenu() {
    WidgetPath path = { "Network", "Zelda Online", SECTION_COLUMN_2 };
    SohGui::mSohMenu->AddWidget(path, "ZeldaOnlineVoiceMenu", WIDGET_CUSTOM).CustomFunction(DrawVoiceMenu).HideInSearch(true);
}

static RegisterMenuInitFunc voiceMenuInitFunc(RegisterVoiceMenu);

} // namespace ZeldaOnline
