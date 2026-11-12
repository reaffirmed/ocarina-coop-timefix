// Offline checks of proximity voice chat (soh/soh/Network/ZeldaOnline/VoiceCore.hpp): the packet format, volume by
// distance, left/right panning, the voice gate, the jitter buffer, and an Opus round trip of a test tone. No game,
// no network, no audio device.
//
// Build, from the repository root:
//   Linux/macOS: c++ -std=c++17 -O2 -Isoh/soh/Network/ZeldaOnline tools/coop-test/unit/voice_core_test.cpp
//                  -lopus -o voice_core_test
//   Windows (developer prompt, vcpkg opus): cl /nologo /std:c++17 /EHsc /Isoh/soh/Network/ZeldaOnline ^
//                  tools/coop-test/unit/voice_core_test.cpp opus.lib /Fe:voice_core_test.exe
// Exit code 0 when every check passes.

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <vector>

#include <opus/opus.h>

#include "VoiceCore.hpp"

using namespace ZeldaOnline::Voice;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        sChecks++;                                                                                                   \
        if (!(cond)) {                                                                                               \
            sFailures++;                                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                      \
        }                                                                                                            \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

static void TestPacket() {
    VoicePacket p;
    p.flags = VOICE_FLAG_END;
    p.seq = 0xBEEF;
    p.frames = { { 1, 2, 3 }, { 4 }, std::vector<uint8_t>(255, 7) };
    std::vector<uint8_t> body;
    EncodeVoiceBody(p, body);
    CHECK(body.size() == 4 + 4 + 2 + 256);
    VoicePacket q;
    CHECK(DecodeVoiceBody(body.data(), body.size(), q));
    CHECK(q.flags == VOICE_FLAG_END && q.seq == 0xBEEF && q.frames.size() == 3);
    CHECK(q.frames[0] == p.frames[0] && q.frames[2] == p.frames[2]);

    // truncated, trailing bytes, zero length frame, too many frames
    CHECK(!DecodeVoiceBody(body.data(), body.size() - 1, q));
    std::vector<uint8_t> extra = body;
    extra.push_back(0);
    CHECK(!DecodeVoiceBody(extra.data(), extra.size(), q));
    std::vector<uint8_t> zero = { 0, 0, 0, 1, 0 };
    CHECK(!DecodeVoiceBody(zero.data(), zero.size(), q));
    std::vector<uint8_t> many = { 0, 0, 0, (uint8_t)(MAX_FRAMES_PER_PACKET + 1) };
    CHECK(!DecodeVoiceBody(many.data(), many.size(), q));
    CHECK(!DecodeVoiceBody(body.data(), 3, q));
    // an empty packet is only valid as the end of a spurt
    std::vector<uint8_t> endOnly = { VOICE_FLAG_END, 5, 0, 0 };
    CHECK(DecodeVoiceBody(endOnly.data(), endOnly.size(), q) && q.frames.empty());
    std::vector<uint8_t> empty = { 0, 5, 0, 0 };
    CHECK(!DecodeVoiceBody(empty.data(), empty.size(), q));
}

static void TestDistance() {
    CHECK(Near(DistanceGain(0, 250, 1500), 1.0f));
    CHECK(Near(DistanceGain(250, 250, 1500), 1.0f));
    CHECK(Near(DistanceGain(1500, 250, 1500), 0.0f));
    CHECK(Near(DistanceGain(5000, 250, 1500), 0.0f));
    CHECK(Near(DistanceGain(875, 250, 1500), 0.25f)); // halfway: (1 - 0.5)^2
    // never gets louder further away
    float last = 2.0f;
    bool monotonic = true;
    for (float d = 0; d < 2000; d += 10) {
        float g = DistanceGain(d, 250, 1500);
        monotonic = monotonic && g <= last + 1e-6f;
        last = g;
    }
    CHECK(monotonic);
}

static void TestPan() {
    float l, r;
    // camera looking along +z: screen right is -x
    PanGains(-1000, 0, 0, 1, 250, l, r);
    CHECK(Near(r, 1.0f) && Near(l, 0.3f));
    PanGains(1000, 0, 0, 1, 250, l, r);
    CHECK(Near(l, 1.0f) && Near(r, 0.3f));
    // straight ahead or behind: centred
    PanGains(0, 1000, 0, 1, 250, l, r);
    CHECK(Near(l, 1.0f) && Near(r, 1.0f));
    PanGains(0, -1000, 0, 1, 250, l, r);
    CHECK(Near(l, 1.0f) && Near(r, 1.0f));
    // turning the camera around swaps the sides
    PanGains(1000, 0, 0, -1, 250, l, r);
    CHECK(Near(r, 1.0f) && Near(l, 0.3f));
    // right next to you: barely panned
    PanGains(-50, 0, 0, 1, 250, l, r);
    CHECK(l > 0.8f && Near(r, 1.0f));
    // degenerate camera or same spot
    PanGains(0, 0, 0, 1, 250, l, r);
    CHECK(Near(l, 1.0f) && Near(r, 1.0f));
    PanGains(500, 0, 0, 0, 250, l, r);
    CHECK(Near(l, 1.0f) && Near(r, 1.0f));
}

