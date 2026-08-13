#pragma once

#include "PCH.h"
#include "Config.h"

namespace OStimTogether
{
    class UdpTransport
    {
    public:
        static UdpTransport& GetSingleton();

        bool Start();
        void Stop();
        void Send(std::string_view payload);

        [[nodiscard]] bool IsRunning() const noexcept
        {
            return _running.load();
        }

    private:
        struct Peer
        {
            sockaddr_in address{};
            std::string name;
            std::string instanceID;
            std::chrono::steady_clock::time_point lastSeen{};
        };

        UdpTransport() = default;
        ~UdpTransport();

        UdpTransport(
            const UdpTransport&) = delete;

        UdpTransport& operator=(
            const UdpTransport&) = delete;

        void ReceiverLoop();
        void DiscoveryLoop(
            std::stop_token stopToken);

        void SendHello();
        void SendHelloTo(
            const sockaddr_in& destination);

        bool HandleDiscoveryPacket(
            std::string_view packet,
            const sockaddr_in& source);

        void RegisterPeer(
            const sockaddr_in& source,
            std::uint16_t advertisedPort,
            std::string_view name,
            std::string_view instanceID);

        void TouchPeerFromGameplayPacket(
            const sockaddr_in& source,
            std::string_view packet);

        void ExpirePeers();

        std::vector<sockaddr_in>
            SnapshotPeers();

        std::string GetLocalClientName() const;
        static std::string SanitizeField(
            std::string value);

        static std::optional<std::string>
            ReadField(
                std::string_view packet,
                std::string_view key);

        static std::string AddressToString(
            const sockaddr_in& address);

        Config _config{};
        SOCKET _socket{ INVALID_SOCKET };

        sockaddr_in _broadcast{};
        sockaddr_in _manualPeer{};
        bool _hasManualPeer{ false };

        std::jthread _receiver;
        std::jthread _discovery;

        std::atomic_bool _running{ false };
        std::mutex _sendMutex;

        mutable std::mutex _peerMutex;
        std::unordered_map<std::string, Peer> _peers;

        std::string _instanceID;
    };
}
