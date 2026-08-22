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
            "OSTNET FREE SCENE ALIGN READY threadsVersion={} mode=noFurniture-native-ownership continuousCorrections=0",
            _threads->getVersion());
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
        {
            std::scoped_lock lock(_stateMutex);
            _pendingRelease.insert(threadID);
            _releasedThreads.erase(threadID);
        }

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN armed thread={} node={} delayMs={} furniture=none action=release-all-continuous-position-ownership",
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

            if (wasReleased) {
                bridge.EnableSTRProxyPoseGuard(
                    threadID,
                    wall ? kWallGuardDelay : kFurnitureGuardDelay,
                    wall ? "free-to-wall" : "free-to-furniture");
            }
            return;
        }

        if (!HasSTRProxy(thread)) {
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

        if (schedule) {
            const auto delay = alreadyReleased ?
                kNodeFreeSceneReleaseDelay :
                kInitialFreeSceneReleaseDelay;

            SKSE::log::info(
                "OSTNET FREE SCENE ALIGN node thread={} node={} delayMs={} alreadyReleased={} action=release-after-native-node-align",
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
        std::scoped_lock lock(_stateMutex);
        _pendingRelease.erase(threadID);
        _releasedThreads.erase(threadID);
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

        // In a free-standing scene the START/NODE world pose is only an
        // initial common origin. OStim animation/root motion is allowed to
        // move every participant after that. Disable OStim Together's full
        // proxy guard and stop only OStim's completed TranslateTo on the STR
        // proxy. There is deliberately no replacement reference/root guard.
        bridge.DisableSTRProxyPoseGuard(
            threadID,
            "free-scene-native-ownership");

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
            SKSE::log::info(
                "OSTNET FREE SCENE NATIVE OWNERSHIP thread={} node={} proxies={} reason={} continuousCorrections=0 proxyOwner=STR actorsOwner=OStim-animation",
                threadID,
                CurrentNodeID(thread),
                released,
                reason);
        }
    }
}
