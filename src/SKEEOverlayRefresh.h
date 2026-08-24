#pragma once

#include "PCH.h"

namespace OStimTogether::SKEEOverlayRefresh
{
    // Queue a coalesced RaceMenu/SKEE overlay + node-override update for an
    // actor. Uses RaceMenu's public ActorUpdateManager pipeline, so it works for
    // both the real PlayerCharacter and dynamic STR proxies. Requests are
    // debounced per actor to avoid the 0.31.6 overlay rebuild storm.
    void Queue(RE::Actor* actor, std::string_view reason);
}
