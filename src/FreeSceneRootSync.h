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
    // "NPC Root [Root]" local transform on a dedicated latest-state STRPM
    // channel. It never writes actor/reference world position.
    class FreeSceneRootSync
    {
    public:
        static FreeSceneRootSync& GetSingleton();

        bool Initialize();
        bool StartTransport();
        void StopTransport();
        void Reset();

        // Called from VisualKeepAlive on Skyrim's game thread once per
        // coalesced rendered-frame task.
        void Tick();

    private:
        FreeSceneRootSync() = default;
        ~FreeSceneRootSync();

        FreeSceneRootSync(const FreeSceneRootSync&) = delete;
        FreeSceneRootSync& operator=(const FreeSceneRootSync&) = delete;

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

        static void __cdecl OnRootMessage(
            const STRPMApi::Message* message,
            void* userData);

        void HandleRootMessage(const STRPMApi::Message& message);
        void StoreIncoming(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view payload);

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

        const STRPMApi::Interface* _api{ nullptr };
        STRPMApi::ListenerHandle _rootListener{};
        std::atomic_bool _transportRunning{ false };

        std::int32_t _activePlayerThreadID{ -1 };
        std::chrono::steady_clock::time_point _lastSend{};
        std::chrono::steady_clock::time_point _lastSendLog{};
        std::chrono::steady_clock::time_point _lastTransportWarn{};

        std::unordered_map<STRPMApi::ConnectionID, RemoteState> _remoteStates;
    };
}
