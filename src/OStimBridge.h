#pragma once

#include "PCH.h"

#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/ModThreadControl.h"
#include "OStimAPI/ModSceneControl.h"
#include "OStimInternalGraphProbe.h"
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

        bool SupportsThreadFurniture() const noexcept
        {
            return _threadInterfaceVersion >= 3;
        }

        // Free-standing scenes need the STR proxy to follow the remote
        // player's real reference/root motion after OStim has performed its
        // initial alignment. Furniture and wall scenes deliberately keep the
        // continuous guard and never call this hook.
        bool DisableSTRProxyPoseGuard(
            std::int32_t threadID,
            std::string_view reason)
        {
            std::scoped_lock lock(_remoteMirrorMutex);
            const bool removed = _strProxyPoseGuardAfter.erase(threadID) > 0;
            _lastSTRProxyPoseGuardLog.erase(threadID);

            if (removed) {
                SKSE::log::info(
                    "OSTNET STR PROXY POSE GUARD disabled thread={} reason={} owner=STR rootMotion=enabled",
                    threadID,
                    reason);
            }

            return removed;
        }

        // A thread that was released while free-standing can later navigate
        // to a wall/furniture node. Restore the established anchored-scene
        // guard in that case rather than leaving the proxy under STR control.
        void EnableSTRProxyPoseGuard(
            std::int32_t threadID,
            std::chrono::milliseconds delay,
            std::string_view reason)
        {
            std::scoped_lock lock(_remoteMirrorMutex);
            _strProxyPoseGuardAfter[threadID] =
                std::chrono::steady_clock::now() + delay;
            _lastSTRProxyPoseGuardLog.erase(threadID);

            SKSE::log::info(
                "OSTNET STR PROXY POSE GUARD rearmed thread={} reason={} delayMs={} owner=OStimTogetherUntilStop",
                threadID,
                reason,
                delay.count());
        }

        // Preflight consent guard. The cooperative listener is intentionally
        // registered before OStimBridge and uses this hook to classify the
        // disposable pre-consent thread as a suppressed mirror before the
        // normal START listener can arm authoritative wall/pose/network work.
        // HandleStop() already forgets mirror-thread bookkeeping, so no extra
        // teardown path is required for the short-lived preflight thread.
        void MarkSuppressedPreflightThread(std::int32_t threadID)
        {
            MarkRemoteMirrorThread(threadID);
        }

        // Sender-side: reconstruct the authoritative world-space
        // center used by OStim from the already aligned local player.
        bool TryComputeSceneCenter(
            OStim::Thread* thread,
            SceneCenter& outCenter,
            bool logDiagnostics = true);

        bool TryComputeActorPose(
            OStim::Thread* thread,
            std::uint32_t actorIndex,
            const SceneCenter& center,
            ActorPose& outPose,
            bool logDiagnostics = true);

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

        bool SetRemoteMirrorSpeed(
            std::string_view sender,
            std::int32_t remoteThreadID,
            std::int32_t speed);

        bool StopRemoteMirror(
            std::string_view sender,
            std::int32_t remoteThreadID);

        void RefreshRemoteMirrors();
        void ResetRemoteState();

    private:
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

        OStimBridge() = default;

        void HandleStart(OStim::Thread* thread);
        void HandleNode(OStim::Thread* thread);
        void HandleSpeed(OStim::Thread* thread);
        void HandleStop(OStim::Thread* thread);

        bool LoadModAPIs();

        static std::string RemoteKey(std::string_view sender, std::int32_t remoteThreadID);

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

        void ScheduleAuthoritativeSelfPoseOnce(std::int32_t localThreadID, std::string_view reason);
        void ScheduleLocalSTRProxyPositionRelease(std::int32_t localThreadID, std::string_view reason);
        void RefreshSTRProxyPoseGuards(std::chrono::steady_clock::time_point now);
        void QueueAuthoritativeWallStart(OStim::Thread* thread);

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
        OStimInternalProbe::GraphLayout _graphLayout{ OStimInternalProbe::GraphLayout::Unsupported };

        OStimModAPI::Thread::IThreadInterface* _threadControl{ nullptr };
        OStimModAPI::Scene::ISceneInterface* _sceneControl{ nullptr };

        StartListener _startListener;
        StopListener _stopListener;
        NodeListener _nodeListener;
        SpeedListener _speedListener;

        std::atomic_bool _creatingRemoteMirror{ false };

        std::mutex _remoteMirrorMutex;
        std::unordered_set<std::int32_t> _remoteMirrorThreads;
        std::unordered_map<std::string, std::int32_t> _remoteToLocal;
        std::unordered_map<std::int32_t, std::string> _localToRemote;
        std::unordered_map<std::int32_t, std::vector<bool>> _localAlignmentMasks;
        std::unordered_map<std::int32_t, std::int32_t> _localSelfIndices;
        std::unordered_map<std::int32_t, SceneCenter> _sceneCenters;
        std::unordered_map<std::int32_t, std::vector<ActorPose>> _authoritativeActorPoses;

        struct PendingWallStart
        {
            std::chrono::steady_clock::time_point due{};
            std::string nodeID;
            SceneCenter earlyCenter{};
        };

        std::unordered_map<std::int32_t, PendingWallStart> _pendingWallStarts;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _strProxyPoseGuardAfter;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _lastSTRProxyPoseGuardLog;
        std::unordered_set<std::int32_t> _pendingInitialAnimationReplay;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _lastAnimationRefresh;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _lastDirectEventLog;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _wallRootProbeUntil;
        std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> _lastWallRootProbeLog;
        std::unordered_map<std::int32_t, std::vector<std::string>> _lastForcedEvents;

        std::uint32_t _mirrorEndSettingsRefCount{ 0 };
        std::vector<EndSettingSnapshot> _mirrorEndSettings;
    };
}
