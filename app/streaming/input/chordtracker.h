#pragma once

#include <cstdint>

// Per-controller fire-once shortcut contract (plan C):
// incomplete chords forward immediately; a completed chord fires once, names
// already-forwarded bits to release, then suppresses those chord buttons until
// they are all released. Two pads never share this state.
struct ChordPadState {
    uint32_t down = 0;
    uint32_t forwarded = 0;
    uint32_t suppressed = 0;
    uint8_t latched = 0;
};

struct ChordMasks {
    uint32_t quickMenu = 0;
    uint32_t gyroToggle = 0;
    uint32_t gyroStick = 0;
    uint32_t xboxToggle = 0;
    bool kbmMode = false;
    bool gyroHoldMode = false;
};

enum class ChordAction : uint8_t {
    Forward,
    Suppress,
    FireQuickMenu,
    FireGyroToggle,
    FireGyroStick,
    FireXboxToggle,
};

struct ChordResult {
    ChordAction action = ChordAction::Forward;
    uint32_t releaseBits = 0;
    uint32_t down = 0;
};

inline void chordReset(ChordPadState& pad)
{
    pad = {};
}

inline uint32_t chordTakeForwarded(ChordPadState& pad)
{
    const uint32_t bits = pad.forwarded;
    chordReset(pad);
    return bits;
}

inline ChordResult chordOnButton(ChordPadState& pad, unsigned button, bool pressed, const ChordMasks& masks)
{
    ChordResult result;
    if (button >= 32) {
        result.down = pad.down;
        return result;
    }

    const uint32_t bit = 1u << button;
    if (pressed) {
        pad.down |= bit;
    }
    else {
        pad.down &= ~bit;
    }

    if (pad.suppressed & bit) {
        if (!pressed) {
            pad.suppressed &= ~bit;
            pad.forwarded &= ~bit;
        }
        if (pad.suppressed == 0) {
            pad.latched = 0;
        }
        result.action = ChordAction::Suppress;
        result.down = pad.down;
        return result;
    }

    auto tryFire = [&](uint32_t mask, uint8_t latchBit, ChordAction fire) {
        if (mask == 0) {
            return false;
        }
        if (pressed && (pad.down & mask) == mask && (pad.latched & latchBit) == 0) {
            pad.latched |= latchBit;
            result.releaseBits = pad.forwarded & mask;
            pad.forwarded &= ~mask;
            pad.suppressed = mask;
            result.action = fire;
            return true;
        }
        if ((pad.down & mask) == 0) {
            pad.latched &= static_cast<uint8_t>(~latchBit);
        }
        return false;
    };

    if (tryFire(masks.quickMenu, 1, ChordAction::FireQuickMenu)) {
        result.down = pad.down;
        return result;
    }
    if (masks.kbmMode && !masks.gyroHoldMode &&
            tryFire(masks.gyroToggle, 2, ChordAction::FireGyroToggle)) {
        result.down = pad.down;
        return result;
    }
    if (!masks.kbmMode && tryFire(masks.gyroStick, 4, ChordAction::FireGyroStick)) {
        result.down = pad.down;
        return result;
    }
    if (!masks.kbmMode && tryFire(masks.xboxToggle, 8, ChordAction::FireXboxToggle)) {
        result.down = pad.down;
        return result;
    }

    if (pressed) {
        pad.forwarded |= bit;
    }
    else {
        pad.forwarded &= ~bit;
    }
    result.action = ChordAction::Forward;
    result.down = pad.down;
    return result;
}
