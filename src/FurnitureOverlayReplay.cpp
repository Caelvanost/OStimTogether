#include "PCH.h"
#include "FurnitureOverlayReplay.h"

#include "RaceMenuOverlayBridge.h"
#include "SKEEOverlayRefresh.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kMarker = "CumOverlays";
        constexpr std::string_view kChannel = "OCum";
        constexpr auto kPollInterval = std::chrono::milliseconds(250);
        constexpr auto kReplayDelay1 = std::chrono::milliseconds(350);
        constexpr auto kReplayDelay2 = std::chrono::milliseconds(1100);
    }

    FurnitureOverlayReplay& FurnitureOverlayReplay::GetSingleton()
    {
        static FurnitureOverlayReplay singleton;
        return singleton;
    }

    void FurnitureOverlayReplay::StartListener::listen(OStim::Thread* thread)
    {
        FurnitureOverlayReplay::GetSingleton().HandleStart(thread);
    }

    void FurnitureOverlayReplay::NodeListener::listen(OStim::Thread* thread)
    {
        FurnitureOverlayReplay::GetSingleton().HandleNode(thread);
    }

    void FurnitureOverlayReplay::StopListener::listen(OStim::Thread* thread)
    {
        FurnitureOverlayReplay::GetSingleton().HandleStop(thread);
    }

    bool FurnitureOverlayReplay::Initialize()
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
                "OSTNET OCUM FURNITURE REPLAY unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM FURNITURE REPLAY unavailable: Threads interface missing");
            return false;
        }

        // Read the interface version once, outside every OStim event callback.
        // Re-entering ThreadInterface while OStim dispatches NODE/START can
        // deadlock if the dispatcher still owns its thread mutex.
        _supportsFurniture = _threads->getVersion() >= 3;

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM FURNITURE REPLAY READY pollMs={} delays={}+{} method=free-scene-racemenu-pipeline physicalFurnitureOnly=1 callbackReentry=0 supportsFurniture={}",
            kPollInterval.count(),
            kReplayDelay1.count(),
            kReplayDelay2.count(),
            _supportsFurniture ? 1 : 0);
        return true;
    }

    void FurnitureOverlayReplay::Reset()
    {
        _activeFurnitureThreads.clear();
        _replayGeneration.clear();
        _overlaySignatures.clear();
        _nextGeneration = 1;
        _nextPoll = {};
    }

    bool FurnitureOverlayReplay::IsPhysicalFurniture(
        OStim::Thread* thread,
        RE::FormID* furnitureFormID) const
    {
        if (!thread || !_supportsFurniture) {
            return false;
        }

        auto* furniture = static_cast<RE::TESObjectREFR*>(
            thread->getFurnitureObject());
        auto* base = furniture ? furniture->GetBaseObject() : nullptr;

        // Free/wall scenes deliberately use a temporary XMarkerHeading as the
        // unified scene anchor. Its base is not TESFurniture, so those already-
        // validated scenes are excluded from this repair path.
        if (!furniture || !base || !base->As<RE::TESFurniture>()) {
            return false;
        }

        if (furnitureFormID) {
            *furnitureFormID = furniture->GetFormID();
        }
        return true;
    }

    void FurnitureOverlayReplay::HandleStart(OStim::Thread* thread)
    {
        RE::FormID furnitureID = 0;
        if (!IsPhysicalFurniture(thread, &furnitureID)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeFurnitureThreads.insert(threadID);

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                _overlaySignatures.erase(actor->GetFormID());
            }
        }

        SKSE::log::info(
            "OSTNET OCUM FURNITURE START thread={} furniture={:08X} actors={} action=schedule-free-pipeline callbackReentry=0",
            threadID,
            furnitureID,
            thread->getActorCount());

        // Do not call ThreadInterface::getThread() from this callback.
        ScheduleReplay(threadID, furnitureID, "START");
    }

    void FurnitureOverlayReplay::HandleNode(OStim::Thread* thread)
    {
        RE::FormID furnitureID = 0;
        if (!IsPhysicalFurniture(thread, &furnitureID)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeFurnitureThreads.insert(threadID);

        auto* node = thread->getCurrentNode();
        const char* nodeID = node ? node->getNodeID() : nullptr;

        SKSE::log::info(
            "OSTNET OCUM FURNITURE NODE thread={} furniture={:08X} node={} action=schedule-free-pipeline callbackReentry=0",
            threadID,
            furnitureID,
            nodeID ? nodeID : "none");

        // OStim may still hold its internal thread mutex while dispatching this
        // listener. Carry forward the already-known furniture ID and defer all
        // ThreadInterface access until the scheduled game-thread pass.
        ScheduleReplay(threadID, furnitureID, "NODE");
    }

    void FurnitureOverlayReplay::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeFurnitureThreads.erase(threadID);
        _replayGeneration[threadID] = _nextGeneration++;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                _overlaySignatures.erase(actor->GetFormID());
            }
        }
    }

    std::string FurnitureOverlayReplay::BuildSignature(
        const std::vector<std::string>& chunks)
    {
        std::string signature = fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:{}|", chunk.size(), chunk);
        }
        return signature;
    }

    void FurnitureOverlayReplay::ScheduleReplay(
        std::int32_t threadID,
        RE::FormID expectedFurniture,
        std::string_view trigger)
    {
        // Callback-safe by design: no _threads->getThread(), getVersion(), or
        // other ThreadInterface calls here. START/NODE listeners can therefore
        // schedule work without re-entering OStim's thread registry.
        if (!_supportsFurniture || expectedFurniture == 0) {
            return;
        }

        const auto generation = _nextGeneration++;
        _replayGeneration[threadID] = generation;
        const std::string triggerCopy(trigger);

        const auto schedule =
            [threadID, generation, expectedFurniture, triggerCopy](
                std::chrono::milliseconds delay,
                const char* phase) {
                std::thread(
                    [threadID,
                     generation,
                     expectedFurniture,
                     triggerCopy,
                     delay,
                     phase = std::string(phase)]() {
                        std::this_thread::sleep_for(delay);

                        auto* tasks = SKSE::GetTaskInterface();
                        if (!tasks) {
                            return;
                        }

                        tasks->AddTask(
                            [threadID,
                             generation,
                             expectedFurniture,
                             triggerCopy,
                             phase]() {
                                FurnitureOverlayReplay::GetSingleton().
                                    RunReplayPass(
                                        threadID,
                                        generation,
                                        expectedFurniture,
                                        triggerCopy,
                                        phase);
                            });
                    }).detach();
            };

        schedule(kReplayDelay1, "T350");
        schedule(kReplayDelay2, "T1100");
    }

    void FurnitureOverlayReplay::RunReplayPass(
        std::int32_t threadID,
        std::uint64_t generation,
        RE::FormID expectedFurniture,
        std::string_view trigger,
        std::string_view phase)
    {
        const auto generationIt = _replayGeneration.find(threadID);
        if (generationIt == _replayGeneration.end() ||
            generationIt->second != generation ||
            !_activeFurnitureThreads.contains(threadID) ||
            !_threads) {
            return;
        }

        // This runs later on Skyrim's game thread, outside the OStim callback.
        // ThreadInterface re-entry is safe here.
        auto* thread = _threads->getThread(threadID);
        RE::FormID furnitureID = 0;
        if (!IsPhysicalFurniture(thread, &furnitureID) ||
            furnitureID != expectedFurniture) {
            return;
        }

        std::uint32_t replayed = 0;
        std::uint32_t empty = 0;
        auto& transport = STRPMTransport::GetSingleton();

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            // This repair is for the two multiplayer representations only:
            // the real local player and the connected STR proxy. Ordinary NPC
            // RaceMenu overlays remain entirely mod-owned.
            const bool isPlayer = actor->IsPlayerRef();
            const bool isProxy =
                !isPlayer &&
                transport.ResolveConnection(actor->GetFormID()).has_value();
            if (!isPlayer && !isProxy) {
                continue;
            }

            const auto chunks =
                RaceMenuOverlayBridge::GetSingleton()
                    .CaptureMarkedOverlayChunks(
                        actor,
                        kMarker,
                        2200);

            if (chunks.empty()) {
                ++empty;
                continue;
            }

            if (isPlayer) {
                // Known-good local path: materialize the current overrides on
                // the live Body [OvlN] nodes, then ask RaceMenu's update manager
                // to rebuild/apply them once more.
                RaceMenuOverlayBridge::GetSingleton()
                    .RefreshLocalOverlayGeometry(
                        actor,
                        kChannel,
                        chunks);
                SKEEOverlayRefresh::Queue(
                    actor,
                    "OCUM-OVERLAY-CHANGED");
            } else {
                // Furniture proxy path must not use OCUM-OVERLAY-CHANGED here:
                // that reason intentionally performs RemoveOverlays/AddOverlays
                // for proxies. In the 0.34.2 furniture test that hard reinstall
                // transiently duplicated one CumOverlays slot into two and the
                // remote 3D overlay stayed invisible. Re-apply the already-
                // authoritative snapshot directly to the existing Body [OvlN]
                // geometry, exactly like the local materialization strategy,
                // then request only the lightweight ActorUpdateManager path.
                for (const auto& chunk : chunks) {
                    RaceMenuOverlayBridge::GetSingleton()
                        .ApplyRemoteOverlayChunk(
                            actor,
                            kChannel,
                            chunk);
                }
                SKEEOverlayRefresh::Queue(
                    actor,
                    "OCUM-FURNITURE-PROXY-REPLAY");
            }

            ++replayed;

            SKSE::log::info(
                "OSTNET OCUM FURNITURE OVERLAY REPLAY trigger={} phase={} thread={} furniture={:08X} actor={:08X} player={} chunks={} action={}",
                trigger,
                phase,
                threadID,
                furnitureID,
                actor->GetFormID(),
                isPlayer ? 1 : 0,
                chunks.size(),
                isPlayer ?
                    "player-direct+racemenu" :
                    "proxy-direct+light-racemenu");
        }

        SKSE::log::info(
            "OSTNET OCUM FURNITURE OVERLAY PASS trigger={} phase={} thread={} furniture={:08X} replayed={} empty={} generation={}",
            trigger,
            phase,
            threadID,
            furnitureID,
            replayed,
            empty,
            generation);
    }

    void FurnitureOverlayReplay::Tick()
    {
        if (!_threads || _activeFurnitureThreads.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_nextPoll.time_since_epoch().count() != 0 && now < _nextPoll) {
            return;
        }
        _nextPoll = now + kPollInterval;

        std::vector<std::int32_t> stale;
        for (const auto threadID : _activeFurnitureThreads) {
            auto* thread = _threads->getThread(threadID);
            RE::FormID furnitureID = 0;
            if (!IsPhysicalFurniture(thread, &furnitureID)) {
                stale.push_back(threadID);
                continue;
            }

            bool changedWithOverlay = false;
            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor) {
                    continue;
                }

                if (!actor->IsPlayerRef() &&
                    !STRPMTransport::GetSingleton()
                         .ResolveConnection(actor->GetFormID()).has_value()) {
                    continue;
                }

                const auto chunks =
                    RaceMenuOverlayBridge::GetSingleton()
                        .CaptureMarkedOverlayChunks(
                            actor,
                            kMarker,
                            2200);
                const auto signature = BuildSignature(chunks);
                auto& previous = _overlaySignatures[actor->GetFormID()];

                if (previous != signature) {
                    previous = signature;
                    if (!chunks.empty()) {
                        changedWithOverlay = true;
                    }
                }
            }

            // OCumStateSync already performs the immediate free-scene refresh
            // when the snapshot changes. Furniture needs the SAME refresh again
            // after the furniture/body attachment has had time to settle.
            if (changedWithOverlay) {
                ScheduleReplay(
                    threadID,
                    furnitureID,
                    "OVERLAY-CHANGED");
            }
        }

        for (const auto threadID : stale) {
            _activeFurnitureThreads.erase(threadID);
            _replayGeneration[threadID] = _nextGeneration++;
        }
    }
}
