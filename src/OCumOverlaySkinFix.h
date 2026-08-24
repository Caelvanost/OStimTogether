#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Repairs RaceMenu Body [OvlN] skin binding without asking RaceMenu to
    // rediscover the body source. OStim body/furniture transitions can make
    // QueueOverlayBuild pick a small auxiliary skinned geometry instead of the
    // actual rendered body. We select the current full body ourselves and bind
    // the existing overlay geometries directly to its skin instance.
    class OCumOverlaySkinFix
    {
    public:
        static OCumOverlaySkinFix& GetSingleton();

        bool Initialize();
        void Tick();

        void Reset()
        {
            _activeThreads.clear();
            _overlaySignatures.clear();
            _nextPoll = {};
        }

    private:
        OCumOverlaySkinFix() = default;

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

        struct BodySource
        {
            RE::BSGeometry* geometry{ nullptr };
            std::uint32_t vertices{ 0 };
            std::uint32_t matrices{ 0 };
            std::uint64_t score{ 0 };
            std::string name;
        };

        struct RebindResult
        {
            std::uint32_t found{ 0 };
            std::uint32_t rebound{ 0 };
            std::uint32_t oldVertices{ 0 };
            std::uint32_t oldMatrices{ 0 };
        };

        void HandleStart(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);

        static std::string BuildSignature(
            const std::vector<std::string>& chunks);
        static BodySource FindBestBodySource(RE::Actor* actor);
        static RebindResult RebindBodyOverlays(
            RE::Actor* actor,
            const BodySource& source);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };
        std::unordered_set<std::int32_t> _activeThreads;
        std::unordered_map<RE::FormID, std::string> _overlaySignatures;
        std::chrono::steady_clock::time_point _nextPoll{};
    };
}
