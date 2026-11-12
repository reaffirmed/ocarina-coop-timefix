#ifndef ZELDAONLINE_VOICE_HPP
#define ZELDAONLINE_VOICE_HPP

// Proximity voice chat. The microphone is read with SDL, compressed with Opus and sent over the game's own server
// connection (so it also works through OOTSteamLink). The server passes it on to the players in the sender's scene.
// Each speaker is played on a separate SDL output device, louder the closer their Link is and panned to their side
// of the screen. See docs/story-coop/VOICE_CHAT.md.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ByteStream.hpp"
#include "VoiceCore.hpp"

struct OpusEncoder;
struct OpusDecoder;
struct Actor;

namespace ZeldaOnline {

class ZeldaOnlineClient;

class VoiceChat {
  public:
    static VoiceChat& Instance();

    // Once per game frame while online, before the network is polled
    void Update(ZeldaOnlineClient* client);
    // SERVER_PACKET_VOICE (the packet id is already read)
    void OnPacket(ZeldaOnlineClient* client, ByteStream& packet);
    // The connection is gone: stop the microphone and forget every speaker
    void OnDisconnected();

    bool Transmitting() const {
        return m_transmitting;
    }
    bool MicOpen() const {
        return m_capDev != 0 || m_testTone > 0;
    }

    // For the settings menu
    void DrawSettings();
    // For the coop-test observations
    nlohmann::json TestStats();

    // The SDL callbacks
    void CaptureCallback(const float* samples, int count);
    void PlaybackCallback(float* out, int frames);

    // Names muted by this player, kept in gZeldaOnline.Voice.Muted
    static bool IsMuted(const std::string& name);
    static void SetMuted(const std::string& name, bool muted);

  private:
    VoiceChat() = default;

    struct Speaker {
        OpusDecoder* decoder = nullptr;
        Voice::JitterBuffer buffer;
        float frame[Voice::FRAME_SAMPLES] = {}; // the frame being played
        int framePos = Voice::FRAME_SAMPLES;    // next sample of `frame`; FRAME_SAMPLES = need a new one
        bool playing = false;                   // `frame` holds speech (not a gap)
        uint16_t nextSeq = 0;
        bool haveSeq = false;
        // set on the game thread, read by the playback callback (both under m_mixMutex)
        float targetLeft = 0.0f, targetRight = 0.0f;
        float curLeft = 0.0f, curRight = 0.0f;
        // statistics
        uint32_t framesReceived = 0;
        uint32_t framesPlayed = 0;
        uint32_t framesLost = 0;
        double outPower = 0.0; // smoothed power of what was played for this speaker, after its gains
        uint32_t lastPlayedTicks = 0;
        uint32_t lastPacketTicks = 0;
        float gain = 0.0f, left = 0.0f, right = 0.0f, distance = -1.0f;
        std::string name;
        bool muted = false;
        Actor* tagActor = nullptr; // the puppet that shows the speaking tag
        uint32_t tagTicks = 0;     // when it was put up (it is put up again now and then, see SetSpeakingTag)
    };

    void OpenPlayback();
    void ClosePlayback();
    void OpenCapture(const std::string& device);
    void CloseCapture();
    bool PushToTalkHeld();
    void ReadMicrophone(ZeldaOnlineClient* client, bool wantSend);
    void SendFrames(ZeldaOnlineClient* client, bool end);
    void UpdateSpeakers(ZeldaOnlineClient* client);
    void SetSpeakingTag(Speaker& s, Actor* actor, bool speaking);
    void Loopback(const float* pcm);
    void Reset();

    // devices (SDL_AudioDeviceID)
    uint32_t m_playDev = 0;
    uint32_t m_playRetryTicks = 0;
    std::string m_playError;
    uint32_t m_capDev = 0;
    std::string m_capDevName;
    uint32_t m_capRetryTicks = 0;
    std::string m_capError;

    // microphone side (m_capMutex guards m_capSamples)
    std::mutex m_capMutex;
    std::vector<float> m_capSamples;
    OpusEncoder* m_encoder = nullptr;
    Voice::VoiceGate m_gate;
    bool m_transmitting = false;
    uint16_t m_sendSeq = 0;
    std::vector<std::vector<uint8_t>> m_pendingFrames;
    float m_micDb = -100.0f; // level of the last frame, for the meter
    uint32_t m_framesSent = 0;
    uint32_t m_packetsSent = 0;
    int m_testTone = 0; // gZeldaOnline.Voice.TestTone: a sine in Hz instead of the microphone (automated tests)
    uint32_t m_toneTicks = 0;
    double m_tonePhase = 0.0;

    // playback side (m_mixMutex guards m_speakers' buffers and gains)
    std::mutex m_mixMutex;
    std::map<int, std::unique_ptr<Speaker>> m_speakers; // by network id; 0 is this player's own mic test
    bool m_loopback = false;
    void* m_tagPlay = nullptr; // the PlayState the speaking tags belong to
    OpusDecoder* m_loopDecoder = nullptr;
};

} // namespace ZeldaOnline

#endif
