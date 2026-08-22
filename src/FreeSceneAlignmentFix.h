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

        struct ConvergenceState
        {
            std::uint64_t generation{ 0 };
            std::uint32_t stableSamples{ 0 };
            std::chrono::steady_clock::time_point started{};
            std::chrono::steady_clock::time_point lastLog{};
            bool released{ false };
        };

        void HandleStart(OStim::Thread* thread);
        void HandleNode(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);

        void ArmConvergence(
            std::int32_t threadID,
            std::chrono::milliseconds delay,
            std::string reason,
            bool rearmPoseGuard);

        void ScheduleConvergenceCheck(
            std::int32_t threadID,
            std::uint64_t generation,
            std::chrono::milliseconds delay,
            std::string reason);

        void CheckConvergence(
            std::int32_t threadID,
            std::uint64_t generation,
            std::string_view reason);

        void ReleaseFreeSceneProxy(
            std::int32_t threadID,
            std::uint64_t generation,
            std::string_view reason,
            bool timeout,
            float maxDistanceSq);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        NodeListener _nodeListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        std::mutex _stateMutex;
        std::unordered_map<std::int32_t, ConvergenceState> _states;
    };
}
