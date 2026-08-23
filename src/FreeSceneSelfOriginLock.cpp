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

        // Normal START/NODE phase replay in the validated 7.5b runtime settles
        // in roughly 0.6 s. The first correction is deliberately later. The
        // second correction is a bounded backup for high-latency phase commits;
        // unlike the old self lock this never writes every frame.
        constexpr auto kMirrorSelfFirstDelay =
            std::chrono::milliseconds(700);
        constexpr auto kMirrorSelfSecondDelay =
            std::chrono::milliseconds(1100);
        constexpr float kMirrorSelfCorrectionToleranceSq = 1.0F;

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
                "OSTNET PROXY ORIGIN unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET PROXY ORIGIN unavailable: Threads interface missing");
            return false;
        }

        _threadInterfaceVersion = _threads->getVersion();
        if (_threadInterfaceVersion < 3) {
            SKSE::log::info(
                "OSTNET PROXY ORIGIN disabled threadsVersion={} reason=no-exact-furniture-state",
                _threadInterfaceVersion);
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET PROXY ORIGIN READY threadsVersion={} proxyWrite=reference-location-only mirrorSelf=bounded-two-shot localOwnerWrites=0 skeletonWrites=0",
            _threadInterfaceVersion);
        return true;
    }

    void FreeSceneSelfOriginLock::Reset()
    {
        _activeThreadID = -1;
        _center = {};
        _lastLog = {};
        _mirrorSelfNodeID.clear();
        _mirrorSelfCorrectionStage = 0;
        _mirrorSelfCorrectionDue = {};
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
        // Keep observer-only Player+NPC mirrors untouched: they already use
        // OStim's native alignment correctly. This path is only for a shared
        // scene where the real local player and at least one STR proxy are both
        // participants.
        if (!thread ||
            !FindLocalPlayer(thread) ||
            !HasSTRRemoteParticipant(thread)) {
            return;
        }

        // OStim emits START while its own thread startup is still in progress.
        // Calling the ModAPI GetActorAlignment path synchronously from this
        // listener can re-enter OStim's thread-control state and hang the game.
        QueueArmAfterStart(thread->getThreadID());
    }

    void FreeSceneSelfOriginLock::QueueArmAfterStart(std::int32_t threadID)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            SKSE::log::warn(
                "OSTNET PROXY ORIGIN ARM skipped thread={} reason=no-task-interface",
                threadID);
            return;
        }

        SKSE::log::info(
            "OSTNET PROXY ORIGIN ARM queued thread={} defer=two-hop",
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
                "OSTNET PROXY ORIGIN ARM failed thread={} reason=no-scene-center source={}",
                threadID,
                remoteMirror ? "remote-start" : "local-derived");
            return;
        }

        _activeThreadID = threadID;
        _center = center;
        _lastLog = {};

        if (remoteMirror) {
            ArmMirrorSelfCorrections(thread);
        } else {
            _mirrorSelfNodeID.clear();
            _mirrorSelfCorrectionStage = 0;
            _mirrorSelfCorrectionDue = {};
        }

        SKSE::log::info(
            "OSTNET PROXY ORIGIN ARM thread={} center=({:.3f},{:.3f},{:.3f},{:.5f}) source={} mode=proxy-reference-location-only mirrorSelf={} skeletonWrites=0",
            _activeThreadID,
            _center.x,
            _center.y,
            _center.z,
            _center.r,
            remoteMirror ? "remote-start" : "local-derived",
            remoteMirror ? "bounded-two-shot" : "none");
    }

    void FreeSceneSelfOriginLock::ArmMirrorSelfCorrections(OStim::Thread* thread)
    {
        auto* node = thread ? thread->getCurrentNode() : nullptr;
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        if (!nodeID || !*nodeID) {
            return;
        }

        _mirrorSelfNodeID = nodeID;
        _mirrorSelfCorrectionStage = 0;
        _mirrorSelfCorrectionDue =
            std::chrono::steady_clock::now() +
            kMirrorSelfFirstDelay;

        SKSE::log::info(
            "OSTNET MIRROR SELF ONESHOT ARMED thread={} node={} firstDelayMs={} secondDelayMs={} maxWrites=2",
            thread->getThreadID(),
            _mirrorSelfNodeID,
            kMirrorSelfFirstDelay.count(),
            kMirrorSelfSecondDelay.count());
    }

    void FreeSceneSelfOriginLock::ApplyMirrorSelfCorrection(
        OStim::Thread* thread,
        RE::PlayerCharacter* localPlayer,
        std::uint32_t stage)
    {
        if (!thread || !localPlayer || !_center.IsFinite()) {
            return;
        }

        const RE::NiPoint3 target{
            _center.x,
            _center.y,
            _center.z
        };

        auto* reference =
            static_cast<RE::TESObjectREFR*>(localPlayer);

        const auto refBefore = localPlayer->GetPosition();
        const auto headingBefore = localPlayer->GetAngleZ();

        RE::NiPoint3 rootBefore = refBefore;
        bool hadRootBefore = false;
        if (auto* root = localPlayer->Get3D()) {
            rootBefore = root->world.translate;
            hadRootBefore = true;
        }

        const float refDistBeforeSq =
            refBefore.GetSquaredDistance(target);
        const float rootDistBeforeSq =
            rootBefore.GetSquaredDistance(target);

        const bool needsWrite =
            refDistBeforeSq > kMirrorSelfCorrectionToleranceSq ||
            (hadRootBefore &&
             rootDistBeforeSq > kMirrorSelfCorrectionToleranceSq);

        if (needsWrite) {
            // The 0.31.5 logs proved that the true mirror participant acquires
            // the same ~22-unit role/root displacement later seen on the STR
            // proxy. Apply the authoritative OStim scene center only after the
            // startup/node animation has settled. This is a bounded one-shot,
            // not a continuous position lock.
            StopReferenceTranslation(reference);
            localPlayer->SetPosition(target, true);
            localPlayer->SetRotationZ(_center.r);
            reference->data.location = target;
            reference->data.angle.z = _center.r;
            reference->Update3DPosition(true);
        }

        const auto refAfter = localPlayer->GetPosition();
        RE::NiPoint3 rootAfter = refAfter;
        bool hadRootAfter = false;
        if (auto* root = localPlayer->Get3D()) {
            rootAfter = root->world.translate;
            hadRootAfter = true;
        }

        SKSE::log::info(
            "OSTNET MIRROR SELF ONESHOT thread={} node={} stage={} actor={:08X} wrote={} refBefore=({:.3f},{:.3f},{:.3f},{:.5f}) rootBefore={}({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f},{:.5f}) refAfter=({:.3f},{:.3f},{:.3f},{:.5f}) rootAfter={}({:.3f},{:.3f},{:.3f}) refDistBefore2={:.3f} rootDistBefore2={:.3f} refDistAfter2={:.6f} rootDistAfter2={:.6f} bounded=1 continuous=0 skeletonWrites=0",
            thread->getThreadID(),
            thread->getCurrentNode() && thread->getCurrentNode()->getNodeID() ?
                thread->getCurrentNode()->getNodeID() : "",
            stage,
            localPlayer->GetFormID(),
            needsWrite ? 1 : 0,
            refBefore.x,
            refBefore.y,
            refBefore.z,
            headingBefore,
            hadRootBefore ? 1 : 0,
            rootBefore.x,
            rootBefore.y,
            rootBefore.z,
            target.x,
            target.y,
            target.z,
            _center.r,
            refAfter.x,
            refAfter.y,
            refAfter.z,
            localPlayer->GetAngleZ(),
            hadRootAfter ? 1 : 0,
            rootAfter.x,
            rootAfter.y,
            rootAfter.z,
            refDistBeforeSq,
            rootDistBeforeSq,
            refAfter.GetSquaredDistance(target),
            rootAfter.GetSquaredDistance(target));
    }

    void FreeSceneSelfOriginLock::HandleStop(OStim::Thread* thread)
    {
        if (thread && thread->getThreadID() == _activeThreadID) {
            SKSE::log::info(
                "OSTNET PROXY ORIGIN STOP thread={}",
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
        auto* localPlayer = thread ? FindLocalPlayer(thread) : nullptr;
        if (!thread ||
            !IsFreeStandingThread(thread) ||
            !localPlayer) {
            return;
        }

        const RE::NiPoint3 target{
            _center.x,
            _center.y,
            _center.z
        };

        auto& bridge = OStimBridge::GetSingleton();
        const bool remoteMirror =
            bridge.IsRemoteMirrorForAlignment(_activeThreadID);

        auto* currentNode = thread->getCurrentNode();
        const auto* currentNodeID =
            currentNode ? currentNode->getNodeID() : nullptr;

        const auto now = std::chrono::steady_clock::now();

        if (remoteMirror && currentNodeID && *currentNodeID) {
            if (_mirrorSelfNodeID != currentNodeID) {
                ArmMirrorSelfCorrections(thread);
            }

            if (_mirrorSelfCorrectionStage < 2 &&
                _mirrorSelfCorrectionDue.time_since_epoch().count() != 0 &&
                now >= _mirrorSelfCorrectionDue) {
                const auto stage = _mirrorSelfCorrectionStage + 1;
                ApplyMirrorSelfCorrection(thread, localPlayer, stage);
                ++_mirrorSelfCorrectionStage;

                if (_mirrorSelfCorrectionStage < 2) {
                    _mirrorSelfCorrectionDue =
                        now + kMirrorSelfSecondDelay;
                } else {
                    _mirrorSelfCorrectionDue = {};
                }
            }
        }

        auto& transport = STRPMTransport::GetSingleton();
        const bool shouldLog =
            _lastLog.time_since_epoch().count() == 0 ||
            now - _lastLog >= kLogInterval;

        if (shouldLog) {
            const auto playerRef = localPlayer->GetPosition();
            RE::NiPoint3 playerRoot = playerRef;
            bool hadPlayerRoot = false;
            float playerRootScale = 1.0F;
            if (auto* root = localPlayer->Get3D()) {
                playerRoot = root->world.translate;
                playerRootScale = root->world.scale;
                hadPlayerRoot = true;
            }

            SKSE::log::info(
                "OSTNET FREE ROLE DIAG thread={} node={} kind=local-player actor={:08X} ref=({:.3f},{:.3f},{:.3f}) root={}({:.3f},{:.3f},{:.3f}) rootFromCenter=({:.3f},{:.3f},{:.3f}) rootScale={:.5f} mirror={} correctionStage={} writes=bounded-only",
                _activeThreadID,
                currentNodeID ? currentNodeID : "",
                localPlayer->GetFormID(),
                playerRef.x,
                playerRef.y,
                playerRef.z,
                hadPlayerRoot ? 1 : 0,
                playerRoot.x,
                playerRoot.y,
                playerRoot.z,
                playerRoot.x - target.x,
                playerRoot.y - target.y,
                playerRoot.z - target.z,
                playerRootScale,
                remoteMirror ? 1 : 0,
                _mirrorSelfCorrectionStage);
        }

        std::uint32_t proxyCount = 0;
        std::uint32_t writeCount = 0;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;

            if (!actor || actor->IsPlayerRef() ||
                !transport.ResolveConnection(actor->GetFormID())) {
                continue;
            }

            ++proxyCount;

            const auto before = actor->GetPosition();
            RE::NiPoint3 rootBefore = before;
            bool hadRootBefore = false;
            float rootScale = 1.0F;
            if (auto* root = actor->Get3D()) {
                rootBefore = root->world.translate;
                rootScale = root->world.scale;
                hadRootBefore = true;
            }

            const auto driftSq = before.GetSquaredDistance(target);
            bool wrote = false;
            if (driftSq > kWriteToleranceSq) {
                static_cast<RE::TESObjectREFR*>(actor)->data.location = target;
                wrote = true;
                ++writeCount;
            }

            const auto after = actor->GetPosition();
            RE::NiPoint3 rootAfter = rootBefore;
            bool hadRootAfter = false;
            if (auto* root = actor->Get3D()) {
                rootAfter = root->world.translate;
                rootScale = root->world.scale;
                hadRootAfter = true;
            }

            if (shouldLog) {
                SKSE::log::info(
                    "OSTNET PROXY ORIGIN LOCK thread={} node={} idx={} actor={:08X} wrote={} refBefore=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) refAfter=({:.3f},{:.3f},{:.3f}) rootBefore={}({:.3f},{:.3f},{:.3f}) rootAfter={}({:.3f},{:.3f},{:.3f}) rootFromCenter=({:.3f},{:.3f},{:.3f}) rootScale={:.5f} driftBefore2={:.3f} driftAfter2={:.6f} rootMoved2={:.6f} proxyReferenceOnly=1 skeletonWrites=0 update3D=0",
                    _activeThreadID,
                    currentNodeID ? currentNodeID : "",
                    i,
                    actor->GetFormID(),
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
                    rootAfter.x - target.x,
                    rootAfter.y - target.y,
                    rootAfter.z - target.z,
                    rootScale,
                    driftSq,
                    after.GetSquaredDistance(target),
                    hadRootBefore && hadRootAfter ?
                        rootAfter.GetSquaredDistance(rootBefore) : 0.0F);
            }
        }

        if (shouldLog) {
            _lastLog = now;
            SKSE::log::info(
                "OSTNET PROXY ORIGIN STATE thread={} proxies={} writes={} owner=remote-proxy-reference mirrorSelfStage={}",
                _activeThreadID,
                proxyCount,
                writeCount,
                _mirrorSelfCorrectionStage);
        }
    }
}
