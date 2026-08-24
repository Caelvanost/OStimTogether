#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Common RaceMenu body-relink path for OCum CumOverlays.
    // Applies equally to free-standing and physical-furniture OStim scenes.
    class OCumOverlayRelink
    {
    public:
        static OCumOverlayRelink& GetSingleton();

        bool Initialize();
        void Tick();

    private:
        OCumOverlayRelink() = default;

        class StartListener final : public OStim::ThreadEventListener
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
        void HandleStop(OStim::Thread* thread);
        void RelinkActor(
            RE::Actor* actor,
            const std::vector<std::string>& chunks,
            std::int32_t threadID,
            std::string_view trigger);
        void ScheduleMaterialReapply(
            RE::FormID actorID,
            std::uint64_t generation,
            std::chrono::milliseconds delay,
            std::string_view phase);

        static std::string BuildSignature(
            const std::vector<std::string>& chunks);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };
        std::unordered_set<std::int32_t> _activeThreads;
        std::unordered_map<RE::FormID, std::string> _overlaySignatures;
        std::unordered_map<RE::FormID, std::uint64_t> _actorGeneration;
        std::uint64_t _nextGeneration{ 1 };
        std::chrono::steady_clock::time_point _nextPoll{};
    };
}
