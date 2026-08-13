#include "PCH.h"
#include "UdpTransport.h"
#include "ActorResolver.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kDiscoveryPrefix =
            "OSTDISC|v1|HELLO|";

        constexpr std::string_view kGameplayPrefix =
            "OSTUDP|v1|";

        std::uint16_t ParsePort(
            const std::optional<std::string>& value)
        {
            if (!value ||
                value->empty()) {
                return 0;
            }

            try {
                const auto parsed =
                    std::stoul(*value);

                if (parsed == 0 ||
                    parsed > 65535) {
                    return 0;
                }

                return static_cast<std::uint16_t>(
                    parsed);
            } catch (...) {
                return 0;
            }
        }
    }

    UdpTransport&
        UdpTransport::GetSingleton()
    {
        static UdpTransport instance;
        return instance;
    }

    UdpTransport::~UdpTransport()
    {
        Stop();
    }

    std::string UdpTransport::SanitizeField(
        std::string value)
    {
        for (auto& c : value) {
            if (c == '|' ||
                c == '\r' ||
                c == '\n') {
                c = '_';
            }
        }

        if (value.empty()) {
            value = "Player";
        }

        return value;
    }

    std::string UdpTransport::GetLocalClientName()
        const
    {
        if (auto* player =
                RE::PlayerCharacter::GetSingleton()) {
            if (const auto* name =
                    player->GetName();
                name &&
                *name) {
                return SanitizeField(name);
            }
        }

        char computerName[
            MAX_COMPUTERNAME_LENGTH + 1]{};

        DWORD length =
            static_cast<DWORD>(
                std::size(computerName));

        if (GetComputerNameA(
                computerName,
                &length) &&
            length > 0) {
            return SanitizeField(
                std::string(
                    computerName,
                    length));
        }

        return "Player";
    }

    std::optional<std::string>
        UdpTransport::ReadField(
            std::string_view packet,
            std::string_view key)
    {
        const auto needle =
            fmt::format(
                "{}=",
                key);

        auto pos =
            packet.find(needle);

        if (pos ==
            std::string_view::npos) {
            return std::nullopt;
        }

        pos +=
            needle.size();

        auto end =
            packet.find(
                '|',
                pos);

        if (end ==
            std::string_view::npos) {
            end =
                packet.size();
        }

        return std::string(
            packet.substr(
                pos,
                end - pos));
    }

    std::string UdpTransport::AddressToString(
        const sockaddr_in& address)
    {
        char ip[
            INET_ADDRSTRLEN]{};

        InetNtopA(
            AF_INET,
            &address.sin_addr,
            ip,
            sizeof(ip));

        return fmt::format(
            "{}:{}",
            ip,
            ntohs(
                address.sin_port));
    }

    bool UdpTransport::Start()
    {
        if (_running.load()) {
            return true;
        }

        _config =
            Config::Load();

        if (!_config.networkEnabled) {
            SKSE::log::info(
                "UDP transport disabled by Network/Disabled=1");
            return false;
        }

        WSADATA wsa{};

        const auto wsaResult =
            WSAStartup(
                MAKEWORD(2, 2),
                &wsa);

        if (wsaResult != 0) {
            SKSE::log::error(
                "UDP WSAStartup failed: {}",
                wsaResult);
            return false;
        }

        _socket =
            socket(
                AF_INET,
                SOCK_DGRAM,
                IPPROTO_UDP);

        if (_socket ==
            INVALID_SOCKET) {
            SKSE::log::error(
                "UDP socket() failed: {}",
                WSAGetLastError());

            WSACleanup();
            return false;
        }

        BOOL broadcastEnabled =
            TRUE;

        if (setsockopt(
                _socket,
                SOL_SOCKET,
                SO_BROADCAST,
                reinterpret_cast<
                    const char*>(
                    &broadcastEnabled),
                sizeof(
                    broadcastEnabled)) ==
            SOCKET_ERROR) {
            SKSE::log::error(
                "UDP SO_BROADCAST failed: {}",
                WSAGetLastError());

            closesocket(_socket);
            _socket =
                INVALID_SOCKET;

            WSACleanup();
            return false;
        }

        BOOL reuseAddress =
            TRUE;

        setsockopt(
            _socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<
                const char*>(
                &reuseAddress),
            sizeof(
                reuseAddress));

        sockaddr_in local{};
        local.sin_family =
            AF_INET;

        local.sin_addr.s_addr =
            htonl(
                INADDR_ANY);

        local.sin_port =
            htons(
                _config.localPort);

        if (bind(
                _socket,
                reinterpret_cast<
                    sockaddr*>(
                    &local),
                sizeof(local)) ==
            SOCKET_ERROR) {
            SKSE::log::error(
                "UDP bind failed on port {}: {}",
                _config.localPort,
                WSAGetLastError());

            closesocket(_socket);
            _socket =
                INVALID_SOCKET;

            WSACleanup();
            return false;
        }

        DWORD timeoutMs =
            250;

        setsockopt(
            _socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<
                const char*>(
                &timeoutMs),
            sizeof(
                timeoutMs));

        _broadcast = {};
        _broadcast.sin_family =
            AF_INET;

        _broadcast.sin_port =
            htons(
                _config.localPort);

        _broadcast.sin_addr.s_addr =
            htonl(
                INADDR_BROADCAST);

        _hasManualPeer =
            false;

        if (!_config.autoDiscovery &&
            !_config.peerHost.empty()) {
            _manualPeer = {};
            _manualPeer.sin_family =
                AF_INET;

            _manualPeer.sin_port =
                htons(
                    _config.peerPort);

            if (InetPtonA(
                    AF_INET,
                    _config.peerHost.c_str(),
                    &_manualPeer.sin_addr) ==
                1) {
                _hasManualPeer =
                    true;
            } else {
                SKSE::log::error(
                    "Manual PeerHost is not a valid IPv4 address: {}",
                    _config.peerHost);

                closesocket(
                    _socket);

                _socket =
                    INVALID_SOCKET;

                WSACleanup();
                return false;
            }
        }

        _instanceID =
            fmt::format(
                "{:08X}-{:016X}",
                GetCurrentProcessId(),
                GetTickCount64());

        _running.store(
            true);

        _receiver =
            std::jthread(
                [this](
                    std::stop_token) {
                    ReceiverLoop();
                });

        if (_config.autoDiscovery) {
            _discovery =
                std::jthread(
                    [this](
                        std::stop_token token) {
                        DiscoveryLoop(
                            token);
                    });
        }

        SKSE::log::info(
            "UDP transport started AUTO={} client=\"{}\" port={} instance={}",
            _config.autoDiscovery ? 1 : 0,
            GetLocalClientName(),
            _config.localPort,
            _instanceID);

        if (_config.autoDiscovery) {
            // Do not wait for the first discovery-loop interval.
            SendHello();
        } else if (_hasManualPeer) {
            SKSE::log::info(
                "UDP manual peer={}",
                AddressToString(
                    _manualPeer));
        }

        return true;
    }

    void UdpTransport::Stop()
    {
        if (!_running.exchange(
                false)) {
            return;
        }

        if (_receiver.joinable()) {
            _receiver.request_stop();
        }

        if (_discovery.joinable()) {
            _discovery.request_stop();
        }

        if (_socket !=
            INVALID_SOCKET) {
            closesocket(
                _socket);

            _socket =
                INVALID_SOCKET;
        }

        if (_receiver.joinable()) {
            _receiver.join();
        }

        if (_discovery.joinable()) {
            _discovery.join();
        }

        {
            std::scoped_lock lock(
                _peerMutex);

            _peers.clear();
        }

        WSACleanup();

        SKSE::log::info(
            "UDP transport stopped");
    }

    void UdpTransport::SendHello()
    {
        if (!_running.load() ||
            _socket ==
                INVALID_SOCKET ||
            !_config.autoDiscovery) {
            return;
        }

        SendHelloTo(
            _broadcast);
    }

    void UdpTransport::SendHelloTo(
        const sockaddr_in& destination)
    {
        const auto packet =
            fmt::format(
                "OSTDISC|v1|HELLO|id={}|name={}|port={}",
                _instanceID,
                GetLocalClientName(),
                _config.localPort);

        std::scoped_lock lock(
            _sendMutex);

        const auto sent =
            sendto(
                _socket,
                packet.data(),
                static_cast<int>(
                    packet.size()),
                0,
                reinterpret_cast<
                    const sockaddr*>(
                    &destination),
                sizeof(
                    destination));

        if (sent ==
            SOCKET_ERROR &&
            _running.load()) {
            SKSE::log::warn(
                "OSTNET discovery TX failed to {}: {}",
                AddressToString(
                    destination),
                WSAGetLastError());
        }
    }

    void UdpTransport::RegisterPeer(
        const sockaddr_in& source,
        std::uint16_t advertisedPort,
        std::string_view name,
        std::string_view instanceID)
    {
        if (instanceID.empty() ||
            instanceID ==
                _instanceID) {
            return;
        }

        sockaddr_in peer =
            source;

        peer.sin_port =
            htons(
                advertisedPort);

        const auto key =
            fmt::format(
                "{}|{}",
                instanceID,
                AddressToString(
                    peer));

        bool isNew =
            false;

        {
            std::scoped_lock lock(
                _peerMutex);

            auto [it, inserted] =
                _peers.try_emplace(
                    key);

            isNew =
                inserted;

            it->second.address =
                peer;

            it->second.name =
                std::string(
                    name);

            it->second.instanceID =
                std::string(
                    instanceID);

            it->second.lastSeen =
                std::chrono::
                    steady_clock::now();
        }

        if (isNew) {
            SKSE::log::info(
                "OSTNET DISCOVERED peer=\"{}\" addr={} instance={}",
                name,
                AddressToString(
                    peer),
                instanceID);

            // Immediate unicast hello speeds up symmetric discovery.
            SendHelloTo(
                peer);
        }
    }

    bool UdpTransport::HandleDiscoveryPacket(
        std::string_view packet,
        const sockaddr_in& source)
    {
        if (!packet.starts_with(
                kDiscoveryPrefix)) {
            return false;
        }

        const auto id =
            ReadField(
                packet,
                "id");

        const auto name =
            ReadField(
                packet,
                "name");

        const auto port =
            ParsePort(
                ReadField(
                    packet,
                    "port"));

        if (!id ||
            !name ||
            port == 0) {
            SKSE::log::warn(
                "OSTNET malformed discovery packet from {}",
                AddressToString(
                    source));

            return true;
        }

        RegisterPeer(
            source,
            port,
            *name,
            *id);

        return true;
    }

    void UdpTransport::
        TouchPeerFromGameplayPacket(
            const sockaddr_in& source,
            std::string_view packet)
    {
        if (!packet.starts_with(
                kGameplayPrefix)) {
            return;
        }

        // A gameplay packet can arrive before HELLO. Use it as a temporary
        // peer record so the reply path is available immediately.
        const auto sender =
            ReadField(
                packet,
                "from");

        const auto key =
            fmt::format(
                "gameplay|{}",
                AddressToString(
                    source));

        std::scoped_lock lock(
            _peerMutex);

        auto& peer =
            _peers[key];

        peer.address =
            source;

        peer.name =
            sender.value_or(
                "Peer");

        peer.instanceID =
            key;

        peer.lastSeen =
            std::chrono::
                steady_clock::now();
    }

    void UdpTransport::ExpirePeers()
    {
        const auto now =
            std::chrono::
                steady_clock::now();

        const auto timeout =
            std::chrono::
                milliseconds(
                    _config.peerTimeoutMs);

        std::vector<std::string>
            expired;

        {
            std::scoped_lock lock(
                _peerMutex);

            for (auto it =
                     _peers.begin();
                 it !=
                     _peers.end();) {
                if (now -
                        it->second.lastSeen >
                    timeout) {
                    expired.push_back(
                        fmt::format(
                            "\"{}\" {}",
                            it->second.name,
                            AddressToString(
                                it->second.address)));

                    it =
                        _peers.erase(
                            it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& peer :
             expired) {
            SKSE::log::info(
                "OSTNET PEER EXPIRED {}",
                peer);
        }
    }

    std::vector<sockaddr_in>
        UdpTransport::SnapshotPeers()
    {
        std::vector<sockaddr_in>
            result;

        {
            std::scoped_lock lock(
                _peerMutex);

            result.reserve(
                _peers.size());

            // Deduplicate by IPv4:port because a gameplay temporary record
            // and a HELLO record can briefly refer to the same peer.
            std::unordered_set<
                std::string>
                seen;

            for (const auto& [key, peer] :
                 _peers) {
                const auto endpoint =
                    AddressToString(
                        peer.address);

                if (seen.insert(
                        endpoint).second) {
                    result.push_back(
                        peer.address);
                }
            }
        }

        return result;
    }

    void UdpTransport::Send(
        std::string_view payload)
    {
        if (!_running.load() ||
            _socket ==
                INVALID_SOCKET ||
            payload.empty()) {
            return;
        }

        const auto packet =
            fmt::format(
                "OSTUDP|v1|from={}|{}",
                GetLocalClientName(),
                payload);

        std::vector<sockaddr_in>
            destinations;

        if (_config.autoDiscovery) {
            destinations =
                SnapshotPeers();

            // If a scene starts before discovery has completed, send the
            // packet once by LAN broadcast. A receiving v0.18 client will
            // process it and learn our source endpoint immediately.
            if (destinations.empty()) {
                destinations.push_back(
                    _broadcast);
            }
        } else if (_hasManualPeer) {
            destinations.push_back(
                _manualPeer);
        }

        if (destinations.empty()) {
            SKSE::log::warn(
                "OSTNET TX dropped: no peer available");
            return;
        }

        std::scoped_lock lock(
            _sendMutex);

        std::size_t sentCount =
            0;

        for (const auto& destination :
             destinations) {
            const auto sent =
                sendto(
                    _socket,
                    packet.data(),
                    static_cast<int>(
                        packet.size()),
                    0,
                    reinterpret_cast<
                        const sockaddr*>(
                        &destination),
                    sizeof(
                        destination));

            if (sent ==
                SOCKET_ERROR) {
                SKSE::log::error(
                    "OSTNET TX failed to {}: {}",
                    AddressToString(
                        destination),
                    WSAGetLastError());
                continue;
            }

            ++sentCount;
        }

        SKSE::log::info(
            "OSTNET TX peers={} {}",
            sentCount,
            packet);
    }

    void UdpTransport::DiscoveryLoop(
        std::stop_token stopToken)
    {
        while (
            !stopToken.stop_requested() &&
            _running.load()) {
            SendHello();
            ExpirePeers();

            const auto total =
                std::chrono::
                    milliseconds(
                        _config.discoveryIntervalMs);

            constexpr auto slice =
                std::chrono::
                    milliseconds(
                        100);

            auto slept =
                std::chrono::
                    milliseconds(
                        0);

            while (
                slept < total &&
                !stopToken.stop_requested() &&
                _running.load()) {
                std::this_thread::
                    sleep_for(
                        slice);

                slept +=
                    slice;
            }
        }
    }

    void UdpTransport::ReceiverLoop()
    {
        std::array<char, 4096>
            buffer{};

        while (_running.load()) {
            sockaddr_in from{};

            int fromLen =
                sizeof(from);

            const auto received =
                recvfrom(
                    _socket,
                    buffer.data(),
                    static_cast<int>(
                        buffer.size() - 1),
                    0,
                    reinterpret_cast<
                        sockaddr*>(
                        &from),
                    &fromLen);

            if (received ==
                SOCKET_ERROR) {
                const auto error =
                    WSAGetLastError();

                if (!_running.load()) {
                    break;
                }

                if (error ==
                        WSAETIMEDOUT ||
                    error ==
                        WSAEWOULDBLOCK) {
                    continue;
                }

                SKSE::log::error(
                    "UDP recvfrom failed: {}",
                    error);

                continue;
            }

            if (received <= 0) {
                continue;
            }

            buffer[
                static_cast<std::size_t>(
                    received)] =
                '\0';

            const std::string packet(
                buffer.data(),
                static_cast<
                    std::size_t>(
                    received));

            if (_config.autoDiscovery &&
                HandleDiscoveryPacket(
                    packet,
                    from)) {
                continue;
            }

            TouchPeerFromGameplayPacket(
                from,
                packet);

            SKSE::log::info(
                "OSTNET RX {} {}",
                AddressToString(
                    from),
                packet);

            // Network receiver is a worker thread. Skyrim actor lookup and
            // OStim calls stay on the game thread.
            if (auto* tasks =
                    SKSE::GetTaskInterface()) {
                tasks->AddTask(
                    [packet]() mutable {
                        ActorResolver::
                            GetSingleton().
                            HandleUdpPacket(
                                std::move(
                                    packet));
                    });
            }
        }
    }
}
