#pragma once

#include "PCH.h"
#include "STRPMApi.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/ModThreadControl.h"

// v0.30.4 proved that OStim's own SetSpeed() is the correct animation replay
// primitive. v0.30.5 also restores the native OStim alignment step immediately
// before that replay. OStim Thread::ChangeNode() normally performs:
//
//   alignActors() -> SetSpeed() -> playAnimation()
//
// The phase barrier previously replayed only SetSpeed(), after STR had already
// had time to update remote proxy references. PhaseThreadControl reproduces the
// missing public-API alignment stage by re-submitting each actor's CURRENT
// OStim alignment through GetActorAlignment()/SetActorAlignment() before the
// SetSpeed task is queued. No direct reference, 3D-root or skeleton write is
// performed by OStim Together.
#define OSTIM_TOGETHER_FORCE_NATIVE_PHASE_REPLAY 1

namespace OStimTogether
{
    class FreeScenePhaseSync
    {
    public:
        static FreeScenePhaseSync& GetSingleton();

        bool Initialize();
        bool StartTransport();
        void StopTransport();
        void Reset();

    private:
        FreeScenePhaseSync() = default;
        ~FreeScenePhaseSync();

        FreeScenePhaseSync(const FreeScenePhaseSync&) = delete;
        FreeScenePhaseSync& operator=(const FreeScenePhaseSync&) = delete;

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

        struct TimingSample
        {
            std::int64_t remoteMinusOwnerUs{ 0 };
            std::int64_t roundTripUs{ 0 };
        };

        struct OwnerPhase
        {
            std::int32_t threadID{ -1 };
            std::uint64_t token{ 0 };
            std::string nodeID;
            std::string reason;
            std::unordered_set<STRPMApi::ConnectionID> expected;
            std::unordered_set<STRPMApi::ConnectionID> ready;
            std::unordered_map<STRPMApi::ConnectionID, TimingSample> timing;
            std::int64_t prepOwnerUs{ 0 };
            bool committed{ false };
        };

        struct RemotePrep
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t ownerThreadID{ -1 };
            std::uint64_t token{ 0 };
            std::string nodeID;
            std::string reason;
            std::int64_t prepOwnerUs{ 0 };
            std::int64_t prepRemoteReceiveUs{ 0 };
        };

        // Narrow adapter used only by FreeScenePhaseSync. It deliberately
        // intercepts SetSpeed() so the synchronized replay uses the same
        // align-then-play order as OStim's native ChangeNode(). All other
        // methods used by this component are direct pass-throughs.
        class PhaseThreadControl
        {
        public:
            PhaseThreadControl() = default;

            PhaseThreadControl& operator=(
                OStimModAPI::Thread::IThreadInterface* value) noexcept
            {
                _raw = value;
                return *this;
            }

            explicit operator bool() const noexcept
            {
                return _raw != nullptr;
            }

            bool operator!() const noexcept
            {
                return _raw == nullptr;
            }

            friend bool operator!=(
                const PhaseThreadControl& value,
                std::nullptr_t) noexcept
            {
                return value._raw != nullptr;
            }

            PhaseThreadControl* operator->() noexcept
            {
                return this;
            }

            const PhaseThreadControl* operator->() const noexcept
            {
                return this;
            }

            std::uint32_t GetPlayerThreadID() noexcept
            {
                return _raw ? _raw->GetPlayerThreadID() : 0;
            }

            bool IsThreadValid(std::uint32_t threadID) noexcept
            {
                return _raw && _raw->IsThreadValid(threadID);
            }

            std::int32_t GetCurrentSpeed(std::uint32_t threadID) noexcept
            {
                return _raw ? _raw->GetCurrentSpeed(threadID) : 0;
            }

