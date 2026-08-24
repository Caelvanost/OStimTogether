#include "PCH.h"
#include "VisualKeepAlive.h"
#include "FreeSceneRootSync.h"
#include "FreeSceneSelfOriginLock.h"
#include "FreeSceneOverlayReplay.h"
#include "OCumStateSync.h"
#include "OStimBridge.h"

namespace OStimTogether
{
    namespace
    {
        // Wake faster than a rendered frame, but queue at most one refresh
        // task. The game-thread task naturally coalesces this to one visual
        // refresh per frame without building a backlog.
        constexpr auto kRefreshPollInterval =
            std::chrono::milliseconds(4);
    }

    VisualKeepAlive& VisualKeepAlive::GetSingleton()
    {
        static VisualKeepAlive instance;
        return instance;
    }

    VisualKeepAlive::~VisualKeepAlive()
    {
        Stop();
    }

    void VisualKeepAlive::Start()
    {
        if (_running.exchange(true)) {
            return;
        }

        // Safety hotfix: v0.35.2 attempted to deep-clone live NiSkinInstance
        // objects for Body [OvlN]. CrashLogger confirmed an access violation in
        // Skyrim's DeepCopyStream during climax while cloning an overlay skin.
        // Keep the OCumOverlaySkinFix code compiled for future diagnostics, but
        // do not initialize or tick it until a non-cloning relink strategy is
        // implemented and validated.
        SKSE::log::warn(
            "OSTNET OCUM DIRECT SKIN DISABLED reason=unsafe-live-NiSkinInstance-deep-copy hotfix=0.35.3");

        // 0.35.4: physical-furniture scenes are currently the known-good
        // baseline. Free/wall scenes get a dedicated non-destructive replay
        // that reuses direct Body [OvlN] materialization and a lightweight SKEE
        // update instead of allowing the old proxy Remove/Add pass to win.
        FreeSceneOverlayReplay::GetSingleton().Initialize();

        _refreshQueued.store(false);

        _worker = std::jthread(
            [this](std::stop_token token) {
                WorkerLoop(token);
            });

        SKSE::log::info(
            "STR visual keepalive started: frame-coalesced poll={}ms",
            kRefreshPollInterval.count());
    }

    void VisualKeepAlive::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }

        if (_worker.joinable()) {
            _worker.request_stop();
            _worker.join();
        }

        _refreshQueued.store(false);

        SKSE::log::info(
            "STR visual keepalive stopped");
    }

    void VisualKeepAlive::WorkerLoop(
        std::stop_token stopToken)
    {
        while (!stopToken.stop_requested() &&
               _running.load()) {
            std::this_thread::sleep_for(
                kRefreshPollInterval);

            if (stopToken.stop_requested() ||
                !_running.load()) {
                break;
            }

            // OStim data and RE scene-graph objects are only touched on
            // Skyrim's game thread. Keep no more than one outstanding task so
            // a slow frame cannot accumulate delayed corrections.
            if (!_refreshQueued.exchange(true)) {
                if (auto* tasks =
                        SKSE::GetTaskInterface()) {
                    tasks->AddTask(
                        []() {
                            OStimBridge::GetSingleton()
                                .RefreshRemoteMirrors();

                            // Historical root-translation probe. It remains
                            // uninitialized/disabled in current runtime builds
                            // and therefore performs no skeleton writes.
                            FreeSceneRootSync::GetSingleton()
                                .Tick();

                            // Keep only the remote STR proxy's logical origin
                            // on the common free-scene center. The real local
                            // PlayerCharacter and all rendered roots remain
                            // OStim-owned.
                            FreeSceneSelfOriginLock::GetSingleton()
                                .Tick();

                            // Detect live OCum equip-object and RaceMenu overlay
                            // state through the established non-destructive path.
                            // The experimental direct skin-clone repair is
                            // intentionally disabled in v0.35.3+.
                            OCumStateSync::GetSingleton()
                                .Tick();

                            // Runs immediately after OCumStateSync so a free-
                            // scene direct/light replay can advance the SKEE
                            // generation before any queued proxy hard reinstall.
                            // Physical TESFurniture scenes are explicitly
                            // excluded by this module.
                            FreeSceneOverlayReplay::GetSingleton()
                                .Tick();

                            VisualKeepAlive::GetSingleton().
                                _refreshQueued.store(false);
                        });
                } else {
                    _refreshQueued.store(false);
                }
            }
        }
    }
}
