#ifndef ZELDAONLINE_VOICECORE_HPP
#define ZELDAONLINE_VOICECORE_HPP

// Proximity voice chat: the parts that need neither SDL nor the game, so tools/coop-test/unit/voice_core_test.cpp can
// check them on their own. ZeldaOnlineVoice.cpp does the devices, the codec and the game side.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ZeldaOnline {
namespace Voice {

// 48 kHz mono, 20 ms frames: what Opus is set up for
static const int SAMPLE_RATE = 48000;
static const int FRAME_SAMPLES = 960;
static const int FRAME_MS = 20;

// Wire format of CLIENT_PACKET_VOICE (and SERVER_PACKET_VOICE after the sender's network id):
//   u1 flags (VOICE_FLAG_*), u2 sequence of the first frame, u1 frame count, then per frame u1 length + Opus bytes
static const uint8_t VOICE_FLAG_END = 1; // the last frame of a talk spurt: the receiver plays out what it has
static const int MAX_FRAMES_PER_PACKET = 12;
static const int MAX_OPUS_FRAME_BYTES = 255;
static const int MAX_PACKET_BODY = 4 + MAX_FRAMES_PER_PACKET * (1 + MAX_OPUS_FRAME_BYTES);

struct VoicePacket {
    uint8_t flags = 0;
    uint16_t seq = 0;
    std::vector<std::vector<uint8_t>> frames;
};

inline void EncodeVoiceBody(const VoicePacket& p, std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(p.flags);
    out.push_back((uint8_t)(p.seq & 0xFF));
    out.push_back((uint8_t)(p.seq >> 8));
    out.push_back((uint8_t)p.frames.size());
    for (const auto& f : p.frames) {
        out.push_back((uint8_t)f.size());
        out.insert(out.end(), f.begin(), f.end());
    }
}

// False on anything malformed (the server relays only bodies that parse, and the client drops the rest)
inline bool DecodeVoiceBody(const uint8_t* data, size_t len, VoicePacket& out) {
    out.frames.clear();
    if (len < 4)
        return false;
    out.flags = data[0];
    out.seq = (uint16_t)(data[1] | (data[2] << 8));
    const int count = data[3];
    if (count > MAX_FRAMES_PER_PACKET)
        return false;
    size_t pos = 4;
    for (int i = 0; i < count; i++) {
        if (pos >= len)
            return false;
        const size_t n = data[pos++];
        if (n == 0 || pos + n > len)
            return false;
        out.frames.emplace_back(data + pos, data + pos + n);
        pos += n;
    }
    return pos == len && (count > 0 || (out.flags & VOICE_FLAG_END));
}

// Loudness of a speaker by the distance between the two Links (game units). Full volume up to `nearDist`, then a
// smooth fade to silence at `range`.
inline float DistanceGain(float dist, float nearDist, float range) {
    if (range <= nearDist)
        return dist <= range ? 1.0f : 0.0f;
    if (dist <= nearDist)
        return 1.0f;
    if (dist >= range)
        return 0.0f;
    const float t = (dist - nearDist) / (range - nearDist);
    return (1.0f - t) * (1.0f - t);
}

// Left/right gains for a speaker at (dx, dz) from the listener, with the camera looking along (fx, fz) in the
// horizontal plane. The game's camera is right handed with y up, so screen right is (-fz, fx). Close speakers stay
// centred, and the far ear never drops below 30%.
inline void PanGains(float dx, float dz, float fx, float fz, float nearDist, float& left, float& right) {
    left = right = 1.0f;
    const float d = std::sqrt(dx * dx + dz * dz);
    const float f = std::sqrt(fx * fx + fz * fz);
    if (d < 1.0f || f < 1e-4f)
        return;
    const float rx = -fz / f, rz = fx / f;
    float pan = (dx * rx + dz * rz) / d; // -1 left, 1 right
    pan *= std::min(1.0f, d / std::max(1.0f, nearDist));
    pan = std::max(-1.0f, std::min(1.0f, pan));
    if (pan > 0.0f)
        left = 1.0f - 0.7f * pan;
    else
        right = 1.0f + 0.7f * pan;
}

// Level of a frame in dBFS (-100 for silence)
inline float FrameDb(const float* s, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += (double)s[i] * s[i];
    const double rms = n > 0 ? std::sqrt(sum / n) : 0.0;
    return rms < 1e-5 ? -100.0f : (float)(20.0 * std::log10(rms));
}

// Voice activation: opens on a frame above the threshold, stays open `hangFrames` after the last loud frame
class VoiceGate {
  public:
    bool Process(float db, float thresholdDb, int hangFrames = 15) {
        if (db >= thresholdDb) {
            m_hang = hangFrames;
            m_open = true;
        } else if (m_hang > 0) {
            m_hang--;
        } else {
            m_open = false;
        }
        return m_open;
    }
    void Reset() {
        m_open = false;
        m_hang = 0;
    }

  private:
    bool m_open = false;
    int m_hang = 0;
};

// Decoded frames of one speaker waiting for the speakers. Frames arrive in bursts (both games talk to the server once
// per game frame, about every 50 ms), so playback starts only once `targetFrames` are queued, and starts over that
// way after running dry in the middle of a talk spurt. A queue that grows past `maxFrames` (clock drift, a stall)
// drops its oldest frames back to the target so the delay stays short.
class JitterBuffer {
  public:
    JitterBuffer(int targetFrames = 5, int maxFrames = 15) : m_target(targetFrames), m_max(maxFrames) {
    }

    void Push(const float* pcm, bool endOfSpurt) {
        m_frames.emplace_back(pcm, pcm + FRAME_SAMPLES);
        if (endOfSpurt)
            m_draining = true; // play out everything, no need to wait for more
        if ((int)m_frames.size() > m_max) {
            while ((int)m_frames.size() > m_target)
                m_frames.pop_front();
            m_dropped++;
        }
    }

    void MarkEnd() {
        m_draining = true;
    }

    // Copies one frame into `out` (FRAME_SAMPLES). False when there is nothing to play right now.
    bool Pop(float* out) {
        if (m_buffering) {
            if ((int)m_frames.size() < m_target && !(m_draining && !m_frames.empty()))
                return false;
            m_buffering = false;
        }
        if (m_frames.empty()) {
            m_buffering = true;
            if (!m_draining)
                m_underruns++;
            m_draining = false;
            return false;
        }
        std::copy(m_frames.front().begin(), m_frames.front().end(), out);
        m_frames.pop_front();
        return true;
    }

    int Queued() const {
        return (int)m_frames.size();
    }
    int Underruns() const {
        return m_underruns;
    }
    int Dropped() const {
        return m_dropped;
    }
    void Clear() {
        m_frames.clear();
        m_buffering = true;
        m_draining = false;
    }

  private:
    std::deque<std::vector<float>> m_frames;
    int m_target;
    int m_max;
    bool m_buffering = true;
    bool m_draining = false;
    int m_underruns = 0;
    int m_dropped = 0;
};

// Moves a gain towards its target over one buffer, so volume changes don't click
inline float RampGain(float current, float target, float maxStep = 0.05f) {
    if (target > current)
        return std::min(target, current + maxStep);
    return std::max(target, current - maxStep);
}

} // namespace Voice
} // namespace ZeldaOnline

#endif