            OStimModAPI::Thread::APIResult SetSpeed(
                std::uint32_t threadID,
                std::int32_t speed) noexcept
            {
                if (!_raw) {
                    return OStimModAPI::Thread::APIResult::Invalid;
                }

                const auto actorCount = _raw->GetActorCount(threadID);
                std::uint32_t aligned = 0;
                std::uint32_t failed = 0;

                for (std::uint32_t i = 0; i < actorCount; ++i) {
                    OStimModAPI::Thread::ActorAlignmentData alignment{};
                    if (!_raw->GetActorAlignment(threadID, i, &alignment)) {
                        ++failed;
                        continue;
                    }

                    const auto result =
                        _raw->SetActorAlignment(threadID, i, &alignment);
                    if (result == OStimModAPI::Thread::APIResult::OK) {
                        ++aligned;
                    } else {
                        ++failed;
                    }
                }

                SKSE::log::info(
                    "OSTNET PHASE NATIVE ALIGN thread={} actors={}/{} failed={} action=get-current-alignment-then-set-speed directPositionWrites=0 skeletonWrites=0",
                    threadID,
                    aligned,
                    actorCount,
                    failed);

                // OStim ModAPI queues all SetActorAlignment tasks and this
                // SetSpeed task on the same SKSE task queue in call order.
                // Therefore the current OStim alignment is re-applied before
                // Thread::SetSpeed() restarts the paired role animations.
                return _raw->SetSpeed(threadID, speed);
            }

        private:
            OStimModAPI::Thread::IThreadInterface* _raw{ nullptr };
        };

        bool LoadOStimAPIs();
        bool IsFreeStandingThread(OStim::Thread* thread) const;
        bool IsDynamicSTRProxy(RE::Actor* actor) const;
        bool ThreadContainsActor(OStim::Thread* thread, RE::Actor* actor) const;
        std::unordered_set<STRPMApi::ConnectionID> ResolveParticipants(OStim::Thread* thread) const;

        void HandleStart(OStim::Thread* thread);
        void HandleNode(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);
        void BeginOwnerPhase(OStim::Thread* thread, std::string_view reason);
        void MaybeReadyRemotePhase(OStim::Thread* thread, std::chrono::milliseconds delay);
        void QueueReady(std::int32_t localThreadID, RemotePrep prep, std::chrono::milliseconds delay);
        void HandleReady(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view payload,
            std::int64_t receiveOwnerUs);
        void CommitOwnerPhase();
        void QueueReplay(
            std::int32_t localThreadID,
            std::uint64_t token,
            std::string nodeID,
            std::int32_t speed,
            std::int64_t executeLocalUs,
            bool mirror);
        void ReplayNow(
            std::int32_t localThreadID,
            std::uint64_t token,
            std::string_view nodeID,
            std::int32_t speed,
            std::int64_t executeLocalUs,
            bool mirror);
        void QueueProxyTranslationRelease(std::int32_t localThreadID, std::uint64_t token);

        static std::optional<std::string> Field(std::string_view payload, std::string_view key);
        static std::optional<std::int32_t> ParseInt(std::string_view payload, std::string_view key);
        static std::optional<std::int64_t> ParseInt64(std::string_view payload, std::string_view key);
        static std::optional<std::uint64_t> ParseUInt64(std::string_view payload, std::string_view key);

        static void __cdecl OnMessage(const STRPMApi::Message* message, void* userData);
        void HandleMessage(const STRPMApi::Message& message);
        void HandleMessageGameThread(
            STRPMApi::ConnectionID senderConnectionID,
            std::string payload,
            std::int64_t receiveLocalUs);
        bool SendTo(STRPMApi::ConnectionID connectionID, std::string_view payload);

        OStim::ThreadInterface* _threads{ nullptr };
        PhaseThreadControl _threadControl{};
        std::uint32_t _threadInterfaceVersion{ 0 };

        StartListener _startListener;
        NodeListener _nodeListener;
        StopListener _stopListener;
        std::atomic_bool _initialized{ false };

        const STRPMApi::Interface* _api{ nullptr };
        STRPMApi::ListenerHandle _listener{};
        std::atomic_bool _transportRunning{ false };

        std::uint64_t _nextToken{ 1 };
        std::optional<OwnerPhase> _ownerPhase;
        std::vector<RemotePrep> _remotePreps;
        std::unordered_map<std::string, std::uint64_t> _lastReadyToken;
        std::unordered_set<std::int32_t> _startedThreads;
    };
}