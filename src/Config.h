#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OStimTogether
{
    struct Config
    {
        struct RemotePeer
        {
            std::string host;
            std::uint16_t port{ 27991 };
        };

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
        bool relayMode{ false };

        // Shared UDP port. Different PCs can bind the same port.
        std::uint16_t localPort{ 27991 };

        // Internet/direct-connect support. Remote clients can automatically
        // reuse Skyrim Together Reborn's saved direct-connect host and contact
        // that same host on OStim Together's UDP port.
        bool autoRemoteFromSTR{ true };
        std::uint16_t autoRemotePort{ 27991 };
        bool autoSharedSecretFromSTR{ false };

        // Manual peers. PeerHost/PeerPort are legacy aliases folded into
        // remotePeers after loading.
        std::string peerHost{};
        std::uint16_t peerPort{ 27991 };
        std::vector<RemotePeer> remotePeers;
        std::string sharedSecret;

        std::uint32_t discoveryIntervalMs{ 1000 };
        std::uint32_t peerTimeoutMs{ 10000 };

        static Config Load();
    };
}
