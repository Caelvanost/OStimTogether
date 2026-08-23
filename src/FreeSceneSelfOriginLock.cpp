#include "PCH.h"
#include "FreeSceneSelfOriginLock.h"

#include "OStimBridge.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr auto kLogInterval = std::chrono::milliseconds(500);
        constexpr float kWriteToleranceSq = 0.01F;
    }

    FreeSceneSelfOriginLock& FreeSceneSelfOriginLock::GetSingleton()
    {
        static FreeSceneSelfOriginLock instance;
        return instance;
    }

    void FreeSceneSelfOriginLock::StartListener::listen(OStim::Thread* thread)
    {
        FreeSceneSelfOriginLock::GetSingleton().HandleStart(thread);
    }

    void FreeSceneSelfOriginLock::StopListener::listen(OStim::Thread* thread)
    {
        FreeSceneSelfOriginLock::GetSingleton().HandleStop(thread);
    }

    bool FreeSceneSelfOriginLock::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        if (!messaging->Dispatch(
                OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
                &exchange,
                sizeof(exchange),
                nullptr) ||
            !exchange.interfaceMap) {
            SKSE::log::warn(
                "OSTNET SELF ORIGIN unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET SELF ORIGIN unavailable: Threads interface missing");
            return false;
        }

        _threadInterfaceVersion = _threads->getVersion();
        if (_threadInterfaceVersion < 3) {
            SKSE::log::info(
                "OSTNET SELF ORIGIN disabled threadsVersion={} reason=no-exact-furniture-state",
                _threadInterfaceVersion);
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET SELF ORIGIN READY threadsVersion={} ownership=real-local-player write=reference-location-only skeletonWrites=0 update3D=0 proxyWrites=0",
            _threadInterfaceVersion);
        return true;
    }

    void FreeSceneSelfOriginLock::Reset()
    {
        _activeThreadID = -1;
        _center = {};
        _lastLog = {};
    }

    bool FreeSceneSelfOriginLock::IsFreeStandingThread(OStim::Thread* thread) const
    {
        if (!thread ||
            !thread->isPlayerThread() ||
            _threadInterfaceVersion < 3 ||
            thread->getFurnitureObject()) {
            return false;
        }

        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        return nodeID &&
               std::string_view(nodeID).find("wall") == std::string_view::npos;
    }

    RE::PlayerCharacter* FreeSceneSelfOriginLock::FindLocalPlayer(
        OStim::Thread* thread) const
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!thread || !player) {
            return nullptr;
        }

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (actor == player) {
                return player;
            }
        }
        return nullptr;
    }

    bool FreeSceneSelfOriginLock::HasSTRRemoteParticipant(
        OStim::Thread* thread) const
    {
        if (!thread) {
            return false;
        }

        auto& transport = STRPMTransport::GetSingleton();
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;

            if (!actor || actor->IsPlayerRef()) {
                continue;
            }

            if (transport.ResolveConnection(actor->GetFormID())) {
                return true;
            }
        }

        return false;
    }

    void FreeSceneSelfOriginLock::HandleStart(OStim::Thread* thread)
    {
        if (!thread ||
            !FindLocalPlayer(thread) ||
            !HasSTRRemoteParticipant(thread)) {
            return;
        }

        // OStim emits START while its own thread startup is still in progress.
        // Calling the ModAPI GetActorAlignment path synchronously from this
        // listener can re-enter OStim's thread-control state and hang the game.
        // Match OStimBridge's startup ordering: first hop yields back to OStim
        // so ChangeNode() can enqueue lockAtPosition(), second hop runs after
        // those startup tasks and only then reads alignment/scene-center state.
        QueueArmAfterStart(thread->getThreadID());
    }

    void FreeSceneSelfOriginLock::QueueArmAfterStart(std::int32_t threadID)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            SKSE::log::warn(
                "OSTNET SELF ORIGIN ARM skipped thread={} reason=no-task-interface",
                threadID);
            return;
        }

        SKSE::log::info(
            "OSTNET SELF ORIGIN ARM queued thread={} defer=two-hop",
            threadID);

        tasks->AddTask(
            [threadID]() {
                auto* secondHop = SKSE::GetTaskInterface();
                if (!secondHop) {
                    return;
                }

                secondHop->AddTask(
                    [threadID]() {
                        FreeSceneSelfOriginLock::GetSingleton().
                            ArmAfterStart(threadID);
                    });
            });
    }

    void FreeSceneSelfOriginLock::ArmAfterStart(std::int32_t threadID)
    {
        auto* thread = _threads ? _threads->getThread(threadID) : nullptr;
        if (!thread ||
            !IsFreeStandingThread(thread) ||
            !FindLocalPlayer(thread) ||
            !HasSTRRemoteParticipant(thread)) {
            return;
        }

        auto& bridge = OStimBridge::GetSingleton();
        const bool remoteMirror =
            bridge.IsRemoteMirrorForAlignment(threadID);

        SceneCenter center{};
        const bool centerReady = remoteMirror ?
            bridge.TryGetAuthoritativeSceneCenter(threadID, center) :
            bridge.TryComputeSceneCenter(thread, center, false);

        if (!centerReady || !center.IsFinite()) {
            SKSE::log::warn(
                "OSTNET SELF ORIGIN ARM failed thread={} reason=no-scene-center source={}",
                threadID,
                remoteMirror ? "remote-start" : "local-derived");
            return;
        }

        _activeThreadID = threadID;
        _center = center;
        _lastLog = {};

        SKSE::log::info(
            "OSTNET SELF ORIGIN ARM thread={} center=({:.3f},{:.3f},{:.3f},{:.5f}) source={} mode=reference-location-only continuousLocalAuthority=1 skeletonWrites=0 proxyWrites=0",
            _activeThreadID,
            _center.x,
            _center.y,
            _center.z,
            _center.r,
            remoteMirror ? "remote-start" : "local-derived");
    }

    void FreeSceneSelfOriginLock::HandleStop(OStim::Thread* thread)
    {
        if (thread && thread->getThreadID() == _activeThreadID) {
            SKSE::log::info(
                "OSTNET SELF ORIGIN STOP thread={}",
                _activeThreadID);
            Reset();
        }
    }

    void FreeSceneSelfOriginLock::Tick()
    {
        if (!_threads || _activeThreadID < 0 || !_center.IsFinite()) {
            return;
        }

        auto* thread = _threads->getThread(_activeThreadID);
        if (!thread || !IsFreeStandingThread(thread)) {
            return;
        }

        auto* player = FindLocalPlayer(thread);
        if (!player) {
            return;
        }

        const RE::NiPoint3 target{
            _center.x,
            _center.y,
            _center.z
        };

        const auto before = player->GetPosition();
        RE::NiPoint3 rootBefore = before;
        bool hadRootBefore = false;
        if (auto* root = player->Get3D()) {
            rootBefore = root->world.translate;
            hadRootBefore = true;
        }

        const auto driftSq = before.GetSquaredDistance(target);
        bool wrote = false;
        if (driftSq > kWriteToleranceSq) {
            // Deliberately modify only the local real player's logical
            // reference origin. Do not call SetPosition(), TranslateTo(),
            // Update3DPosition(), or write any skeleton node. The local
            // animation keeps its rendered displacement while STR publishes a
            // common free-scene origin to the remote proxy.
            static_cast<RE::TESObjectREFR*>(player)->data.location = target;
            wrote = true;
        }

        const auto after = player->GetPosition();
        RE::NiPoint3 rootAfter = rootBefore;
        bool hadRootAfter = false;
        if (auto* root = player->Get3D()) {
            rootAfter = root->world.translate;
            hadRootAfter = true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_lastLog.time_since_epoch().count() == 0 ||
            now - _lastLog >= kLogInterval) {
            _lastLog = now;
            SKSE::log::info(
                "OSTNET SELF ORIGIN LOCK thread={} node={} wrote={} refBefore=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) refAfter=({:.3f},{:.3f},{:.3f}) rootBefore={}({:.3f},{:.3f},{:.3f}) rootAfter={}({:.3f},{:.3f},{:.3f}) driftBefore2={:.3f} driftAfter2={:.6f} rootMoved2={:.6f} referenceOnly=1 skeletonWrites=0 update3D=0 proxyWrites=0",
                _activeThreadID,
                thread->getCurrentNode() && thread->getCurrentNode()->getNodeID() ?
                    thread->getCurrentNode()->getNodeID() : "",
                wrote ? 1 : 0,
                before.x,
                before.y,
                before.z,
                target.x,
                target.y,
                target.z,
                after.x,
                after.y,
                after.z,
                hadRootBefore ? 1 : 0,
                rootBefore.x,
                rootBefore.y,
                rootBefore.z,
                hadRootAfter ? 1 : 0,
                rootAfter.x,
                rootAfter.y,
                rootAfter.z,
                driftSq,
                after.GetSquaredDistance(target),
                hadRootBefore && hadRootAfter ?
                    rootAfter.GetSquaredDistance(rootBefore) : 0.0F);
        }
    }
}
