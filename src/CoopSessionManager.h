#pragma once

#include "PCH.h"
#include "STRPMApi.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/ModThreadControl.h"
#include "OStimAPI/ModSceneControl.h"

namespace OStimTogether
{
    class CoopSessionManager
    {
    public:
        static CoopSessionManager& GetSingleton();

        bool Initialize();
        void Reset();

        bool HandleConsentKey(std::uint32_t keyCode);
        bool InterceptOutgoing(std::string_view payload);
        bool HandleIncoming(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view sender,
            std::string_view payload);

    private:
        CoopSessionManager() = default;

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
        class SpeedListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };
        class StopListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        struct AuthoritativeSession
        {
            std::int32_t threadID{ -1 };
            std::string startPayload;
            std::unordered_set<STRPMApi::ConnectionID> participants;
            std::unordered_set<STRPMApi::ConnectionID> accepted;
            std::string latestNodePayload;
            std::string latestSpeedPayload;
            bool active{ false };
            bool canceled{ false };
            bool invitesSent{ false };
            std::uint64_t generation{ 0 };
        };

        struct PendingInvite
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t remoteThreadID{ -1 };
            std::string sender;
            std::chrono::steady_clock::time_point created{};
        };

        struct PendingMirrorStart
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t remoteThreadID{ -1 };
            std::string nodeID;
            std::chrono::steady_clock::time_point created{};
        };

        struct MirrorRoute
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t remoteThreadID{ -1 };
        };

        struct MirrorSuppression
        {
            std::optional<std::string> expectedNode;
            std::optional<std::int32_t> expectedSpeed;
            bool stop{ false };
        };

        static std::optional<std::string> Field(
            std::string_view payload,
            std::string_view key);
        static std::optional<std::int32_t> ThreadID(
            std::string_view payload);
        static std::string MirrorKey(
            STRPMApi::ConnectionID ownerConnectionID,
            std::int32_t remoteThreadID);
        static std::string SafeLabel(std::string_view value);

        std::unordered_set<STRPMApi::ConnectionID>
            ResolveSceneParticipants(std::string_view startPayload) const;

        bool BeginConsent(std::string_view startPayload);
        void DispatchInvitesDeferred(
            std::int32_t threadID,
            std::uint64_t generation);
        void RegisterIncomingInvite(
            STRPMApi::ConnectionID ownerConnectionID,
            std::int32_t remoteThreadID,
            std::string sender);
        void AnswerOldestInvite(bool accepted);
        void AnswerConsent(
            STRPMApi::ConnectionID ownerConnectionID,
            std::int32_t remoteThreadID,
            bool accepted);
        void HandleConsentResponse(
            STRPMApi::ConnectionID participantConnectionID,
            std::int32_t threadID,
            bool accepted);
        void ActivateAuthoritativeSession(std::int32_t threadID);
        void CancelAuthoritativeSession(
            std::int32_t threadID,
            std::string_view reason,
            bool stopLocalThread);
        void QueueConsentTimeout(
            std::int32_t threadID,
            std::uint64_t generation);
        void QueueIncomingInviteTimeout(
            STRPMApi::ConnectionID ownerConnectionID,
            std::int32_t remoteThreadID);

        bool RouteAuthoritativePayload(std::string_view payload);
        bool HandleControlRequest(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view payload);
        void NoteIncomingAuthoritative(
            STRPMApi::ConnectionID ownerConnectionID,
            std::string_view payload);

        bool LooksLikeRemoteMirror(OStim::Thread* thread) const;
        void HandleThreadStart(OStim::Thread* thread);
        void HandleThreadNode(OStim::Thread* thread);
        void HandleThreadSpeed(OStim::Thread* thread);
        void HandleThreadStop(OStim::Thread* thread);

        bool LoadOStimAPIs();

        OStim::ThreadInterface* _threads{ nullptr };
        OStimModAPI::Thread::IThreadInterface* _threadControl{ nullptr };
        OStimModAPI::Scene::ISceneInterface* _sceneControl{ nullptr };

        StartListener _startListener;
        NodeListener _nodeListener;
        SpeedListener _speedListener;
        StopListener _stopListener;

        std::atomic_bool _initialized{ false };
        std::atomic_uint64_t _generation{ 1 };
        mutable std::mutex _mutex;

        std::unordered_map<std::int32_t, AuthoritativeSession> _authoritative;
        std::vector<PendingInvite> _pendingInvites;
        std::vector<PendingMirrorStart> _pendingMirrorStarts;
        std::unordered_map<std::int32_t, MirrorRoute> _mirrorRoutes;
        std::unordered_map<std::string, std::int32_t> _mirrorByRemote;
        std::unordered_map<std::int32_t, MirrorSuppression> _mirrorSuppressions;
    };
}
