#include "chordtracker.h"

#include "../vrr/assertions.h"
#include <cstdio>
#include <vector>

namespace {

constexpr unsigned kCreate = 4;
constexpr unsigned kPs = 5;
constexpr unsigned kTriangle = 3;
constexpr unsigned kA = 0;
constexpr unsigned kStart = 6;
constexpr unsigned kLb = 9;
constexpr unsigned kRb = 10;

struct HostEvent {
    enum Kind { ForwardPress, ForwardRelease, FireQuick, FireGyro, FireGyroStick, Suppress, ReleaseForwarded } kind;
    unsigned button = 0;
    uint32_t bits = 0;
};

struct Pad {
    ChordPadState state;
    std::vector<HostEvent> log;
};

ChordMasks overlayMasks()
{
    ChordMasks masks;
    masks.quickMenu = (1u << kCreate) | (1u << kPs);
    masks.gyroToggle = (1u << 7) | (1u << 8);
    masks.gyroStick = (1u << kCreate) | (1u << kTriangle);
    return masks;
}

void feed(Pad& pad, unsigned button, bool pressed, const ChordMasks& masks)
{
    const ChordResult r = chordOnButton(pad.state, button, pressed, masks);
    switch (r.action) {
    case ChordAction::Forward:
        pad.log.push_back({pressed ? HostEvent::ForwardPress : HostEvent::ForwardRelease, button, 0});
        break;
    case ChordAction::Suppress:
        pad.log.push_back({HostEvent::Suppress, button, 0});
        break;
    case ChordAction::FireQuickMenu:
        if (r.releaseBits) {
            pad.log.push_back({HostEvent::ReleaseForwarded, 0, r.releaseBits});
        }
        pad.log.push_back({HostEvent::FireQuick, button, r.releaseBits});
        break;
    case ChordAction::FireGyroToggle:
        if (r.releaseBits) {
            pad.log.push_back({HostEvent::ReleaseForwarded, 0, r.releaseBits});
        }
        pad.log.push_back({HostEvent::FireGyro, button, r.releaseBits});
        break;
    case ChordAction::FireGyroStick:
        if (r.releaseBits) {
            pad.log.push_back({HostEvent::ReleaseForwarded, 0, r.releaseBits});
        }
        pad.log.push_back({HostEvent::FireGyroStick, button, r.releaseBits});
        break;
    }
}

bool hasKind(const Pad& pad, HostEvent::Kind kind)
{
    for (const auto& e : pad.log) {
        if (e.kind == kind) {
            return true;
        }
    }
    return false;
}

int countKind(const Pad& pad, HostEvent::Kind kind)
{
    int n = 0;
    for (const auto& e : pad.log) {
        if (e.kind == kind) {
            ++n;
        }
    }
    return n;
}

void requireSeq(const Pad& pad, const std::vector<HostEvent::Kind>& kinds)
{
    assert(pad.log.size() == kinds.size());
    for (size_t i = 0; i < kinds.size(); ++i) {
        assert(pad.log[i].kind == kinds[i]);
    }
}

void incompleteForwardsImmediately()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    feed(pad, kCreate, false, masks);
    requireSeq(pad, {HostEvent::ForwardPress, HostEvent::ForwardRelease});
    assert(!hasKind(pad, HostEvent::FireQuick));
    assert(pad.state.down == 0);
    assert(pad.state.forwarded == 0);
}

void completeReleasesPreviouslySentAndSuppresses()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    assert(pad.log.back().kind == HostEvent::ForwardPress);
    feed(pad, kPs, true, masks);
    assert(countKind(pad, HostEvent::FireQuick) == 1);
    assert(countKind(pad, HostEvent::ReleaseForwarded) == 1);
    assert(pad.log[1].kind == HostEvent::ReleaseForwarded);
    assert(pad.log[1].bits == (1u << kCreate));
    assert((pad.state.suppressed & ((1u << kCreate) | (1u << kPs))) == ((1u << kCreate) | (1u << kPs)));

    const size_t before = pad.log.size();
    feed(pad, kA, true, masks);
    assert(pad.log.size() == before + 1);
    assert(pad.log[before].kind == HostEvent::ForwardPress);

    feed(pad, kPs, false, masks);
    feed(pad, kCreate, false, masks);
    assert(pad.log[before + 1].kind == HostEvent::Suppress);
    assert(pad.log[before + 2].kind == HostEvent::Suppress);
    assert(pad.state.suppressed == 0);
    assert(pad.state.latched == 0);
}

