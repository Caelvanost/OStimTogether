#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Physical-furniture companion for the OCum RaceMenu overlay path.
    //
    // Free-standing scenes already materialize CumOverlays correctly through
    // RaceMenuOverlayBridge + SKEEOverlayRefresh. Physical furniture can finish
    // an OStim body/animation attachment after that first materialization. This
    // class does not invent a second rendering path: it simply replays the
    // already-validated free-scene path after furniture START/NODE settling and
    // after a CumOverlays snapshot changes while that furniture scene is active.
    class FurnitureOverlayReplay
    {
    public:
        static FurnitureOverlayReplay& GetSingleton();

        bool Initialize();
        void Reset();

        // Called from VisualKeepAlive on Skyrim's game thread. Internally
        // rate-limited and a no-op outside physical-furniture OStim scenes.
        void Tick();

    private:
        FurnitureOverlayReplay() = default;

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

        [[nodiscard]] bool IsPhysicalFurniture(
            OStim::Thread* thread,
            RE::FormID* furnitureFormID = nullptr) const;

        void ScheduleReplay(
            std::int32_t threadID,
            std::string_view trigger);

        void RunReplayPass(
            std::int32_t threadID,
            std::uint64_t generation,
            RE::FormID expectedFurniture,
            std::string_view trigger,
            std::string_view phase);

        static std::string BuildSignature(
            const std::vector<std::string>& chunks);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        NodeListener _nodeListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        std::unordered_set<std::int32_t> _activeFurnitureThreads;
        std::unordered_map<std::int32_t, std::uint64_t> _replayGeneration;
        std::unordered_map<RE::FormID, std::string> _overlaySignatures;
        std::uint64_t _nextGeneration{ 1 };
        std::chrono::steady_clock::time_point _nextPoll{};
    };
}