static void TestGate() {
    VoiceGate g;
    CHECK(!g.Process(-60, -45));
    CHECK(g.Process(-30, -45));
    // stays open through a short pause, then closes
    int open = 0;
    for (int i = 0; i < 20; i++)
        open += g.Process(-60, -45, 15) ? 1 : 0;
    CHECK(open == 15);
    CHECK(!g.Process(-60, -45));
    CHECK(FrameDb(std::vector<float>(960, 0.0f).data(), 960) == -100.0f);
    CHECK(Near(FrameDb(std::vector<float>(960, 0.5f).data(), 960), -6.0206f, 0.01f));
}

static void TestJitter() {
    std::vector<float> in(FRAME_SAMPLES), out(FRAME_SAMPLES);
    JitterBuffer jb(5, 15);
    // waits for 5 frames before playing
    for (int i = 0; i < 4; i++) {
        in[0] = (float)i;
        jb.Push(in.data(), false);
        CHECK(!jb.Pop(out.data()));
    }
    in[0] = 4;
    jb.Push(in.data(), false);
    for (int i = 0; i < 5; i++) {
        CHECK(jb.Pop(out.data()));
        CHECK(out[0] == (float)i);
    }
    // ran dry mid spurt: one underrun, then buffers again
    CHECK(!jb.Pop(out.data()));
    CHECK(jb.Underruns() == 1);
    jb.Push(in.data(), false);
    CHECK(!jb.Pop(out.data()));

    // a short spurt that ends plays out at once, and ending isn't an underrun
    JitterBuffer jb2(5, 15);
    jb2.Push(in.data(), false);
    jb2.Push(in.data(), true);
    CHECK(jb2.Pop(out.data()));
    CHECK(jb2.Pop(out.data()));
    CHECK(!jb2.Pop(out.data()));
    CHECK(jb2.Underruns() == 0);

    // a backlog is cut back to the target, keeping the newest frames
    JitterBuffer jb3(5, 15);
    for (int i = 0; i < 16; i++) {
        in[0] = (float)i;
        jb3.Push(in.data(), false);
    }
    CHECK(jb3.Queued() == 5 && jb3.Dropped() == 1);
    CHECK(jb3.Pop(out.data()) && out[0] == 11.0f);
}

static void TestOpusRoundTrip() {
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder* dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    CHECK(enc != nullptr && dec != nullptr);
    if (enc == nullptr || dec == nullptr)
        return;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    double phase = 0;
    int maxBytes = 0;
    bool fits = true;
    float lastDb = -100;
    std::vector<float> pcm(FRAME_SAMPLES), out(FRAME_SAMPLES);
    for (int f = 0; f < 50; f++) {
        for (auto& v : pcm) {
            v = (float)(0.25 * std::sin(phase));
            phase += 2 * M_PI * 440.0 / SAMPLE_RATE;
        }
        unsigned char bytes[MAX_OPUS_FRAME_BYTES];
        int n = opus_encode_float(enc, pcm.data(), FRAME_SAMPLES, bytes, MAX_OPUS_FRAME_BYTES);
        fits = fits && n > 0 && n <= MAX_OPUS_FRAME_BYTES;
        maxBytes = std::max(maxBytes, n);
        int m = opus_decode_float(dec, bytes, n, out.data(), FRAME_SAMPLES, 0);
        CHECK(m == FRAME_SAMPLES);
        lastDb = FrameDb(out.data(), FRAME_SAMPLES);
    }
    CHECK(fits);
    // a 0.25 amplitude sine is about -15 dBFS; the decoded tone should be close once the codec settled
    CHECK(lastDb > -18.0f && lastDb < -12.0f);
    std::printf("opus: largest 20 ms frame %d bytes, decoded tone %.1f dBFS\n", maxBytes, lastDb);
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
}

int main() {
    TestPacket();
    TestDistance();
    TestPan();
    TestGate();
    TestJitter();
    TestOpusRoundTrip();
    std::printf("%d of %d checks passed\n", sChecks - sFailures, sChecks);
    return sFailures == 0 ? 0 : 1;
}
