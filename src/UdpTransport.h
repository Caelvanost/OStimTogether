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
        void MaintenanceLoop(
            std::stop_token stopToken);

        void SendHello();
        void SendHelloTo(
            const sockaddr_in& destination,
            bool useObservedSourcePort);

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
            SnapshotDestinations(
                const sockaddr_in* excluded = nullptr);

        void RelayGameplayPacket(
            std::string_view packet,
            const sockaddr_in& source);

        bool SendPacketTo(
            std::string_view packet,
            const sockaddr_in& destination,
            std::string_view operation);

        std::optional<sockaddr_in>
            ResolveRemotePeer(
                const Config::RemotePeer& peer) const;

        void RefreshSkyrimTogetherAutoConfig(
            bool force);

        std::vector<sockaddr_in>
            SnapshotConfiguredPeers() const;

        std::string GetSharedSecretSnapshot() const;
        std::string SignPacket(
            std::string packet) const;
        bool AuthenticatePacket(
            std::string_view packet) const;
        std::string MarkRelayed(
            std::string_view packet) const;

        std::string GetLocalClientName() const;
        static std::string SanitizeField(
            std::string value);

        static std::optional<std::string>
            ReadField(
                std::string_view packet,
                std::string_view key);

        static std::string RemoveAuthField(
            std::string_view packet);

        static std::string AddressToString(
            const sockaddr_in& address);

        Config _config{};
        SOCKET _socket{ INVALID_SOCKET };

        sockaddr_in _broadcast{};
        std::vector<sockaddr_in> _configuredPeers;

        std::jthread _receiver;
        std::jthread _maintenance;

        std::atomic_bool _running{ false };
        std::mutex _sendMutex;

        mutable std::mutex _peerMutex;
        std::unordered_map<std::string, Peer> _peers;

        mutable std::mutex _configuredPeerMutex;
        mutable std::mutex _authMutex;
        std::string _sharedSecret;
        std::chrono::steady_clock::time_point
            _lastStrAutoConfigRefresh{};

        std::string _instanceID;
    };
}
