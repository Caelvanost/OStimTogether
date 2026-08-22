#include "PCH.h"

#include "AddonBridge.h"
#include "AddonStateRepair.h"
#include "CoopSessionManager.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"
#include "FreeSceneAlignmentFix.h"
#include "FreeSceneRootSync.h"
#include "Input.h"
#include "MirrorUndressRepair.h"
#include "OStimBridge.h"
#include "PapyrusConsentBridge.h"
#include "PreflightGuard.h"
#include "ProxyOnlyThreadCleanup.h"
#include "RaceMenuOverlayBridge.h"
#include "SharedSceneControl.h"
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

            // Ordinary free-standing scenes never receive direct
            // TESObjectREFR/3D warps from OStim Together after native startup.
            OStimTogether::FreeSceneAlignmentFix::GetSingleton().Initialize();

            // v0.30.0 restores only the animated NPC Root [Root] LOCAL
            // translation on remote STR proxies. Rotation, scale, world
            // transforms, descendants and actor/reference position are never
            // copied from another client.
            OStimTogether::FreeSceneRootSync::GetSingleton().Initialize();

            // A pre-scene OStim UI path can create a one-actor thread that
            // contains only the targeted STR proxy. Such a thread survived in
            // the 0.29.0 test and made later mirror builders reject that actor.
            // Stop any mapped-proxy thread that contains no real local player.
            OStimTogether::ProxyOnlyThreadCleanup::GetSingleton().Initialize();

            OStimTogether::CoopSessionManager::GetSingleton().Initialize();

            // OStim mirror threads are created with NO_PLAYER_CONTROL for
            // safety. Re-enable OStim's own SceneMenu only after the core
            // routing listeners are registered so any local NODE/SPEED/STOP
            // action is immediately converted into a multiplayer control
            // request.
            OStimTogether::SharedSceneControl::GetSingleton().Initialize();

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
            } else if (!OStimTogether::FreeSceneRootSync::GetSingleton().StartTransport()) {
                SKSE::log::warn(
                    "OSTNET ROOT TRANSLATION transport unavailable: free-scene visual translation sync disabled");
            }

            OStimTogether::VisualKeepAlive::GetSingleton().Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            OStimTogether::EquipmentLock::GetSingleton().ClearAllOStimTargets();
            OStimTogether::EquipmentLock::GetSingleton().ClearManual();
            OStimTogether::DefaultOutfitGuard::GetSingleton().RestoreAll();
            OStimTogether::CoopSessionManager::GetSingleton().Reset();
            OStimTogether::OStimBridge::GetSingleton().ResetRemoteState();
            OStimTogether::FreeSceneRootSync::GetSingleton().Reset();
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