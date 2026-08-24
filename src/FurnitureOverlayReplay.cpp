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

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM FURNITURE REPLAY READY pollMs={} delays={}+{} method=free-scene-racemenu-pipeline physicalFurnitureOnly=1",
            kPollInterval.count(),
            kReplayDelay1.count(),
            kReplayDelay2.count());
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
        if (!thread || !_threads || _threads->getVersion() < 3) {
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
            "OSTNET OCUM FURNITURE START thread={} furniture={:08X} actors={} action=schedule-free-pipeline",
            threadID,
            furnitureID,
            thread->getActorCount());

        ScheduleReplay(threadID, "START");
    }

    void FurnitureOverlayReplay::HandleNode(OStim::Thread* thread)
    {
        RE::FormID furnitureID = 0;
        if (!IsPhysicalFurniture(thread, &furnitureID)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeFurnitureThreads.insert(threadID);

        const auto* node = thread->getCurrentNode();
        const char* nodeID = node ? node->getNodeID() : nullptr;

        SKSE::log::info(
            "OSTNET OCUM FURNITURE NODE thread={} furniture={:08X} node={} action=schedule-free-pipeline",
            threadID,
            furnitureID,
            nodeID ? nodeID : "none");

        ScheduleReplay(threadID, "NODE");
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
        std::string_view trigger)
    {
        if (!_threads) {
            return;
        }

        auto* thread = _threads->getThread(threadID);
        RE::FormID furnitureID = 0;
        if (!IsPhysicalFurniture(thread, &furnitureID)) {
            return;
        }

        const auto generation = _nextGeneration++;
        _replayGeneration[threadID] = generation;
        const std::string triggerCopy(trigger);

        const auto schedule =
            [threadID, generation, furnitureID, triggerCopy](
                std::chrono::milliseconds delay,
                const char* phase) {
                std::thread(
                    [threadID,
                     generation,
                     furnitureID,
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
                             furnitureID,
                             triggerCopy,
                             phase]() {
                                FurnitureOverlayReplay::GetSingleton().
                                    RunReplayPass(
                                        threadID,
                                        generation,
                                        furnitureID,
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
            if (!actor->IsPlayerRef() &&
                !transport.ResolveConnection(actor->GetFormID()).has_value()) {
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

            // Reuse the exact path that has already rendered these 3D overlays
            // in free-standing scenes. No furniture-specific shader, material,
            // slot or mesh implementation is introduced here.
            if (actor->IsPlayerRef()) {
                RaceMenuOverlayBridge::GetSingleton()
                    .RefreshLocalOverlayGeometry(
                        actor,
                        kChannel,
                        chunks);
            }

            SKEEOverlayRefresh::Queue(
                actor,
                "OCUM-OVERLAY-CHANGED");
            ++replayed;

            SKSE::log::info(
                "OSTNET OCUM FURNITURE OVERLAY REPLAY trigger={} phase={} thread={} furniture={:08X} actor={:08X} player={} chunks={} action=free-scene-pipeline",
                trigger,
                phase,
                threadID,
                furnitureID,
                actor->GetFormID(),
                actor->IsPlayerRef() ? 1 : 0,
                chunks.size());
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
            if (!IsPhysicalFurniture(thread)) {
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
                ScheduleReplay(threadID, "OVERLAY-CHANGED");
            }
        }

        for (const auto threadID : stale) {
            _activeFurnitureThreads.erase(threadID);
            _replayGeneration[threadID] = _nextGeneration++;
        }
    }
}
