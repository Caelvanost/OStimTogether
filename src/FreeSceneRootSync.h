#pragma once

#include "PCH.h"
#include "STRPMApi.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    // Free-standing OStim scenes place every actor reference on a shared scene
    // origin and obtain the visible per-role displacement from the animated
    // skeleton root. Skyrim Together correctly owns the remote player's
    // TESObjectREFR position, but its proxy can lose/overwrite that visual root
    // displacement. This component synchronizes only the animated
    // "NPC Root [Root]" local transform. It never writes actor/reference world
    // position and therefore does not fight STR movement replication.
    class FreeSceneRootSync
    {
    public:
        static FreeSceneRootSync& GetSingleton();

        bool Initialize();
        void Reset();

        // Called from VisualKeepAlive on Skyrim's game thread once per
        // coalesced rendered-frame task.
        void Tick();

        // ROOTBONE packets are consumed before the generic scene packet
        // handlers. The packet is stored and applied by Tick() only when the
        // sender's STR proxy is an actor in the active local OStim player
        // thread.
        bool HandleIncoming(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view payload);

    private:
        FreeSceneRootSync() = default;

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

        struct RootTransform
        {
            RE::NiPoint3 translate{};
            RE::NiMatrix3 rotate{};

            [[nodiscard]] bool IsFinite() const noexcept;
        };

        struct RemoteState
        {
            std::int32_t remoteThreadID{ -1 };
            RootTransform transform{};
            std::chrono::steady_clock::time_point received{};
            std::chrono::steady_clock::time_point lastLog{};
        };

        void HandleStart(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);

        [[nodiscard]] bool IsFreeStandingThread(OStim::Thread* thread) const;
        [[nodiscard]] bool ThreadContainsActor(
            OStim::Thread* thread,
            RE::Actor* actor) const;

        static RE::NiAVObject* FindAnimatedRoot(RE::Actor* actor);
        static void UpdateNodeWorldTransform(RE::NiAVObject* node);
        static void UpdateTreeTransforms(RE::NiAVObject* node);
        static void ApplyRootTransform(
            RE::Actor* actor,
            const RootTransform& transform);

        static std::optional<std::string> Field(
            std::string_view payload,
            std::string_view key);

        OStim::ThreadInterface* _threads{ nullptr };
        std::uint32_t _threadInterfaceVersion{ 0 };
        StartListener _startListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        std::int32_t _activePlayerThreadID{ -1 };
        std::chrono::steady_clock::time_point _lastSend{};
        std::chrono::steady_clock::time_point _lastSendLog{};

        std::unordered_map<STRPMApi::ConnectionID, RemoteState> _remoteStates;
    };
}
