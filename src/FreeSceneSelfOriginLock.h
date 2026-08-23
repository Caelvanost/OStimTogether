#pragma once

#include "PCH.h"
#include "SceneCenter.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Legacy class name retained for source/package stability.
    //
    // Shared free scenes have two narrowly scoped responsibilities here:
    //   1. keep only each STR remote-player proxy's LOGICAL TESObjectREFR
    //      origin on the common scene center (no proxy 3D/skeleton write), and
    //   2. on a REMOTE MIRROR only, perform at most two one-shot corrections
    //      of the true local PlayerCharacter after a START/NODE animation has
    //      settled if its rendered root drifted away from that center.
    //
    // The second path is intentionally bounded per node. It is not the old
    // continuous self-position lock that caused oscillation.
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
        void ArmMirrorSelfCorrections(OStim::Thread* thread);
        void ApplyMirrorSelfCorrection(
            OStim::Thread* thread,
            RE::PlayerCharacter* localPlayer,
            std::uint32_t stage);

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

        std::string _mirrorSelfNodeID{};
        std::uint32_t _mirrorSelfCorrectionStage{ 0 };
        std::chrono::steady_clock::time_point _mirrorSelfCorrectionDue{};
    };
}
