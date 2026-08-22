#include "PCH.h"
#include "FreeSceneAlignmentFix.h"

#include "OStimBridge.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr auto kOwnerInitialConvergenceDelay =
            std::chrono::milliseconds(100);
        constexpr auto kOwnerNodeConvergenceDelay =
            std::chrono::milliseconds(75);
        constexpr auto kMirrorAuthoritativeSettleDelay =
            std::chrono::milliseconds(350);
        constexpr auto kConvergenceInterval =
            std::chrono::milliseconds(50);
        constexpr auto kConvergenceTimeout =
            std::chrono::milliseconds(2500);
        constexpr auto kConvergenceLogInterval =
            std::chrono::milliseconds(250);
        constexpr auto kFurnitureGuardDelay =
            std::chrono::milliseconds(200);
        constexpr auto kWallGuardDelay =
            std::chrono::milliseconds(1100);
        constexpr auto kFreeNodeGuardDelay =
            std::chrono::milliseconds(50);

        constexpr float kConvergedDistance = 4.0F;
        constexpr float kConvergedDistanceSq =
            kConvergedDistance * kConvergedDistance;
        constexpr std::uint32_t kRequiredStableSamples = 3;
        constexpr float kRadiansToDegrees =
            57.2957795130823208768F;

        bool IsLikelySTRRemotePlayerProxy(RE::Actor* actor)
        {
            if (!actor || actor->IsPlayerRef()) {
                return false;
            }

            auto* base = actor->GetActorBase();
            if (!base) {
                return false;
            }

            constexpr RE::FormID kDynamicMask = 0xFF000000;
            return
                (actor->GetFormID() & kDynamicMask) == kDynamicMask &&
                (base->GetFormID() & kDynamicMask) == kDynamicMask;
        }

        const char* CurrentNodeID(OStim::Thread* thread)
        {
            auto* node = thread ? thread->getCurrentNode() : nullptr;
            const auto* nodeID = node ? node->getNodeID() : nullptr;
            return nodeID ? nodeID : "";
        }

        bool IsWallNode(OStim::Thread* thread)
        {
            return std::string_view(CurrentNodeID(thread)).find("wall") !=
                std::string_view::npos;
        }

        bool HasSTRProxy(OStim::Thread* thread)
        {
            if (!thread) {
                return false;
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* threadActor = thread->getActor(i);
                auto* actor = threadActor ?
                    static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
                if (IsLikelySTRRemotePlayerProxy(actor)) {
                    return true;
                }
            }

            return false;
        }

        void StopReferenceTranslation(RE::TESObjectREFR* object)
        {
            if (!object) {
                return;
            }

            using func_t = void(
                RE::BSScript::IVirtualMachine*,
                RE::VMStackID,
                RE::TESObjectREFR*);

            static REL::Relocation<func_t> func{
                RELOCATION_ID(55712, 56243)
            };

            func(nullptr, 0, object);
        }

        void TranslateReferenceTo(
            RE::TESObjectREFR* object,
            const RE::NiPoint3& target,
            float radians)
        {
            if (!object) {
                return;
            }

            using func_t = void(
                RE::BSScript::IVirtualMachine*,
                RE::VMStackID,
                RE::TESObjectREFR*,
                float,
                float,
                float,
                float,
                float,
                float,
                float,
                float);

            static REL::Relocation<func_t> func{
                RELOCATION_ID(55706, 56237)
            };

            func(
                nullptr,
                0,
                object,
                target.x,
                target.y,
                target.z,
                0.0F,
                0.0F,
                radians * kRadiansToDegrees + 1.0F,
                1000000.0F,
                0.0001F);
        }

        void HardSnapReference(
            RE::Actor* actor,
            const ActorPose& pose)
        {
            if (!actor || !pose.IsFinite()) {
                return;
            }

            auto* reference = static_cast<RE::TESObjectREFR*>(actor);
            const RE::NiPoint3 target{ pose.x, pose.y, pose.z };

            StopReferenceTranslation(reference);
            reference->SetPosition(target);
            actor->SetPosition(target, true);
            reference->SetPosition(target);
            reference->data.location = target;
            actor->SetRotationZ(pose.r);
            reference->data.angle.z = pose.r;
            reference->Update3DPosition(true);
        }
    }

    FreeSceneAlignmentFix& FreeSceneAlignmentFix::GetSingleton()
    {
        static FreeSceneAlignmentFix instance;
        return instance;
    }

    void FreeSceneAlignmentFix::StartListener::listen(OStim::Thread* thread)
    {
        FreeSceneAlignmentFix::GetSingleton().HandleStart(thread);
    }

    void FreeSceneAlignmentFix::NodeListener::listen(OStim::Thread* thread)
    {
        FreeSceneAlignmentFix::GetSingleton().HandleNode(thread);
    }

    void FreeSceneAlignmentFix::StopListener::listen(OStim::Thread* thread)
    {
        FreeSceneAlignmentFix::GetSingleton().HandleStop(thread);
    }

    bool FreeSceneAlignmentFix::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::warn(
                "OSTNET FREE SCENE ALIGN unavailable: no SKSE messaging interface");
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        const bool dispatched = messaging->Dispatch(
            OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
            &exchange,
            sizeof(exchange),
            nullptr);

        if (!dispatched || !exchange.interfaceMap) {
            SKSE::log::warn(
                "OSTNET FREE SCENE ALIGN unavailable: OStim interface exchange failed dispatched={} map={}",
                dispatched ? 1 : 0,
                exchange.interfaceMap ? 1 : 0);
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));

        if (!_threads) {
            SKSE::log::warn(
                "OSTNET FREE SCENE ALIGN unavailable: Threads interface missing");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN READY threadsVersion={} ownerMode=convergence mirrorMode=authoritative-start-settle mirrorNodeRealign=0 threshold={} stableSamples={} timeoutMs={}",
            _threads->getVersion(),
            kConvergedDistance,
            kRequiredStableSamples,
            kConvergenceTimeout.count());
        return true;
    }

    void FreeSceneAlignmentFix::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();

        if (!bridge.SupportsThreadFurniture() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread) ||
            !HasSTRProxy(thread)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        const bool mirror = bridge.IsRemoteMirrorForAlignment(threadID);
        const auto delay = mirror ?
            kMirrorAuthoritativeSettleDelay :
            kOwnerInitialConvergenceDelay;

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN ARM thread={} node={} reason=START mirror={} delayMs={} action={}",
            threadID,
            CurrentNodeID(thread),
            mirror ? 1 : 0,
            delay.count(),
            mirror ? "settle-authoritative-pose-then-release" : "wait-for-owner-proxy-convergence");

        ArmConvergence(
            threadID,
            delay,
            mirror ? "START-MIRROR" : "START-OWNER",
            false);
    }

    void FreeSceneAlignmentFix::HandleNode(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        if (!bridge.SupportsThreadFurniture()) {
            return;
        }

        const auto threadID = thread->getThreadID();
        const bool hasFurniture = thread->getFurnitureObject() != nullptr;
        const bool wall = IsWallNode(thread);

        if (hasFurniture || wall) {
            bool hadFreeState = false;
            {
                std::scoped_lock lock(_stateMutex);
                hadFreeState = _states.erase(threadID) > 0;
            }

            if (hadFreeState) {
                bridge.EnableSTRProxyPoseGuard(
                    threadID,
                    wall ? kWallGuardDelay : kFurnitureGuardDelay,
                    wall ? "free-to-wall" : "free-to-furniture");
            }
            return;
        }

        if (!HasSTRProxy(thread)) {
            std::scoped_lock lock(_stateMutex);
            _states.erase(threadID);
            return;
        }

        const bool mirror = bridge.IsRemoteMirrorForAlignment(threadID);
        bool wasReleased = false;
        bool hasState = false;
        {
            std::scoped_lock lock(_stateMutex);
            const auto it = _states.find(threadID);
            hasState = it != _states.end();
            wasReleased = hasState && it->second.released;
        }

        if (mirror && wasReleased) {
            // The mirror already shares the owner's original thread center.
            // OStim ChangeNode() applies the same role animation locally.
            // Recomputing a new center from the mirror's animated local SELF
            // created a second target 20-40 units away from the authoritative
            // pose in 0.27.2. Never rearm position ownership for ordinary
            // free-standing mirror node changes.
            bridge.DisableSTRProxyPoseGuard(
                threadID,
                "free-mirror-node-native");

            SKSE::log::info(
                "OSTNET FREE SCENE MIRROR NODE thread={} node={} action=no-position-realign ownerCenter=preserved",
                threadID,
                CurrentNodeID(thread));
            return;
        }

        if (mirror) {
            // Initial mirror NODE is emitted immediately before START. Arm a
            // settle task here as a fallback; START will supersede it with a
            // newer generation. The same path also handles wall/furniture ->
            // free transitions, where the anchored pose guard needs a short
            // settle before being released again.
            SKSE::log::info(
                "OSTNET FREE SCENE ALIGN ARM thread={} node={} reason=NODE-MIRROR delayMs={} previousReleased={} action=settle-authoritative-pose-then-release",
                threadID,
                CurrentNodeID(thread),
                kMirrorAuthoritativeSettleDelay.count(),
                wasReleased ? 1 : 0);

            ArmConvergence(
                threadID,
                kMirrorAuthoritativeSettleDelay,
                "NODE-MIRROR",
                false);
            return;
        }

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN ARM thread={} node={} reason=NODE-OWNER delayMs={} previousReleased={} action=wait-for-owner-proxy-convergence",
            threadID,
            CurrentNodeID(thread),
            kOwnerNodeConvergenceDelay.count(),
            wasReleased ? 1 : 0);

        ArmConvergence(
            threadID,
            kOwnerNodeConvergenceDelay,
            "NODE-OWNER",
            wasReleased);
    }

    void FreeSceneAlignmentFix::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        std::scoped_lock lock(_stateMutex);
        _states.erase(thread->getThreadID());
    }

    void FreeSceneAlignmentFix::ArmConvergence(
        std::int32_t threadID,
        std::chrono::milliseconds delay,
        std::string reason,
        bool rearmPoseGuard)
    {
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(_stateMutex);
            auto& state = _states[threadID];
            ++state.generation;
            state.stableSamples = 0;
            state.started = std::chrono::steady_clock::now();
            state.lastLog = {};
            state.released = false;
            generation = state.generation;
        }

        if (rearmPoseGuard) {
            OStimBridge::GetSingleton().EnableSTRProxyPoseGuard(
                threadID,
                kFreeNodeGuardDelay,
                "free-owner-node-convergence");
        }

        ScheduleConvergenceCheck(
            threadID,
            generation,
            delay,
            std::move(reason));
    }

    void FreeSceneAlignmentFix::ScheduleConvergenceCheck(
        std::int32_t threadID,
        std::uint64_t generation,
        std::chrono::milliseconds delay,
        std::string reason)
    {
        std::thread([
            this,
            threadID,
            generation,
            delay,
            reason = std::move(reason)]() {
            std::this_thread::sleep_for(delay);

            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }

            tasks->AddTask([
                this,
                threadID,
                generation,
                reason]() {
                CheckConvergence(threadID, generation, reason);
            });
        }).detach();
    }

    void FreeSceneAlignmentFix::CheckConvergence(
        std::int32_t threadID,
        std::uint64_t generation,
        std::string_view reason)
    {
        std::chrono::steady_clock::time_point started{};
        std::uint32_t previousStableSamples = 0;
        {
            std::scoped_lock lock(_stateMutex);
            const auto it = _states.find(threadID);
            if (it == _states.end() ||
                it->second.generation != generation ||
                it->second.released) {
                return;
            }
            started = it->second.started;
            previousStableSamples = it->second.stableSamples;
        }

        if (!_threads) {
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread ||
            !thread->isPlayerThread() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread) ||
            !HasSTRProxy(thread)) {
            std::scoped_lock lock(_stateMutex);
            _states.erase(threadID);
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        const bool mirror = bridge.IsRemoteMirrorForAlignment(threadID);

        if (mirror) {
            // The authoritative pose guard in OStimBridge uses the START poses
            // received from the owner and has been active since ~200 ms after
            // thread creation. Do NOT derive another center from the mirror's
            // animated local PlayerCharacter here. 0.27.2 proved that those
            // two coordinate frames differ by the animation/root displacement
            // and fight each other continuously.
            SKSE::log::info(
                "OSTNET FREE SCENE MIRROR SETTLED thread={} node={} reason={} action=release-authoritative-start-guard localCenterRecompute=0",
                threadID,
                CurrentNodeID(thread),
                reason);

            ReleaseFreeSceneProxy(
                threadID,
                generation,
                reason,
                false,
                0.0F);
            return;
        }

        SceneCenter center{};
        const bool haveCenter =
            bridge.TryComputeSceneCenter(thread, center, false);

        std::uint32_t proxyCount = 0;
        std::uint32_t translated = 0;
        float maxDistanceSq = 0.0F;

        if (haveCenter) {
            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* threadActor = thread->getActor(i);
                auto* actor = threadActor ?
                    static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;

                if (!IsLikelySTRRemotePlayerProxy(actor)) {
                    continue;
                }

                ActorPose pose{};
                if (!bridge.TryComputeActorPose(
                        thread,
                        i,
                        center,
                        pose,
                        false) ||
                    !pose.IsFinite()) {
                    continue;
                }

                ++proxyCount;

                const RE::NiPoint3 target{ pose.x, pose.y, pose.z };
                const auto current = actor->GetPosition();
                const auto distanceSq = current.GetSquaredDistance(target);
                maxDistanceSq = std::max(maxDistanceSq, distanceSq);

                if (distanceSq > kConvergedDistanceSq) {
                    StopReferenceTranslation(actor);
                    TranslateReferenceTo(actor, target, pose.r);
                    ++translated;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
        const bool timedOut = elapsed >= kConvergenceTimeout;
        const bool sampleConverged =
            haveCenter && proxyCount > 0 &&
            maxDistanceSq <= kConvergedDistanceSq;

        const std::uint32_t stableSamples = sampleConverged ?
            previousStableSamples + 1 : 0;

        bool writeLog = false;
        {
            std::scoped_lock lock(_stateMutex);
            const auto it = _states.find(threadID);
            if (it == _states.end() ||
                it->second.generation != generation ||
                it->second.released) {
                return;
            }

            it->second.stableSamples = stableSamples;
            if (it->second.lastLog.time_since_epoch().count() == 0 ||
                now - it->second.lastLog >= kConvergenceLogInterval ||
                stableSamples > 0 ||
                timedOut) {
                it->second.lastLog = now;
                writeLog = true;
            }
        }

        if (writeLog) {
            SKSE::log::info(
                "OSTNET FREE SCENE OWNER CONVERGENCE thread={} node={} reason={} elapsedMs={} centerValid={} proxies={} translated={} maxDist={:.3f} stable={}/{} timeout={}",
                threadID,
                CurrentNodeID(thread),
                reason,
                elapsed.count(),
                haveCenter ? 1 : 0,
                proxyCount,
                translated,
                std::sqrt(maxDistanceSq),
                stableSamples,
                kRequiredStableSamples,
                timedOut ? 1 : 0);
        }

        if (stableSamples >= kRequiredStableSamples || timedOut) {
            ReleaseFreeSceneProxy(
                threadID,
                generation,
                reason,
                timedOut,
                maxDistanceSq);
            return;
        }

        ScheduleConvergenceCheck(
            threadID,
            generation,
            kConvergenceInterval,
            std::string(reason));
    }

    void FreeSceneAlignmentFix::ReleaseFreeSceneProxy(
        std::int32_t threadID,
        std::uint64_t generation,
        std::string_view reason,
        bool timeout,
        float maxDistanceSq)
    {
        if (!_threads) {
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread ||
            !thread->isPlayerThread() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread)) {
            std::scoped_lock lock(_stateMutex);
            _states.erase(threadID);
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        const bool mirror = bridge.IsRemoteMirrorForAlignment(threadID);

        if (timeout && !mirror) {
            SceneCenter center{};
            if (bridge.TryComputeSceneCenter(thread, center, false)) {
                for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                    auto* threadActor = thread->getActor(i);
                    auto* actor = threadActor ?
                        static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
                    if (!IsLikelySTRRemotePlayerProxy(actor)) {
                        continue;
                    }

                    ActorPose pose{};
                    if (bridge.TryComputeActorPose(
                            thread,
                            i,
                            center,
                            pose,
                            false) &&
                        pose.IsFinite()) {
                        HardSnapReference(actor, pose);
                    }
                }
            }
        }

        bridge.DisableSTRProxyPoseGuard(
            threadID,
            mirror ?
                "free-mirror-authoritative-settled" :
                (timeout ?
                    "free-owner-convergence-timeout" :
                    "free-owner-converged"));

        std::uint32_t released = 0;
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;

            if (!IsLikelySTRRemotePlayerProxy(actor)) {
                continue;
            }

            StopReferenceTranslation(actor);
            ++released;
        }

        {
            std::scoped_lock lock(_stateMutex);
            const auto it = _states.find(threadID);
            if (it == _states.end() || it->second.generation != generation) {
                return;
            }
            it->second.released = released > 0;
            it->second.stableSamples = kRequiredStableSamples;
        }

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN RELEASE thread={} node={} reason={} mirror={} proxies={} timeout={} finalMaxDist={:.3f} localCenterRecompute={} action=release-to-str-animation continuousCorrections=0",
            threadID,
            CurrentNodeID(thread),
            reason,
            mirror ? 1 : 0,
            released,
            timeout ? 1 : 0,
            std::sqrt(maxDistanceSq),
            mirror ? 0 : 1);
    }
}
