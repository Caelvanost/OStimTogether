#pragma once

#include <cstdint>
#include <string>

namespace OStimTogether
{
    struct Config
    {
        std::uint32_t toggleKey{ 0x44 };  // F10
        std::uint32_t clearKey{ 0x57 };   // F11
        std::uint32_t intervalMs{ 25 };

        std::uint32_t slotMask{
            (1u << 2) |  // 32 Body
            (1u << 3) |  // 33 Hands
            (1u << 7)    // 37 Feet
        };

        bool debugNotifications{ true };

        // v0.18:
        // Networking is automatic by default. Existing old INIs containing
        // Enabled=0 / ClientName / PeerHost no longer need to be edited.
        bool networkEnabled{ true };
        bool autoDiscovery{ true };

        // Shared UDP port. Different PCs can bind the same port.
        std::uint16_t localPort{ 27991 };

        // Advanced/manual fallback only.
        std::string peerHost{};
        std::uint16_t peerPort{ 27991 };

        std::uint32_t discoveryIntervalMs{ 1000 };
        std::uint32_t peerTimeoutMs{ 10000 };

        static Config Load();
    };
}
