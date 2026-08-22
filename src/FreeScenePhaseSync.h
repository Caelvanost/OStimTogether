#pragma once

#include "PCH.h"
#include "STRPMApi.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/ModThreadControl.h"

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
        OStimModAPI::Thread::IThreadInterface* _threadControl{ nullptr };
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
