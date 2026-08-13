#include "PCH.h"
#include "VisualKeepAlive.h"
#include "OStimBridge.h"

namespace OStimTogether
{
    namespace
    {
        // Position is the most aggressively overwritten part by STR.
        constexpr auto kAlignmentInterval =
            std::chrono::milliseconds(25);
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

        _worker = std::jthread(
            [this](std::stop_token token) {
                WorkerLoop(token);
            });

        SKSE::log::info(
            "STR visual keepalive started: alignment+direct-changenode={}ms",
            kAlignmentInterval.count());
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

        SKSE::log::info(
            "STR visual keepalive stopped");
    }

    void VisualKeepAlive::WorkerLoop(
        std::stop_token stopToken)
    {
        while (!stopToken.stop_requested() &&
               _running.load()) {
            std::this_thread::sleep_for(
                kAlignmentInterval);

            if (stopToken.stop_requested() ||
                !_running.load()) {
                break;
            }

            // OStim data and RE objects are only touched on
            // Skyrim's game thread.
            if (auto* tasks =
                    SKSE::GetTaskInterface()) {
                tasks->AddTask(
                    []() {
                        OStimBridge::GetSingleton()
                            .RefreshRemoteMirrors();
                    });
            }
        }
    }
}
