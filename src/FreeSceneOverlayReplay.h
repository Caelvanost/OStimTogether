#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Free/wall-scene companion for OCum RaceMenu overlays.
    //
    // Physical TESFurniture scenes are deliberately excluded: the current
    // 0.35.3 path rendered the first new climax correctly on both clients in
    // validation. Free scenes still allowed the generic proxy hard reinstall to
    // race the direct materialization path. This class observes only non-
    // TESFurniture OStim threads and promotes the already validated direct
    // player/proxy materialization path, with bounded delayed replays.
    class FreeSceneOverlayReplay
    {
    public:
        static FreeSceneOverlayReplay& GetSingleton();

        bool Initialize();
        void Reset();
        void Tick();

    private:
        FreeSceneOverlayReplay() = default;

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

        [[nodiscard]] bool IsPhysicalFurniture(
            OStim::Thread* thread) const;

        static std::string BuildSignature(
            const std::vector<std::string>& chunks);

        void ApplyDirect(
            RE::Actor* actor,
            const std::vector<std::string>& chunks,
            std::string_view phase);

        void ScheduleActorReplay(
            RE::FormID actorID,
            std::int32_t threadID);

        void RunActorReplay(
            RE::FormID actorID,
            std::int32_t threadID,
            std::uint64_t generation,
            std::string_view phase);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };
        bool _supportsFurniture{ false };

        std::unordered_set<std::int32_t> _activeThreads;
        std::unordered_map<RE::FormID, std::int32_t> _actorThreads;
        std::unordered_map<RE::FormID, std::string> _overlaySignatures;
        std::unordered_map<RE::FormID, std::uint64_t> _actorGenerations;
        std::uint64_t _nextGeneration{ 1 };
        std::chrono::steady_clock::time_point _nextPoll{};
    };
}
