#include "PCH.h"

#include "AddonBridge.h"
#include "AddonStateRepair.h"
#include "CoopSessionManager.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"
#include "FreeSceneAlignmentFix.h"
#include "Input.h"
#include "MirrorUndressRepair.h"
#include "OStimBridge.h"
#include "PapyrusConsentBridge.h"
#include "PreflightGuard.h"
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

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            path->string(), true);
        auto log = std::make_shared<spdlog::logger>(
            "OStimTogether", std::move(sink));

        spdlog::set_default_logger(std::move(log));
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            OStimTogether::RaceMenuOverlayBridge::GetSingleton().Initialize();

            // Must be registered before every other OStim Together START
            // listener. It classifies the disposable pre-consent thread before
            // OStimBridge can arm authoritative wall/pose/network tasks.
            OStimTogether::PreflightGuard::GetSingleton().Initialize();

            OStimTogether::OStimBridge::GetSingleton().Initialize();

            // Register after OStimBridge: free-standing scenes keep the
            // bridge's short initial proxy settle, then this listener removes
            // only the long-lived no-furniture/non-wall position pin.
            OStimTogether::FreeSceneAlignmentFix::GetSingleton().Initialize();

            OStimTogether::CoopSessionManager::GetSingleton().Initialize();
            OStimTogether::MirrorUndressRepair::GetSingleton().Initialize();
            OStimTogether::AddonStateRepair::GetSingleton().Initialize();
            break;

        case SKSE::MessagingInterface::kInputLoaded:
            OStimTogether::InputHandler::GetSingleton().Register();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            OStimTogether::AddonBridge::GetSingleton().Register();
            OStimTogether::EquipmentLock::GetSingleton().Start();

            if (!OStimTogether::STRPMTransport::GetSingleton().Start()) {
                SKSE::log::error(
                    "OSTNET STRPM unavailable: multiplayer synchronization disabled; no UDP fallback");
            }

            OStimTogether::VisualKeepAlive::GetSingleton().Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            OStimTogether::EquipmentLock::GetSingleton().ClearAllOStimTargets();
            OStimTogether::EquipmentLock::GetSingleton().ClearManual();
            OStimTogether::DefaultOutfitGuard::GetSingleton().RestoreAll();
            OStimTogether::CoopSessionManager::GetSingleton().Reset();
            OStimTogether::OStimBridge::GetSingleton().ResetRemoteState();
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

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        SKSE::log::critical("No SKSE messaging interface");
        return false;
    }

    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(OStimTogether::PapyrusConsentBridge::Register);
    } else {
        SKSE::log::warn(
            "OSTNET PAPYRUS CONSENT bridge unavailable: no Papyrus interface");
    }

    messaging->RegisterListener(OnSKSEMessage);
    return true;
}