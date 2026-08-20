#pragma once

#include "PCH.h"

namespace OStimTogether::SKEEOverlayRefresh
{
    // Queue a normal RaceMenu/SKEE overlay rebuild for a dynamic actor after
    // remote override properties have been stored. This uses only SKEE's
    // public interface-exchange ABI and never removes existing overlays.
    void Queue(RE::Actor* actor, std::string_view reason);
}
