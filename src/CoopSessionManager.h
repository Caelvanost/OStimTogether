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

        // Legacy 0.23.x keyboard consent is kept as a no-op compatibility
        // entry point for Input.cpp. Consent is modal again in 0.24.0 via the
        // Skyrim-Souls-safe MessageBox implementation.
        bool HandleConsentKey(std::uint32_t) { return false; }

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

        struct OwnerSession
        {
            std::uint64_t sessionID{ 0 };
            std::int32_t preflightThreadID{ -1 };
            std::int32_t activeThreadID{ -1 };
            std::vector<RE::FormID> actorFormIDs;
            RE::FormID furnitureFormID{ 0 };
            std::string nodeID;
            std::unordered_set<STRPMApi::ConnectionID> participants;
            std::unordered_set<STRPMApi::ConnectionID> accepted;
            bool invitesSent{ false };
            bool restarting{ false };
            bool active{ false };
            bool canceled{ false };
        };

        struct PendingMirrorStart
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t ownerThreadID{ -1 };
            std::string nodeID;
            std::chrono::steady_clock::time_point created{};
        };

        struct MirrorRoute
        {
            STRPMApi::ConnectionID ownerConnectionID{ 0 };
            std::int32_t ownerThreadID{ -1 };
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
        static std::optional<std::uint64_t> SessionID(
            std::string_view payload);
        static std::string SafeLabel(std::string_view value);
        static std::string MirrorKey(
            STRPMApi::ConnectionID ownerConnectionID,
            std::int32_t ownerThreadID);

        bool LoadOStimAPIs();
        bool IsDynamicSTRProxy(RE::Actor* actor) const;

        std::optional<PendingMirrorStart> TakePendingMirrorStart(
            OStim::Thread* thread,
            std::string_view nodeID);

        std::unordered_set<STRPMApi::ConnectionID>
            ResolveRemoteParticipants(OStim::Thread* thread) const;

        void BeginOwnerPreflight(
            OStim::Thread* thread,
            std::unordered_set<STRPMApi::ConnectionID> participants,
            std::string nodeID);
        void StopPreflightAndInvite(std::uint64_t sessionID);
        void QueueOwnerTimeout(std::uint64_t sessionID);
        void HandleInviteResponse(
            STRPMApi::ConnectionID participantConnectionID,
            std::uint64_t sessionID,
            bool accepted);
        void StartApprovedOwnerSession(std::uint64_t sessionID);
        void RetryStartApprovedOwnerSession(std::uint64_t sessionID);
        void CancelOwnerSession(
            std::uint64_t sessionID,
            std::string_view reason);

        void ShowInviteMessageBox(
            STRPMApi::ConnectionID ownerConnectionID,
            std::uint64_t sessionID,
            std::string sender);
        void SendInviteResponse(
            STRPMApi::ConnectionID ownerConnectionID,
            std::uint64_t sessionID,
            bool accepted);

        bool RouteOwnerPayload(std::string_view payload);
        bool HandleControlRequest(
            STRPMApi::ConnectionID senderConnectionID,
            std::string_view payload);
        void NoteIncomingAuthoritative(
            STRPMApi::ConnectionID ownerConnectionID,
            std::string_view payload);

        void HandleThreadStart(OStim::Thread* thread);
        void HandleThreadNode(OStim::Thread* thread);
        void HandleThreadSpeed(OStim::Thread* thread);
        void HandleThreadStop(OStim::Thread* thread);
        void HandleDeferredMirrorSpeed(std::int32_t localThreadID);

        OStim::ThreadInterface* _threads{ nullptr };
        OStimModAPI::Thread::IThreadInterface* _threadControl{ nullptr };
        OStimModAPI::Scene::ISceneInterface* _sceneControl{ nullptr };

        StartListener _startListener;
        NodeListener _nodeListener;
        SpeedListener _speedListener;
        StopListener _stopListener;

        std::atomic_bool _initialized{ false };
        std::atomic_uint64_t _nextSessionID{ 1 };
        mutable std::mutex _mutex;

        std::unordered_map<std::uint64_t, OwnerSession> _ownerSessions;
        std::unordered_map<std::int32_t, std::uint64_t> _pendingOwnerByThread;
        std::unordered_map<std::int32_t, std::uint64_t> _activeOwnerByThread;

        // StartScene() invokes OStim listeners synchronously. This arm tells
        // HandleThreadStart that the next matching local player thread is the
        // accepted replay and must not be preflighted a second time.
        std::optional<std::uint64_t> _approvedReplayArmed;

        std::vector<PendingMirrorStart> _pendingMirrorStarts;
        std::unordered_map<std::int32_t, MirrorRoute> _mirrorRoutes;
        std::unordered_map<std::string, std::int32_t> _mirrorByRemote;
        std::unordered_map<std::int32_t, MirrorSuppression> _mirrorSuppressions;
    };
}
