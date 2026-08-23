#pragma once

#include "PCH.h"
#include "SceneCenter.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Keeps only the real local PlayerCharacter's logical TESObjectREFR origin
    // on the shared free-scene center. The rendered 3D/skeleton is never
    // touched. Skyrim Together then publishes the common logical origin to the
    // remote proxy while OStim/animation remains free to provide the visible
    // per-role displacement.
    class FreeSceneSelfOriginLock
    {
    public:
        static FreeSceneSelfOriginLock& GetSingleton();

        bool Initialize();
        void Reset();
        void Tick();

    private:
        FreeSceneSelfOriginLock() = default;

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
        void QueueArmAfterStart(std::int32_t threadID);
        void ArmAfterStart(std::int32_t threadID);

        [[nodiscard]] bool IsFreeStandingThread(OStim::Thread* thread) const;
        [[nodiscard]] RE::PlayerCharacter* FindLocalPlayer(OStim::Thread* thread) const;
        [[nodiscard]] bool HasSTRRemoteParticipant(OStim::Thread* thread) const;

        OStim::ThreadInterface* _threads{ nullptr };
        std::uint32_t _threadInterfaceVersion{ 0 };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        std::int32_t _activeThreadID{ -1 };
        SceneCenter _center{};
        std::chrono::steady_clock::time_point _lastLog{};
    };
}
