#include "PCH.h"

#include "AddonBridge.h"
#include "AddonStateRepair.h"
#include "CoopSessionManager.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"
#include "FreeSceneAlignmentFix.h"
#include "FreeScenePhaseSync.h"
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

            // Synchronize animation replay phase for ordinary free-standing
            // multiplayer scenes. v0.30.4 forces this barrier through OStim's
            // own ModAPI SetSpeed(currentSpeed) replay path instead of direct
            // NotifyAnimationGraph calls, which were rejected in 0.30.3.
            OStimTogether::FreeScenePhaseSync::GetSingleton().Initialize();

            // v0.30.4 deliberately leaves FreeSceneRootSync uninitialized.
            // 0.30.3 logs proved NPC Root [Root].local.translate was always
            // (0,0,0) for the tested scenes, so writing that value to remote
            // proxies adds no alignment information and can only interfere
            // with locally evaluated animation state. Keep the source only for
            // future read-only diagnostics.
            SKSE::log::info(
                "OSTNET ROOT TRANSLATION DISABLED mode=probe-only reason=no-useful-root-delta skeletonWrites=0");

            // A pre-scene OStim UI path can create a one-actor thread that
            // contains only the targeted STR proxy. Stop any mapped-proxy
            // thread that contains no real local player.
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

            {
                const bool strpmReady =
                    OStimTogether::STRPMTransport::GetSingleton().Start();

                if (!strpmReady) {
                    SKSE::log::error(
                        "OSTNET STRPM unavailable: multiplayer synchronization disabled; no UDP fallback");
                } else if (!OStimTogether::FreeScenePhaseSync::GetSingleton().StartTransport()) {
                    SKSE::log::warn(
                        "OSTNET PHASE SYNC transport unavailable: free-scene phase barrier disabled");
                }
            }

            OStimTogether::VisualKeepAlive::GetSingleton().Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            OStimTogether::EquipmentLock::GetSingleton().ClearAllOStimTargets();
            OStimTogether::EquipmentLock::GetSingleton().ClearManual();
            OStimTogether::DefaultOutfitGuard::GetSingleton().RestoreAll();
            OStimTogether::CoopSessionManager::GetSingleton().Reset();
            OStimTogether::OStimBridge::GetSingleton().ResetRemoteState();
            OStimTogether::FreeScenePhaseSync::GetSingleton().Reset();
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
