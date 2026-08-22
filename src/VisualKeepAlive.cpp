#include "PCH.h"
#include "VisualKeepAlive.h"
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

                            // v0.28.1 intentionally performs no remote
                            // skeleton-root writes here. The v0.28.0
                            // FreeSceneRootSync experiment could deform the
                            // complete actor hierarchy when local transforms
                            // from one animated skeleton were copied into an
                            // independently evaluated proxy skeleton.

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
