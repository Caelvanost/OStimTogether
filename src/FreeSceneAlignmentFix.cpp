#include "PCH.h"
#include "FreeSceneAlignmentFix.h"

#include "OStimBridge.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr auto kInitialFreeSceneReleaseDelay =
            std::chrono::milliseconds(250);
        constexpr auto kNodeFreeSceneReleaseDelay =
            std::chrono::milliseconds(75);
        constexpr auto kFurnitureGuardDelay =
            std::chrono::milliseconds(200);
        constexpr auto kWallGuardDelay =
            std::chrono::milliseconds(1100);
        constexpr auto kReferenceGuardInterval =
            std::chrono::milliseconds(25);
        constexpr auto kReferenceGuardLogInterval =
            std::chrono::milliseconds(500);

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

            // Same native ObjectReference.StopTranslation relocation used by
            // OStim itself and by OStimBridge's existing proxy release path.
            using func_t = void(
                RE::BSScript::IVirtualMachine*,
                RE::VMStackID,
                RE::TESObjectREFR*);

            static REL::Relocation<func_t> func{
                RELOCATION_ID(55712, 56243)
            };

            func(nullptr, 0, object);
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
            "OSTNET FREE SCENE ALIGN READY threadsVersion={} mode=noFurniture-reference-lock-root-free",
            _threads->getVersion());
        return true;
    }

    void FreeSceneAlignmentFix::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();

        // Exact furniture detection is available only through Threads ABI v3.
        // On OStim 7.4c retain the established pose-guard behavior rather than
        // guessing whether a thread is really furniture-free.
        if (!bridge.SupportsThreadFurniture() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread) ||
            !HasSTRProxy(thread)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        {
            std::scoped_lock lock(_stateMutex);
            _pendingRelease.insert(threadID);
            _releasedThreads.erase(threadID);
        }
        StopReferenceGuard(threadID);

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN armed thread={} node={} delayMs={} furniture=none action=switch-to-reference-only-after-initial-align",
            threadID,
            CurrentNodeID(thread),
            kInitialFreeSceneReleaseDelay.count());

        ScheduleFreeSceneRelease(
            threadID,
            kInitialFreeSceneReleaseDelay,
            "start-free-scene");
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
            bool wasReleased = false;
            {
                std::scoped_lock lock(_stateMutex);
                _pendingRelease.erase(threadID);
                wasReleased = _releasedThreads.erase(threadID) > 0;
            }

            StopReferenceGuard(threadID);

            if (wasReleased) {
                bridge.EnableSTRProxyPoseGuard(
                    threadID,
                    wall ? kWallGuardDelay : kFurnitureGuardDelay,
                    wall ? "free-to-wall" : "free-to-furniture");
            }
            return;
        }

        if (!HasSTRProxy(thread)) {
            StopReferenceGuard(threadID);
            return;
        }

        bool alreadyReleased = false;
        bool schedule = false;
        {
            std::scoped_lock lock(_stateMutex);
            alreadyReleased = _releasedThreads.contains(threadID);
            if (!_pendingRelease.contains(threadID)) {
                _pendingRelease.insert(threadID);
                schedule = true;
            }
        }

        // OStim queues a fresh lockAtPosition/TranslateTo on every node
        // change. Once the scene is already in reference-only mode, stop that
        // translation shortly after the node settles and keep only the logical
        // TESObjectREFR origin locked. Initial START keeps the longer settle.
        if (schedule) {
            const auto delay = alreadyReleased ?
                kNodeFreeSceneReleaseDelay :
                kInitialFreeSceneReleaseDelay;

            SKSE::log::info(
                "OSTNET FREE SCENE ALIGN node thread={} node={} delayMs={} alreadyReleased={} action=switch-to-reference-only-after-node-align",
                threadID,
                CurrentNodeID(thread),
                delay.count(),
                alreadyReleased ? 1 : 0);

            ScheduleFreeSceneRelease(
                threadID,
                delay,
                alreadyReleased ?
                    "node-free-scene" :
                    "enter-free-scene");
        }
    }

    void FreeSceneAlignmentFix::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        StopReferenceGuard(threadID);

        std::scoped_lock lock(_stateMutex);
        _pendingRelease.erase(threadID);
        _releasedThreads.erase(threadID);
        _lastReferenceGuardLog.erase(threadID);
    }

    void FreeSceneAlignmentFix::ScheduleFreeSceneRelease(
        std::int32_t threadID,
        std::chrono::milliseconds delay,
        std::string reason)
    {
        std::thread([this, threadID, delay, reason = std::move(reason)]() {
            std::this_thread::sleep_for(delay);

            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                std::scoped_lock lock(_stateMutex);
                _pendingRelease.erase(threadID);
                return;
            }

            tasks->AddTask([this, threadID, reason]() {
                ReleaseFreeSceneProxy(threadID, reason);
            });
        }).detach();
    }

    void FreeSceneAlignmentFix::ReleaseFreeSceneProxy(
        std::int32_t threadID,
        std::string_view reason)
    {
        const auto clearPending = [this, threadID]() {
            std::scoped_lock lock(_stateMutex);
            _pendingRelease.erase(threadID);
        };

        if (!_threads) {
            clearPending();
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread || !thread->isPlayerThread()) {
            StopReferenceGuard(threadID);
            std::scoped_lock lock(_stateMutex);
            _pendingRelease.erase(threadID);
            _releasedThreads.erase(threadID);
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        if (!bridge.SupportsThreadFurniture() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread)) {
            clearPending();
            return;
        }

        // The full OStimTogether guard rewrites the loaded 3D root as well as
        // the reference. That is useful for furniture/wall scenes, but it
        // destroys actor-relative animation root motion in free scenes.
        // Disable only that full guard, stop OStim's completed TranslateTo,
        // then keep the logical reference origin aligned without touching 3D.
        bridge.DisableSTRProxyPoseGuard(
            threadID,
            "free-scene-reference-only");

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
            _pendingRelease.erase(threadID);
            if (released > 0) {
                _releasedThreads.insert(threadID);
            } else {
                _releasedThreads.erase(threadID);
            }
        }

        if (released > 0) {
            StartReferenceGuard(threadID);
            SKSE::log::info(
                "OSTNET FREE SCENE PROXY RELEASE thread={} node={} proxies={} reason={} action=stop-translation referenceOwner=OStimTogether rootOwner=animation",
                threadID,
                CurrentNodeID(thread),
                released,
                reason);
        } else {
            StopReferenceGuard(threadID);
        }
    }

    void FreeSceneAlignmentFix::StartReferenceGuard(std::int32_t threadID)
    {
        bool startWorker = false;
        {
            std::scoped_lock lock(_stateMutex);
            _referenceGuardThreads.insert(threadID);
            if (!_referenceGuardWorkers.contains(threadID)) {
                _referenceGuardWorkers.insert(threadID);
                startWorker = true;
            }
        }

        if (!startWorker) {
            return;
        }

        std::thread([this, threadID]() {
            for (;;) {
                std::this_thread::sleep_for(kReferenceGuardInterval);

                {
                    std::scoped_lock lock(_stateMutex);
                    if (!_referenceGuardThreads.contains(threadID)) {
                        _referenceGuardWorkers.erase(threadID);
                        return;
                    }
                }

                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    continue;
                }

                tasks->AddTask([this, threadID]() {
                    ApplyReferenceGuard(threadID);
                });
            }
        }).detach();
    }

    void FreeSceneAlignmentFix::StopReferenceGuard(std::int32_t threadID)
    {
        std::scoped_lock lock(_stateMutex);
        _referenceGuardThreads.erase(threadID);
        _lastReferenceGuardLog.erase(threadID);
    }

    void FreeSceneAlignmentFix::ApplyReferenceGuard(std::int32_t threadID)
    {
        {
            std::scoped_lock lock(_stateMutex);
            if (!_referenceGuardThreads.contains(threadID)) {
                return;
            }
        }

        if (!_threads) {
            StopReferenceGuard(threadID);
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread ||
            !thread->isPlayerThread() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread) ||
            !HasSTRProxy(thread)) {
            StopReferenceGuard(threadID);
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        SceneCenter center{};
        if (!bridge.TryComputeSceneCenter(thread, center, false)) {
            return;
        }

        std::uint32_t guarded = 0;
        RE::FormID sampleActorID = 0;
        RE::NiPoint3 sampleRefBefore{};
        RE::NiPoint3 sampleRefAfter{};
        RE::NiPoint3 sampleTarget{};
        RE::NiPoint3 sampleRoot{};
        bool sampleHasRoot = false;
        float maxRefBeforeDistSq = 0.0F;
        float maxRefAfterDistSq = 0.0F;
        float maxRootTargetDistSq = 0.0F;

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

            auto* reference = static_cast<RE::TESObjectREFR*>(actor);
            const RE::NiPoint3 target{ pose.x, pose.y, pose.z };
            const auto refBefore = reference->GetPosition();

            RE::NiPoint3 root{};
            bool hasRoot = false;
            if (auto* root3D = actor->Get3D()) {
                root = root3D->world.translate;
                hasRoot = true;
            }

            // Critical distinction from OStimBridge::ForceSTRProxyPose():
            // write only the logical TESObjectREFR transform. Do NOT call
            // SetPosition(), Update3DPosition(), TranslateTo(), or touch the
            // NiAVObject root. The animation is therefore free to keep its
            // actor-relative root offset while STR cannot move the scene's
            // logical origin away from OStim's expected pose.
            reference->data.location = target;
            reference->data.angle.z = pose.r;

            const auto refAfter = reference->GetPosition();
            const auto beforeDistSq = refBefore.GetSquaredDistance(target);
            const auto afterDistSq = refAfter.GetSquaredDistance(target);
            const auto rootDistSq = hasRoot ?
                root.GetSquaredDistance(target) : 0.0F;

            maxRefBeforeDistSq = std::max(maxRefBeforeDistSq, beforeDistSq);
            maxRefAfterDistSq = std::max(maxRefAfterDistSq, afterDistSq);
            maxRootTargetDistSq = std::max(maxRootTargetDistSq, rootDistSq);

            sampleActorID = actor->GetFormID();
            sampleRefBefore = refBefore;
            sampleRefAfter = refAfter;
            sampleTarget = target;
            sampleRoot = root;
            sampleHasRoot = hasRoot;
            ++guarded;
        }

        if (guarded == 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        bool writeLog = false;
        {
            std::scoped_lock lock(_stateMutex);
            auto& last = _lastReferenceGuardLog[threadID];
            if (last.time_since_epoch().count() == 0 ||
                now - last >= kReferenceGuardLogInterval) {
                last = now;
                writeLog = true;
            }
        }

        if (writeLog) {
            SKSE::log::info(
                "OSTNET FREE SCENE REFERENCE GUARD thread={} node={} guarded={} actor={:08X} refBefore=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) refAfter=({:.3f},{:.3f},{:.3f}) root={}({:.3f},{:.3f},{:.3f}) maxRefBeforeDist2={:.4f} maxRefAfterDist2={:.6f} maxRootTargetDist2={:.4f} referenceLocked=1 rootTouched=0",
                threadID,
                CurrentNodeID(thread),
                guarded,
                sampleActorID,
                sampleRefBefore.x,
                sampleRefBefore.y,
                sampleRefBefore.z,
                sampleTarget.x,
                sampleTarget.y,
                sampleTarget.z,
                sampleRefAfter.x,
                sampleRefAfter.y,
                sampleRefAfter.z,
                sampleHasRoot ? 1 : 0,
                sampleRoot.x,
                sampleRoot.y,
                sampleRoot.z,
                maxRefBeforeDistSq,
                maxRefAfterDistSq,
                maxRootTargetDistSq);
        }
    }
}
