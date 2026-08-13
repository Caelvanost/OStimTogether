#include "PCH.h"
#include "Config.h"

#include <Windows.h>

namespace OStimTogether
{
    namespace
    {
        constexpr wchar_t kIniPath[] =
            L".\\Data\\SKSE\\Plugins\\OStimTogether.ini";

        std::string ReadString(
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* fallback)
        {
            wchar_t buffer[512]{};

            GetPrivateProfileStringW(
                section,
                key,
                fallback,
                buffer,
                static_cast<DWORD>(
                    std::size(buffer)),
                kIniPath);

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
            bool fallback)
        {
            return
                GetPrivateProfileIntW(
                    section,
                    key,
                    fallback ? 1 : 0,
                    kIniPath) != 0;
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
        // irrelevant. This is intentional so an old INI with Enabled=0
        // does not disable networking after a mod update.
        cfg.autoDiscovery =
            ReadBool(
                L"Network",
                L"AutoDiscovery",
                true);

        // Explicit opt-out for advanced users. This is a NEW key; the old
        // Enabled=0 key is deliberately ignored in automatic mode.
        cfg.networkEnabled =
            !ReadBool(
                L"Network",
                L"Disabled",
                false);

        auto localPort =
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(
                    L"Network",
                    L"LocalPort",
                    cfg.localPort,
                    kIniPath));

        if (localPort == 0 ||
            localPort > 65535) {
            localPort = 27991;
        }

        cfg.localPort =
            static_cast<std::uint16_t>(
                localPort);

        cfg.peerPort =
            cfg.localPort;

        cfg.discoveryIntervalMs =
            Clamp(
                static_cast<std::uint32_t>(
                    GetPrivateProfileIntW(
                        L"Network",
                        L"DiscoveryIntervalMs",
                        cfg.discoveryIntervalMs,
                        kIniPath)),
                250,
                5000);

        cfg.peerTimeoutMs =
            Clamp(
                static_cast<std::uint32_t>(
                    GetPrivateProfileIntW(
                        L"Network",
                        L"PeerTimeoutMs",
                        cfg.peerTimeoutMs,
                        kIniPath)),
                3000,
                60000);

        // Manual fallback remains possible, but is never required.
        if (!cfg.autoDiscovery) {
            cfg.peerHost =
                ReadString(
                    L"Network",
                    L"PeerHost",
                    L"");

            auto peerPort =
                static_cast<std::uint32_t>(
                    GetPrivateProfileIntW(
                        L"Network",
                        L"PeerPort",
                        cfg.peerPort,
                        kIniPath));

            if (peerPort == 0 ||
                peerPort > 65535) {
                peerPort =
                    cfg.localPort;
            }

            cfg.peerPort =
                static_cast<std::uint16_t>(
                    peerPort);
        }

        cfg.intervalMs =
            Clamp(
                cfg.intervalMs,
                25,
                1000);

        return cfg;
    }
}
