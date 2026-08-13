#include "PCH.h"

#include "AddonBridge.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"
#include "Input.h"
#include "OStimBridge.h"
#include "RaceMenuOverlayBridge.h"
#include "UdpTransport.h"
#include "VisualKeepAlive.h"

namespace
{
    void InitLogging()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }

        *path /= "OStimTogether.log";

        auto sink =
            std::make_shared<
                spdlog::sinks::basic_file_sink_mt>(
                    path->string(),
                    true);

        auto log =
            std::make_shared<spdlog::logger>(
                "OStimTogether",
                std::move(sink));

        spdlog::set_default_logger(
            std::move(log));

        spdlog::set_level(
            spdlog::level::trace);

        spdlog::flush_on(
            spdlog::level::trace);
    }

    void OnSKSEMessage(
        SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            OStimTogether::RaceMenuOverlayBridge::
                GetSingleton().Initialize();

            OStimTogether::OStimBridge::
                GetSingleton().Initialize();
            break;

        case SKSE::MessagingInterface::kInputLoaded:
            OStimTogether::InputHandler::
                GetSingleton().Register();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            OStimTogether::AddonBridge::
                GetSingleton().Register();

            OStimTogether::EquipmentLock::
                GetSingleton().Start();

            OStimTogether::UdpTransport::
                GetSingleton().Start();

            OStimTogether::VisualKeepAlive::
                GetSingleton().Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            OStimTogether::EquipmentLock::
                GetSingleton().ClearAllOStimTargets();

            OStimTogether::EquipmentLock::
                GetSingleton().ClearManual();

            OStimTogether::DefaultOutfitGuard::
                GetSingleton().RestoreAll();

            OStimTogether::OStimBridge::
                GetSingleton().ResetRemoteState();
            break;

        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    InitLogging();
    SKSE::Init(skse);

    SKSE::log::info(
        "OStim Together v0.19.3 loading");

    auto* messaging =
        SKSE::GetMessagingInterface();

    if (!messaging) {
        SKSE::log::critical(
            "No SKSE messaging interface");
        return false;
    }

    messaging->RegisterListener(
        OnSKSEMessage);

    return true;
}
