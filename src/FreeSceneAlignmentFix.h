#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    class FreeSceneAlignmentFix
    {
    public:
        static FreeSceneAlignmentFix& GetSingleton();
        bool Initialize();

    private:
        FreeSceneAlignmentFix() = default;

        class StartListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        class NodeListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        class StopListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        void HandleStart(OStim::Thread* thread);
        void HandleNode(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);
        void ScheduleFreeSceneRelease(
            std::int32_t threadID,
            std::chrono::milliseconds delay,
            std::string reason);
        void ReleaseFreeSceneProxy(
            std::int32_t threadID,
            std::string_view reason);

        void StartReferenceGuard(std::int32_t threadID);
        void StopReferenceGuard(std::int32_t threadID);
        void ApplyReferenceGuard(std::int32_t threadID);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        NodeListener _nodeListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        std::mutex _stateMutex;
        std::unordered_set<std::int32_t> _pendingRelease;
        std::unordered_set<std::int32_t> _releasedThreads;
        std::unordered_set<std::int32_t> _referenceGuardThreads;
        std::unordered_set<std::int32_t> _referenceGuardWorkers;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
            _lastReferenceGuardLog;
    };
}
