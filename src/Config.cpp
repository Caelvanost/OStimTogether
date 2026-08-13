#include "PCH.h"
#include "Config.h"

#include <Windows.h>

namespace OStimTogether
{
    namespace
    {
        constexpr wchar_t kIniPath[] =
            L".\\Data\\SKSE\\Plugins\\OStimTogether.ini";
        constexpr wchar_t kRelayHostIniPath[] =
            L".\\Data\\SKSE\\Plugins\\OStimTogether_RelayHost.ini";

        std::string ReadString(
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* fallback,
            const wchar_t* path = kIniPath)
        {
            wchar_t buffer[2048]{};

            GetPrivateProfileStringW(
                section,
                key,
                fallback,
                buffer,
                static_cast<DWORD>(
                    std::size(buffer)),
                path);

            if (buffer[0] == L'\0') {
                return {};
            }

            const int needed =
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    buffer,
                    -1,
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

            if (needed <= 1) {
                return {};
            }

            // Include room for the terminating NUL while converting.
            std::string converted(
                static_cast<std::size_t>(needed),
                '\0');

            const auto written =
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    buffer,
                    -1,
                    converted.data(),
                    needed,
                    nullptr,
                    nullptr);

            if (written <= 1) {
                return {};
            }

            converted.resize(
                static_cast<std::size_t>(
                    written - 1));

            return converted;
        }

        bool ReadBool(
            const wchar_t* section,
            const wchar_t* key,
            bool fallback,
            const wchar_t* path = kIniPath)
        {
            return
                GetPrivateProfileIntW(
                    section,
                    key,
                    fallback ? 1 : 0,
                    path) != 0;
        }

