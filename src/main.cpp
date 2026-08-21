#include "PCH.h"

#include "AddonBridge.h"
#include "AddonStateRepair.h"
#include "CoopSessionManager.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"
#include "Input.h"
#include "MirrorUndressRepair.h"
#include "OStimBridge.h"
#include "RaceMenuOverlayBridge.h"
#include "STRPMTransport.h"
#include "VisualKeepAlive.h"

#ifndef OSTIM_TOGETHER_VERSION
#define OSTIM_TOGETHER_VERSION "dev"
#endif

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

            OStimTogether::CoopSessionManager::
                GetSingleton().Initialize();

            OStimTogether::MirrorUndressRepair::
                GetSingleton().Initialize();

            OStimTogether::AddonStateRepair::
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

            // STRPM is the only multiplayer transport on this branch.
            // If it is unavailable, synchronization is intentionally disabled
            // rather than falling back to the unvalidated custom UDP stack.
            if (!OStimTogether::STRPMTransport::
                    GetSingleton().Start()) {
                SKSE::log::error(
                    "OSTNET STRPM unavailable: multiplayer synchronization disabled; no UDP fallback");
            }

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

            OStimTogether::CoopSessionManager::
                GetSingleton().Reset();

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
        "OStim Together v{} loading (STRPM-only transport)",
        OSTIM_TOGETHER_VERSION);

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
