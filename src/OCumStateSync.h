#pragma once

#include "PCH.h"

namespace OStimTogether
{
    class OCumStateSync
    {
    public:
        static OCumStateSync& GetSingleton();

        // Sends the real local player's current OCum RaceMenu overlays and
        // vaginal/anal equip-object state through the generic ADDON protocol.
        // Safe no-op when OCum.esp is not installed.
        void SendLocalSnapshot(std::string_view reason);

    private:
        OCumStateSync() = default;
        static std::string HexEncode(std::string_view value);
    };
}