        std::uint32_t ReadUInt(
            const wchar_t* section,
            const wchar_t* key,
            std::uint32_t fallback,
            const wchar_t* path = kIniPath)
        {
            return static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    section,
                    key,
                    fallback,
                    path));
        }

        std::uint32_t Clamp(
            std::uint32_t value,
            std::uint32_t minValue,
            std::uint32_t maxValue)
        {
            return std::max(
                minValue,
                std::min(
                    value,
                    maxValue));
        }

        std::uint16_t ClampPort(
            std::uint32_t value,
            std::uint16_t fallback)
        {
            if (value == 0 ||
                value > 65535) {
                return fallback;
            }

            return static_cast<std::uint16_t>(
                value);
        }

        std::string Trim(
            std::string value)
        {
            const auto isSpace =
                [](unsigned char ch) {
                    return std::isspace(ch) != 0;
                };

            value.erase(
                value.begin(),
                std::find_if_not(
                    value.begin(),
                    value.end(),
                    isSpace));

            value.erase(
                std::find_if_not(
                    value.rbegin(),
                    value.rend(),
                    isSpace).base(),
                value.end());

            return value;
        }

        std::optional<Config::RemotePeer>
            ParseRemotePeer(
                std::string value,
                std::uint16_t defaultPort)
        {
            value =
                Trim(
                    std::move(value));

            if (value.empty()) {
                return std::nullopt;
            }

            Config::RemotePeer result{};
            result.port =
                defaultPort;

            const auto separator =
                value.rfind(':');

            if (separator !=
                std::string::npos) {
                const auto portText =
                    Trim(
                        value.substr(
                            separator + 1));

                try {
                    const auto parsed =
                        std::stoul(
                            portText);

                    if (parsed == 0 ||
                        parsed > 65535) {
                        return std::nullopt;
                    }

                    result.port =
                        static_cast<std::uint16_t>(
                            parsed);

                    value.resize(
                        separator);
                } catch (...) {
                    return std::nullopt;
                }
            }

            result.host =
                Trim(
                    std::move(value));

            if (result.host.empty() ||
                result.host.find('|') !=
                    std::string::npos) {
                return std::nullopt;
            }

            return result;
        }

        std::vector<Config::RemotePeer>
            ParseRemotePeers(
                std::string value,
                std::uint16_t defaultPort)
        {
            std::vector<Config::RemotePeer>
                result;
            std::unordered_set<std::string>
                seen;

            std::size_t start = 0;

            while (start <= value.size()) {
                const auto end =
                    value.find_first_of(
                        ",;",
                        start);

                const auto item =
                    value.substr(
                        start,
                        end == std::string::npos ?
                            std::string::npos :
                            end - start);

                if (auto peer =
                        ParseRemotePeer(
                            item,
                            defaultPort)) {
                    auto key =
                        peer->host;

                    std::transform(
                        key.begin(),
                        key.end(),
                        key.begin(),
                        [](unsigned char ch) {
                            return static_cast<char>(
                                std::tolower(ch));
                        });

                    key =
                        fmt::format(
                            "{}:{}",
                            key,
                            peer->port);

                    if (seen.insert(key).second) {
                        result.push_back(
                            std::move(*peer));
                    }
                } else if (!Trim(item).empty()) {
                    SKSE::log::warn(
                        "OSTNET ignored invalid RemotePeers entry: \"{}\"",
                        Trim(item));
                }

                if (result.size() >= 64) {
                    SKSE::log::warn(
                        "OSTNET RemotePeers limited to 64 entries");
                    break;
                }

                if (end == std::string::npos) {
                    break;
                }

                start =
                    end + 1;
            }

            return result;
        }
    }

    Config Config::Load()
    {
        Config cfg{};

        cfg.toggleKey =
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    L"General",
                    L"ToggleKey",
                    cfg.toggleKey,
                    kIniPath));

        cfg.clearKey =
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    L"General",
                    L"ClearKey",
                    cfg.clearKey,
                    kIniPath));

        cfg.intervalMs =
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    L"General",
                    L"IntervalMs",
                    cfg.intervalMs,
                    kIniPath));

        cfg.slotMask =
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    L"Equipment",
                    L"SlotMask",
                    cfg.slotMask,
                    kIniPath));

        cfg.debugNotifications =
            ReadBool(
                L"General",
                L"DebugNotifications",
                cfg.debugNotifications);

        // AutoDiscovery defaults to ON and makes the legacy network keys
        // irrelevant for LAN use. RemotePeers/AutoRemoteFromSTR can coexist
        // with LAN discovery for mixed local/Internet sessions.
        cfg.autoDiscovery =
            ReadBool(
                L"Network",
                L"AutoDiscovery",
                true);

        cfg.relayMode =
            ReadBool(
                L"Network",
                L"RelayMode",
                cfg.relayMode);

        // Explicit opt-out for advanced users. This is a NEW key; the old
        // Enabled=0 key is deliberately ignored in automatic mode.
        cfg.networkEnabled =
            !ReadBool(
                L"Network",
                L"Disabled",
                false);

        cfg.localPort =
            ClampPort(
                ReadUInt(
                    L"Network",
                    L"LocalPort",
                    cfg.localPort),
                27991);

        cfg.peerPort =
            cfg.localPort;

        cfg.autoRemotePort =
            cfg.localPort;

        cfg.autoRemoteFromSTR =
            ReadBool(
                L"Network",
                L"AutoRemoteFromSTR",
                cfg.autoRemoteFromSTR);

        cfg.autoSharedSecretFromSTR =
            ReadBool(
                L"Network",
                L"AutoSharedSecretFromSTR",
                cfg.autoSharedSecretFromSTR);

        cfg.autoRemotePort =
            ClampPort(
                ReadUInt(
                    L"Network",
                    L"AutoRemotePort",
                    cfg.autoRemotePort),
                cfg.localPort);

        cfg.discoveryIntervalMs =
            Clamp(
                ReadUInt(
                    L"Network",
                    L"DiscoveryIntervalMs",
                    cfg.discoveryIntervalMs),
                250,
                5000);

        cfg.peerTimeoutMs =
            Clamp(
                ReadUInt(
                    L"Network",
                    L"PeerTimeoutMs",
                    cfg.peerTimeoutMs),
                3000,
                60000);

        cfg.peerHost =
            ReadString(
                L"Network",
                L"PeerHost",
                L"");

        cfg.peerPort =
            ClampPort(
                ReadUInt(
                    L"Network",
                    L"PeerPort",
                    cfg.peerPort),
                cfg.localPort);

        cfg.remotePeers =
            ParseRemotePeers(
                ReadString(
                    L"Network",
                    L"RemotePeers",
                    L""),
                cfg.peerPort);

        if (auto legacyPeer =
                ParseRemotePeer(
                    cfg.peerHost,
                    cfg.peerPort)) {
            const auto duplicate =
                std::any_of(
                    cfg.remotePeers.begin(),
                    cfg.remotePeers.end(),
                    [&](const Config::RemotePeer& peer) {
                        return
                            _stricmp(
                                peer.host.c_str(),
                                legacyPeer->host.c_str()) == 0 &&
                            peer.port ==
                                legacyPeer->port;
                    });

            if (!duplicate) {
                cfg.remotePeers.push_back(
                    std::move(*legacyPeer));
            }
        }

        cfg.sharedSecret =
            ReadString(
                L"Network",
                L"SharedSecret",
                L"");

        if (GetFileAttributesW(kRelayHostIniPath) !=
            INVALID_FILE_ATTRIBUTES) {
            cfg.autoDiscovery =
                ReadBool(
                    L"Network",
                    L"AutoDiscovery",
                    cfg.autoDiscovery,
                    kRelayHostIniPath);

            cfg.relayMode =
                ReadBool(
                    L"Network",
                    L"RelayMode",
                    cfg.relayMode,
                    kRelayHostIniPath);

            cfg.autoRemoteFromSTR =
                ReadBool(
                    L"Network",
                    L"AutoRemoteFromSTR",
                    cfg.autoRemoteFromSTR,
                    kRelayHostIniPath);

            cfg.autoSharedSecretFromSTR =
                ReadBool(
                    L"Network",
                    L"AutoSharedSecretFromSTR",
                    cfg.autoSharedSecretFromSTR,
                    kRelayHostIniPath);

            cfg.localPort =
                ClampPort(
                    ReadUInt(
                        L"Network",
                        L"LocalPort",
                        cfg.localPort,
                        kRelayHostIniPath),
                    cfg.localPort);

            cfg.autoRemotePort =
                ClampPort(
                    ReadUInt(
                        L"Network",
                        L"AutoRemotePort",
                        cfg.autoRemotePort,
                        kRelayHostIniPath),
                    cfg.autoRemotePort);

            const auto relaySharedSecret =
                ReadString(
                    L"Network",
                    L"SharedSecret",
                    L"",
                    kRelayHostIniPath);

            if (!relaySharedSecret.empty()) {
                cfg.sharedSecret =
                    relaySharedSecret;
            }
        }

        cfg.intervalMs =
            Clamp(
                cfg.intervalMs,
                25,
                1000);

        return cfg;
    }
}
