#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    class OCumStateSync
    {
    public:
        static OCumStateSync& GetSingleton();

        bool Initialize();
        void Reset();

        // Called on Skyrim's game thread by VisualKeepAlive. Polling is
        // internally rate-limited; it only watches actors in active OStim
        // threads and is a no-op when OCum.esp is absent.
        void Tick();

        // Sends the real local player's current OCum RaceMenu overlays and
        // vaginal/anal equip-object state through the generic ADDON protocol.
        // Safe no-op when OCum.esp is not installed.
        void SendLocalSnapshot(std::string_view reason);

    private:
        OCumStateSync() = default;

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

        struct MeshState
        {
            bool initialized{ false };
            bool vaginal{ false };
            bool anal{ false };
            std::chrono::steady_clock::time_point lastVisualRefresh{};

            // RaceMenu overlay state is separate from OCum's worn mesh state.
            // Track a compact signature so a real overlay/node rebuild is only
            // requested when CumOverlays actually changes, never every poll.
            bool overlayInitialized{ false };
            std::string overlaySignature;
            std::chrono::steady_clock::time_point lastOverlayPoll{};
        };

        void HandleStart(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);
        void SendLocalObjectState(
            RE::PlayerCharacter* player,
            std::string_view type,
            bool equipped,
            std::string_view reason);

        static std::string HexEncode(std::string_view value);
        static std::string BuildOverlaySignature(
            const std::vector<std::string>& chunks);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };
        std::unordered_set<std::int32_t> _activeThreads;
        std::unordered_map<RE::FormID, MeshState> _meshStates;
        std::chrono::steady_clock::time_point _nextPoll{};
    };
}
