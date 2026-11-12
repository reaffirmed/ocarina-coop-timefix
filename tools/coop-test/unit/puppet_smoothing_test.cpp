// Offline checks of the partner movement smoothing (soh/soh/Network/ZeldaOnline/ActorControllers/PuppetSmoothing.hpp).
// No game, no network.
//
// Build, from the repository root:
//   cl /nologo /std:c++20 /EHsc tools/coop-test/unit/puppet_smoothing_test.cpp /Fe:puppet_smoothing_test.exe
//   g++ -std=c++20 tools/coop-test/unit/puppet_smoothing_test.cpp -o puppet_smoothing_test
// Exit code 0 when every check passes.

#include <cmath>
#include <cstdio>
#include "../../../soh/soh/Network/ZeldaOnline/ActorControllers/PuppetSmoothing.hpp"

using namespace ZeldaOnline;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        sChecks++;                                                                                                     \
        if (!(cond)) {                                                                                                 \
            sFailures++;                                                                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
        }                                                                                                              \
    } while (0)

static void Send(PuppetSmoother& s, float x, float y, float z) {
    s.SetAxis(0, x);
    s.SetAxis(1, y);
    s.SetAxis(2, z);
}

static bool Near(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    // the first position is taken as it is
    {
        PuppetSmoother s;
        Send(s, 100, 0, -50);
        CHECK(s.Step());
        CHECK(s.hasTarget);
        CHECK(Near(s.shown[0], 100) && Near(s.shown[2], -50));
    }

    // steady running (10 units a frame) is followed closely, and never overshoots the newest position by much
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        float lag = 0;
        for (int f = 1; f <= 60; f++) {
            Send(s, f * 10.0f, 0, 0);
            CHECK(!s.Step());
            lag = s.target[0] - s.shown[0];
        }
        CHECK(lag > 0.0f && lag < 15.0f);
    }

    // jitter: two updates in one frame, none in the next. The shown path keeps moving forward without a stall
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        float prev = s.shown[0];
        float x = 0;
        for (int f = 1; f <= 40; f++) {
            if (f % 2 == 1) {
                x += 20.0f; // two updates' worth arrives at once (the last one wins)
                Send(s, x, 0, 0);
            }
            s.Step();
            CHECK(s.shown[0] >= prev - 0.001f); // never moves backwards
            prev = s.shown[0];
        }
    }

    // updates stop: it keeps moving in the same direction for a few frames, then settles on the last position
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        for (int f = 1; f <= 10; f++) {
            Send(s, f * 10.0f, 0, 0);
            s.Step();
        }
        float atStop = s.shown[0];
        s.Step();
        CHECK(s.shown[0] > atStop); // still moving the next frame
        float limit = 100.0f + 10.0f * PuppetSmoother::kMaxExtrapolateFrames;
        for (int f = 0; f < 30; f++) {
            s.Step();
            CHECK(s.shown[0] <= limit + 0.01f);
        }
        CHECK(s.shown[0] == 100.0f); // then exactly where it stopped
    }

    // a warp is applied at once
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        Send(s, 5000, 200, 3000);
        CHECK(s.Step());
        CHECK(Near(s.shown[0], 5000) && Near(s.shown[1], 200) && Near(s.shown[2], 3000));
    }

    // a short teleport (moved by a script or a warp point next door) is applied at once, not slid
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        Send(s, 150, 0, 0);
        CHECK(s.Step());
        CHECK(Near(s.shown[0], 150));
    }

    // a short teleport after standing still for a while (no updates in between) is applied at once too
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        for (int f = 0; f < 200; f++)
            s.Step();
        Send(s, 180, 0, 0);
        CHECK(s.Step());
        CHECK(Near(s.shown[0], 180));
    }

    // Epona at full gallop is still smoothed
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        bool snapped = false;
        for (int f = 1; f <= 30; f++) {
            Send(s, f * 28.0f, 0, 0);
            snapped |= s.Step();
        }
        CHECK(!snapped);
    }

    // standing still stays exactly still
    {
        PuppetSmoother s;
        for (int f = 0; f < 20; f++) {
            Send(s, 12.5f, 30, -7);
            s.Step();
        }
        CHECK(Near(s.shown[0], 12.5f, 0.001f) && Near(s.shown[1], 30, 0.001f) && Near(s.shown[2], -7, 0.001f));
    }

    // facing: turns half way each frame, and the short way round across the wrap
    {
        PuppetSmoother s;
        s.SetYaw(0x7000);
        s.Step();
        CHECK(s.shownYaw == 0x7000);
        s.SetYaw((int16_t)0x9000); // 0x2000 further, across 0x8000
        s.Step();
        CHECK(s.shownYaw == (int16_t)0x8000);
        s.Step();
        CHECK(s.shownYaw == (int16_t)0x8800);
        // a backflip turn is shown at once
        s.SetYaw(0x1000);
        s.Step();
        CHECK(s.shownYaw == 0x1000);
    }

    // after stopping, the puppet ends up exactly on the last position and facing (not a hair short)
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.SetYaw(0);
        s.Step();
        Send(s, 40, 0, 0);
        s.SetYaw(0x1234);
        for (int f = 0; f < 30; f++)
            s.Step();
        CHECK(s.shown[0] == 40.0f && s.shown[1] == 0.0f && s.shown[2] == 0.0f);
        CHECK(s.shownYaw == 0x1234);
    }

    // Reset forgets everything: the next position is taken as it is
    {
        PuppetSmoother s;
        Send(s, 0, 0, 0);
        s.Step();
        s.Reset();
        Send(s, 100, 0, 0);
        CHECK(s.Step());
        CHECK(Near(s.shown[0], 100));
    }

    // only the changed axes arrive: the others keep the spawn position, not 0
    {
        PuppetSmoother s;
        s.SeedPosition(160, 0, 1415);
        s.SetAxis(0, 225);
        CHECK(s.Step());
        CHECK(Near(s.shown[0], 225) && Near(s.shown[1], 0) && Near(s.shown[2], 1415));
        // a later update of one axis keeps the others
        s.SetAxis(1, 20);
        s.Step();
        CHECK(Near(s.target[0], 225) && Near(s.target[1], 20) && Near(s.target[2], 1415));
    }

    std::printf("%d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
