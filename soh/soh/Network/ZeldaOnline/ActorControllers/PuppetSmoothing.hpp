#ifndef ZELDAONLINE_PUPPETSMOOTHING_HPP
#define ZELDAONLINE_PUPPETSMOOTHING_HPP

// Smooths the position and facing of another player's Link (and their horse) between network updates.
//
// Updates arrive about once per game frame, but not evenly: two can land in one frame and none in the next. Writing
// each one straight onto the puppet makes it hop. Instead the puppet eases toward the newest position, and when an
// update is late it keeps going in the direction it was moving for a few frames, then stops. A big jump (a warp, a
// void out, a respawn) is applied at once.
//
// Header only and free of game types beyond plain floats and s16, so it can be unit tested without the game.

#include <cmath>
#include <cstdint>

namespace ZeldaOnline {

struct PuppetSmoother {
    // Fraction of the remaining distance covered per game frame. 0.5 halves the gap each frame: under two frames of
    // added delay, with no visible hop.
    static constexpr float kFollow = 0.5f;
    // Gaps larger than this (in world units) are teleports, not movement. Link runs about 10 units a frame and Epona
    // about 25.
    static constexpr float kSnapDistance = 250.0f;
    // How many frames to keep moving along the last known direction when updates stop.
    static constexpr int kMaxExtrapolateFrames = 3;

    float target[3] = { 0, 0, 0 };   // newest position from the network
    float velocity[3] = { 0, 0, 0 }; // per frame, from the last two updates
    float shown[3] = { 0, 0, 0 };    // what the puppet is drawn at
    int16_t targetYaw = 0;
    int16_t shownYaw = 0;
    int framesSinceUpdate = 0;
    bool hasTarget = false;
    bool hasYaw = false;
    bool gotPosThisFrame = false;

    void Reset() {
        hasTarget = false;
        hasYaw = false;
        framesSinceUpdate = 0;
        gotPosThisFrame = false;
        for (int i = 0; i < 3; i++) {
            velocity[i] = 0;
        }
    }

    // One axis of a new network position. Call SetAxis for x, y, z (in any order); the sample is committed by Step.
    void SetAxis(int axis, float value) {
        incoming[axis] = value;
        gotPosThisFrame = true;
    }

    void SetYaw(int16_t yaw) {
        targetYaw = yaw;
        if (!hasYaw) {
            shownYaw = yaw;
            hasYaw = true;
        }
    }

    // Call once per game frame after this frame's updates were applied. Returns true when the shown position snapped
    // (callers may want to reset things that follow the puppet, like a held item).
    bool Step() {
        bool snapped = false;
        if (gotPosThisFrame) {
            gotPosThisFrame = false;
            if (!hasTarget) {
                for (int i = 0; i < 3; i++) {
                    target[i] = shown[i] = incoming[i];
                    velocity[i] = 0;
                }
                hasTarget = true;
                framesSinceUpdate = 0;
                snapped = true;
            } else {
                int frames = framesSinceUpdate + 1;
                for (int i = 0; i < 3; i++) {
                    velocity[i] = (incoming[i] - target[i]) / (float)frames;
                    target[i] = incoming[i];
                }
                framesSinceUpdate = 0;
            }
        } else if (hasTarget) {
            framesSinceUpdate++;
        }

        if (hasTarget) {
            float goal[3];
            int ahead = framesSinceUpdate < kMaxExtrapolateFrames ? framesSinceUpdate : kMaxExtrapolateFrames;
            for (int i = 0; i < 3; i++) {
                goal[i] = target[i] + velocity[i] * (float)ahead;
            }
            float dx = goal[0] - shown[0], dy = goal[1] - shown[1], dz = goal[2] - shown[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > kSnapDistance || velocityTooFast()) {
                for (int i = 0; i < 3; i++) {
                    shown[i] = target[i];
                    velocity[i] = 0;
                }
                snapped = true;
            } else {
                shown[0] += dx * kFollow;
                shown[1] += dy * kFollow;
                shown[2] += dz * kFollow;
            }
        }

        if (hasYaw) {
            int16_t diff = (int16_t)(targetYaw - shownYaw);
            // Turn half the way each frame; a turn of almost all the way round (a backflip) is shown at once.
            if (diff > 0x7000 || diff < -0x7000 || snapped) {
                shownYaw = targetYaw;
            } else {
                shownYaw = (int16_t)(shownYaw + diff / 2);
            }
        }
        return snapped;
    }

  private:
    float incoming[3] = { 0, 0, 0 };

    bool velocityTooFast() const {
        float v = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
        return v > kSnapDistance / 2;
    }
};

} // namespace ZeldaOnline

#endif