void repeatFiresAfterFullRelease()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    feed(pad, kPs, true, masks);
    feed(pad, kPs, false, masks);
    feed(pad, kCreate, false, masks);
    pad.log.clear();
    feed(pad, kCreate, true, masks);
    feed(pad, kPs, true, masks);
    assert(countKind(pad, HostEvent::FireQuick) == 1);
}

void twoPadsCannotCompleteOneShortcut()
{
    Pad a, b;
    const ChordMasks masks = overlayMasks();
    feed(a, kCreate, true, masks);
    feed(b, kPs, true, masks);
    assert(!hasKind(a, HostEvent::FireQuick));
    assert(!hasKind(b, HostEvent::FireQuick));
    assert(a.log.back().kind == HostEvent::ForwardPress);
    assert(b.log.back().kind == HostEvent::ForwardPress);

    feed(a, kPs, true, masks);
    assert(countKind(a, HostEvent::FireQuick) == 1);
    assert(!hasKind(b, HostEvent::FireQuick));
}

void disconnectReleasesForwardedState()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    const uint32_t released = chordTakeForwarded(pad.state);
    assert(released == (1u << kCreate));
    assert(pad.state.down == 0);
    assert(pad.state.forwarded == 0);
    assert(pad.state.suppressed == 0);
}

void focusLossDropsChordWithoutStuckButtons()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    feed(pad, kA, true, masks);
    const uint32_t released = chordTakeForwarded(pad.state);
    assert(released == ((1u << kCreate) | (1u << kA)));
    chordReset(pad.state);
    pad.log.clear();
    feed(pad, kCreate, false, masks);
    feed(pad, kA, false, masks);
    requireSeq(pad, {HostEvent::ForwardRelease, HostEvent::ForwardRelease});
}

void gyroHoldDoesNotFireToggle()
{
    Pad pad;
    ChordMasks masks = overlayMasks();
    masks.kbmMode = true;
    masks.gyroHoldMode = true;
    feed(pad, 7, true, masks);
    feed(pad, 8, true, masks);
    assert(!hasKind(pad, HostEvent::FireGyro));
    assert(countKind(pad, HostEvent::ForwardPress) == 2);
}

void gyroToggleFiresOnceInKbm()
{
    Pad pad;
    ChordMasks masks = overlayMasks();
    masks.kbmMode = true;
    masks.gyroHoldMode = false;
    feed(pad, 7, true, masks);
    feed(pad, 8, true, masks);
    assert(countKind(pad, HostEvent::FireGyro) == 1);
    feed(pad, 8, false, masks);
    feed(pad, 7, false, masks);
    feed(pad, 7, true, masks);
    feed(pad, 8, true, masks);
    assert(countKind(pad, HostEvent::FireGyro) == 2);
}

void gyroStickOverlapsQuickMenuWithoutStealingFirstButton()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    assert(pad.log.back().kind == HostEvent::ForwardPress);
    feed(pad, kTriangle, true, masks);
    assert(countKind(pad, HostEvent::FireGyroStick) == 1);
    assert(!hasKind(pad, HostEvent::FireQuick));
    assert(pad.log[1].kind == HostEvent::ReleaseForwarded);
    assert(pad.log[1].bits == (1u << kCreate));
}

void quitChordButtonsStayOrdinaryAgainstOverlayMask()
{
    Pad pad;
    const ChordMasks masks = overlayMasks();
    feed(pad, kCreate, true, masks);
    feed(pad, kStart, true, masks);
    feed(pad, kLb, true, masks);
    feed(pad, kRb, true, masks);
    assert(!hasKind(pad, HostEvent::FireQuick));
    assert(countKind(pad, HostEvent::ForwardPress) == 4);
}

void paddleChordForwardsIncompleteThenFires()
{
    Pad pad;
    ChordMasks masks;
    masks.quickMenu = (1u << 16) | (1u << 17);
    feed(pad, 16, true, masks);
    assert(pad.log.back().kind == HostEvent::ForwardPress);
    feed(pad, 17, true, masks);
    assert(countKind(pad, HostEvent::FireQuick) == 1);
    assert(pad.log[1].bits == (1u << 16));
}

} // namespace

int main()
{
    incompleteForwardsImmediately();
    completeReleasesPreviouslySentAndSuppresses();
    repeatFiresAfterFullRelease();
    twoPadsCannotCompleteOneShortcut();
    disconnectReleasesForwardedState();
    focusLossDropsChordWithoutStuckButtons();
    gyroHoldDoesNotFireToggle();
    gyroToggleFiresOnceInKbm();
    gyroStickOverlapsQuickMenuWithoutStealingFirstButton();
    quitChordButtonsStayOrdinaryAgainstOverlayMask();
    paddleChordForwardsIncompleteThenFires();
    std::puts("tst_chordtracker: all event sequences passed");
    return 0;
}
