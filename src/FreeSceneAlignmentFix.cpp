#include "PCH.h"
#include "FreeSceneAlignmentFix.h"

#include "OStimBridge.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr auto kFreeSceneReleaseDelay =
            std::chrono::milliseconds(250);

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

        bool IsWallNode(OStim::Thread* thread)
        {
            auto* node = thread ? thread->getCurrentNode() : nullptr;
            const auto* nodeID = node ? node->getNodeID() : nullptr;
            return nodeID && std::string_view(nodeID).find("wall") !=
                std::string_view::npos;
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

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN READY threadsVersion={} mode=noFurniture-nonWall-release-proxy-root-motion",
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
        if (!bridge.SupportsThreadFurniture()) {
            return;
        }

        if (thread->getFurnitureObject() || IsWallNode(thread)) {
            return;
        }

        bool hasSTRProxy = false;
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (IsLikelySTRRemotePlayerProxy(actor)) {
                hasSTRProxy = true;
                break;
            }
        }

        if (!hasSTRProxy) {
            return;
        }

        const auto threadID = thread->getThreadID();
        const auto* nodeID = thread->getCurrentNode() &&
                thread->getCurrentNode()->getNodeID() ?
            thread->getCurrentNode()->getNodeID() : "";

        SKSE::log::info(
            "OSTNET FREE SCENE ALIGN armed thread={} node={} delayMs={} furniture=none action=release-proxy-after-initial-align",
            threadID,
            nodeID,
            kFreeSceneReleaseDelay.count());

        std::thread([this, threadID]() {
            std::this_thread::sleep_for(kFreeSceneReleaseDelay);

            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }

            tasks->AddTask([this, threadID]() {
                ReleaseFreeSceneProxy(threadID);
            });
        }).detach();
    }

    void FreeSceneAlignmentFix::ReleaseFreeSceneProxy(std::int32_t threadID)
    {
        if (!_threads) {
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        if (!bridge.SupportsThreadFurniture() ||
            thread->getFurnitureObject() ||
            IsWallNode(thread)) {
            return;
        }

        // The OStimBridge START listener has already armed its normal 200 ms
        // proxy guard. Removing it now preserves that short initial settling
        // window, but stops the long-lived center pin that fights root motion
        // in standing/free animation packs.
        bridge.DisableSTRProxyPoseGuard(
            threadID,
            "free-scene-no-furniture");

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

        if (released > 0) {
            const auto* nodeID = thread->getCurrentNode() &&
                    thread->getCurrentNode()->getNodeID() ?
                thread->getCurrentNode()->getNodeID() : "";

            SKSE::log::info(
                "OSTNET FREE SCENE PROXY RELEASE thread={} node={} proxies={} action=stop-translation owner=STR rootMotion=enabled",
                threadID,
                nodeID,
                released);
        }
    }
}
