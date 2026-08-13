#pragma once

#include "PCH.h"

#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/ModThreadControl.h"
#include "OStimAPI/ModSceneControl.h"
#include "SceneCenter.h"

namespace OStimTogether
{
    class OStimBridge final
    {
    public:
        static OStimBridge& GetSingleton();

        bool Initialize();

        bool IsConnected() const noexcept
        {
            return _threads != nullptr;
        }

        // Sender-side: reconstruct the authoritative world-space
        // center used by OStim from the already aligned local player.
        bool TryComputeSceneCenter(
            OStim::Thread* thread,
            SceneCenter& outCenter);

        bool TryComputeActorPose(
            OStim::Thread* thread,
            std::uint32_t actorIndex,
            const SceneCenter& center,
            ActorPose& outPose);

        std::int32_t StartRemoteMirror(
            std::string_view sender,
            std::int32_t remoteThreadID,
            const std::vector<RE::Actor*>& actors,
            const std::vector<bool>& localAlignmentMask,
            std::int32_t localSelfIndex,
            const SceneCenter& authoritativeCenter,
            const std::vector<ActorPose>& authoritativePoses,
            RE::TESObjectREFR* localFurniture,
            std::string_view nodeID);

        bool NavigateRemoteMirror(
            std::string_view sender,
            std::int32_t remoteThreadID,
            std::string_view nodeID,
            const std::vector<ActorPose>& authoritativePoses);

        bool StopRemoteMirror(
            std::string_view sender,
            std::int32_t remoteThreadID);

        // Reassert OStim alignment on mirror threads.
        // Called on the Skyrim game thread by VisualKeepAlive.
        void RefreshRemoteMirrors();

        // Called before loading another save.
        // Restores any OStim settings temporarily overridden by mirrors
        // and forgets stale mirror mappings.
        void ResetRemoteState();

    private:
        class StartListener final :
            public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        class StopListener final :
            public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        class NodeListener final :
            public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        OStimBridge() = default;

        void HandleStart(OStim::Thread* thread);
        void HandleNode(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);

        bool LoadModAPIs();

        static std::string RemoteKey(
            std::string_view sender,
            std::int32_t remoteThreadID);

        bool IsRemoteMirrorThread(std::int32_t threadID);
        void MarkRemoteMirrorThread(std::int32_t threadID);

        void RegisterRemoteMapping(
            std::string_view sender,
            std::int32_t remoteThreadID,
            std::int32_t localThreadID,
            const std::vector<bool>& localAlignmentMask,
            std::int32_t localSelfIndex,
            const SceneCenter& authoritativeCenter,
            const std::vector<ActorPose>& authoritativePoses);

        std::optional<std::int32_t> FindRemoteMirror(
            std::string_view sender,
            std::int32_t remoteThreadID);

        void ForgetRemoteMirrorThread(std::int32_t threadID);

        void AcquireMirrorEndSettings();
        void ReleaseMirrorEndSettings();
        void RestoreMirrorEndSettingsNow();

        // Apply the authoritative transform to the real local
        // PlayerCharacter once, after OStim's own ChangeNode/alignment tasks.
        // Continuous 25 ms forcing causes visible root-motion jitter.
        void ScheduleAuthoritativeSelfPoseOnce(
            std::int32_t localThreadID,
            std::string_view reason);

        void QueueAuthoritativeWallStart(
            OStim::Thread* thread);

        bool ApplySceneAnchorToLocalSelf(
            OStim::Thread* thread,
            std::uint32_t actorIndex,
            const SceneCenter& center);

        static float NormalizeRadians(float value);

        struct EndSettingSnapshot
        {
            std::uint32_t formID{ 0 };
            RE::TESGlobal* global{ nullptr };
            float originalValue{ 0.0F };
        };

        OStim::ThreadInterface* _threads{ nullptr };
        std::uint32_t _threadInterfaceVersion{ 0 };

        OStimModAPI::Thread::IThreadInterface*
            _threadControl{ nullptr };

        OStimModAPI::Scene::ISceneInterface*
            _sceneControl{ nullptr };

        StartListener _startListener;
        StopListener _stopListener;
        NodeListener _nodeListener;

        std::atomic_bool _creatingRemoteMirror{ false };

        std::mutex _remoteMirrorMutex;
        std::unordered_set<std::int32_t> _remoteMirrorThreads;
        std::unordered_map<std::string, std::int32_t> _remoteToLocal;
        std::unordered_map<std::int32_t, std::string> _localToRemote;

        // Per mirror thread, one entry per OStim actor index.
        //
        // true  = OStim Together reasserts OStim alignment every 25 ms.
        // false = continuous alignment is delegated elsewhere.
        //
        // Sender role=player proxy -> STR.
        // NPCs -> OStim Together alignment keepalive.
        // Real local PlayerCharacter SELF -> authoritative one-shot pose
        // after START/NODE; never continuous position forcing.
        std::unordered_map<std::int32_t, std::vector<bool>>
            _localAlignmentMasks;

        // If the real local PlayerCharacter is a participant, its
        // world-space placement is derived from the authoritative scene
        // center rather than OStim's mirror-local center.
        std::unordered_map<std::int32_t, std::int32_t>
            _localSelfIndices;

        std::unordered_map<std::int32_t, SceneCenter>
            _sceneCenters;

        std::unordered_map<std::int32_t, std::vector<ActorPose>>
            _authoritativeActorPoses;

        struct PendingWallStart
        {
            std::chrono::steady_clock::time_point due{};
            std::string nodeID;
            SceneCenter earlyCenter{};
        };

        // Authoritative Wall scenes need longer than the generic two task
        // hops before OStim has settled the reference transform against the
        // wall. Delay only their network START; ordinary/furniture scenes
        // keep the existing immediate post-align path.
        std::unordered_map<std::int32_t, PendingWallStart>
            _pendingWallStarts;

        // The initial OStim animation can start before the mirror's local
        // PlayerCharacter visual root has settled. Replay the CURRENT speed
        // once after the mirror thread is fully alive; OStim SetSpeed()
        // replays the current animation without changing nodes.
        std::unordered_set<std::int32_t>
            _pendingInitialAnimationReplay;

        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
            _lastAnimationRefresh;

        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
            _lastDirectEventLog;

        // v0.18.16 diagnostic: after a mirrored Wall starts, sample the real
        // local PlayerCharacter reference and rendered NiNode root for a short
        // window. This distinguishes TESObjectREFR stability from visual-root
        // drift without applying any additional position correction.
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
            _wallRootProbeUntil;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point>
            _lastWallRootProbeLog;

        // Last Papyrus-forced animation event dispatched for each actor
        // position in a mirror thread.
        std::unordered_map<std::int32_t, std::vector<std::string>>
            _lastForcedEvents;

        std::uint32_t _mirrorEndSettingsRefCount{ 0 };
        std::vector<EndSettingSnapshot> _mirrorEndSettings;
    };
}
