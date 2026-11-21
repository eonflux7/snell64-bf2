#pragma once

// [CHOSEN, not recovered] Issue #60 (https://github.com/kr4uzi/OpenBF2/issues/60): when an
// anti-aliasing change needs no restart. Pure rules shared by Game/VideoSettings.cpp (the notice
// and the push) and RendDX9/VideoOptions.cpp (what the next level load rebuilds), testable without
// a renderer (Code/Tests/antialiasing_policy.cpp).

namespace dice { namespace hfe { namespace aapolicy {

// Whether Apply pushes the anti-aliasing level to the renderer. Retail also required that the
// settings repository names no level (GSLoadLevel), which after the first match of a session is
// never the case. The renderer's device reset, multisample probes and define rebuild
// (videoOptionsChangeResolution) leave nothing stale once the next level load rebuilds the
// effects (see defineInputsChanged), so only a loaded game blocks the push.
inline bool pushesAntialiasing(bool gameLoaded) { return !gameLoaded; }

// Whether an anti-aliasing change still needs a restart: when Apply will not push it (a game is
// loaded) or no renderer is there to apply it.
inline bool antialiasingNeedsRestart(bool gameLoaded, bool rendererPresent)
{
    return !(pushesAntialiasing(gameLoaded) && rendererPresent);
}

// Whether the effect compile defines (HASALPHA2MASK) differ between two device states, so effects
// compiled before the switch are stale and the next level load must reload them.
inline bool defineInputsChanged(bool alpha2MaskBefore, bool alpha2MaskAfter)
{
    return alpha2MaskBefore != alpha2MaskAfter;
}

}}} // namespace dice::hfe::aapolicy
