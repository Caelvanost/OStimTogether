#include "PCH.h"
#include "FreeSceneOverlayReplay.h"

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

    FreeSceneOverlayReplay& FreeSceneOverlayReplay::GetSingleton()
    {
        static FreeSceneOverlayReplay singleton;
        return singleton;
    }

    void FreeSceneOverlayReplay::StartListener::listen(OStim::Thread* thread)
    {
        FreeSceneOverlayReplay::GetSingleton().HandleStart(thread);
    }

    void FreeSceneOverlayReplay::StopListener::listen(OStim::Thread* thread)
    {
        FreeSceneOverlayReplay::GetSingleton().HandleStop(thread);
    }

    bool FreeSceneOverlayReplay::Initialize()
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
                "OSTNET OCUM FREE REPLAY unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM FREE REPLAY unavailable: Threads interface missing");
            return false;
        }

        _supportsFurniture = _threads->getVersion() >= 3;
        if (!_supportsFurniture) {
            SKSE::log::warn(
                "OSTNET OCUM FREE REPLAY unavailable: Threads interface version={} lacks furniture object classification",
                _threads->getVersion());
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM FREE REPLAY READY pollMs={} delays={}+{} physicalFurnitureExcluded=1 directPlayer=1 directProxy=1",
            kPollInterval.count(),
            kReplayDelay1.count(),
            kReplayDelay2.count());
        return true;
    }

    void FreeSceneOverlayReplay::Reset()
    {
        _activeThreads.clear();
        _actorThreads.clear();
        _overlaySignatures.clear();
        _actorGenerations.clear();
        _nextGeneration = 1;
        _nextPoll = {};
    }

    bool FreeSceneOverlayReplay::IsPhysicalFurniture(
        OStim::Thread* thread) const
    {
        if (!thread || !_supportsFurniture) {
            return false;
        }

        auto* furniture = static_cast<RE::TESObjectREFR*>(
            thread->getFurnitureObject());
        auto* base = furniture ? furniture->GetBaseObject() : nullptr;
        return base && base->As<RE::TESFurniture>();
    }

    void FreeSceneOverlayReplay::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !_supportsFurniture || IsPhysicalFurniture(thread)) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeThreads.insert(threadID);

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            const auto actorID = actor->GetFormID();
            _actorThreads[actorID] = threadID;
            _overlaySignatures.erase(actorID);
            _actorGenerations.erase(actorID);
        }

        SKSE::log::info(
            "OSTNET OCUM FREE REPLAY START thread={} actors={} mode=non-TESFurniture",
            threadID,
            thread->getActorCount());
    }

    void FreeSceneOverlayReplay::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeThreads.erase(threadID);

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            const auto actorID = actor->GetFormID();
            const auto owner = _actorThreads.find(actorID);
            if (owner != _actorThreads.end() && owner->second == threadID) {
                _actorThreads.erase(owner);
                _overlaySignatures.erase(actorID);
                _actorGenerations[actorID] = _nextGeneration++;
            }
        }
    }

    std::string FreeSceneOverlayReplay::BuildSignature(
        const std::vector<std::string>& chunks)
    {
        std::string signature = fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:{}|", chunk.size(), chunk);
        }
        return signature;
    }

    void FreeSceneOverlayReplay::ApplyDirect(
        RE::Actor* actor,
        const std::vector<std::string>& chunks,
        std::string_view phase)
    {
        if (!actor || chunks.empty()) {
            return;
        }

        const bool isPlayer = actor->IsPlayerRef();
        const bool isProxy =
            !isPlayer &&
            STRPMTransport::GetSingleton()
                .ResolveConnection(actor->GetFormID()).has_value();
        if (!isPlayer && !isProxy) {
            return;
        }

        if (isPlayer) {
            RaceMenuOverlayBridge::GetSingleton()
                .RefreshLocalOverlayGeometry(
                    actor,
                    kChannel,
                    chunks);
            SKEEOverlayRefresh::Queue(
                actor,
                "OCUM-FREE-PLAYER-DIRECT");
        } else {
            for (const auto& chunk : chunks) {
                RaceMenuOverlayBridge::GetSingleton()
                    .ApplyRemoteOverlayChunk(
                        actor,
                        kChannel,
                        chunk);
            }
            SKEEOverlayRefresh::Queue(
                actor,
                "OCUM-FREE-PROXY-DIRECT");
        }

        SKSE::log::info(
            "OSTNET OCUM FREE OVERLAY REPLAY phase={} actor={:08X} player={} chunks={} action={}",
            phase,
            actor->GetFormID(),
            isPlayer ? 1 : 0,
            chunks.size(),
            isPlayer ?
                "player-direct+light-racemenu" :
                "proxy-direct+light-racemenu");
    }

    void FreeSceneOverlayReplay::ScheduleActorReplay(
        RE::FormID actorID,
        std::int32_t threadID)
    {
        const auto generation = _nextGeneration++;
        _actorGenerations[actorID] = generation;

        const auto schedule =
            [actorID, threadID, generation](
                std::chrono::milliseconds delay,
                const char* phase) {
                std::thread(
                    [actorID,
                     threadID,
                     generation,
                     delay,
                     phase = std::string(phase)]() {
                        std::this_thread::sleep_for(delay);

                        auto* tasks = SKSE::GetTaskInterface();
                        if (!tasks) {
                            return;
                        }

                        tasks->AddTask(
                            [actorID,
                             threadID,
                             generation,
                             phase]() {
                                FreeSceneOverlayReplay::GetSingleton()
                                    .RunActorReplay(
                                        actorID,
                                        threadID,
                                        generation,
                                        phase);
                            });
                    }).detach();
            };

        schedule(kReplayDelay1, "T350");
        schedule(kReplayDelay2, "T1100");
    }

    void FreeSceneOverlayReplay::RunActorReplay(
        RE::FormID actorID,
        std::int32_t threadID,
        std::uint64_t generation,
        std::string_view phase)
    {
        const auto gen = _actorGenerations.find(actorID);
        const auto owner = _actorThreads.find(actorID);
        if (gen == _actorGenerations.end() ||
            gen->second != generation ||
            owner == _actorThreads.end() ||
            owner->second != threadID ||
            !_activeThreads.contains(threadID)) {
            return;
        }

        auto* form = RE::TESForm::LookupByID(actorID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            return;
        }

        const auto chunks =
            RaceMenuOverlayBridge::GetSingleton()
                .CaptureMarkedOverlayChunks(
                    actor,
                    kMarker,
                    2200);
        if (chunks.empty()) {
            return;
        }

        ApplyDirect(actor, chunks, phase);
    }

    void FreeSceneOverlayReplay::Tick()
    {
        if (!_threads || _activeThreads.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_nextPoll.time_since_epoch().count() != 0 && now < _nextPoll) {
            return;
        }
        _nextPoll = now + kPollInterval;

        std::vector<std::int32_t> staleThreads;
        auto& transport = STRPMTransport::GetSingleton();

        for (const auto threadID : _activeThreads) {
            auto* thread = _threads->getThread(threadID);
            if (!thread || IsPhysicalFurniture(thread)) {
                staleThreads.push_back(threadID);
                continue;
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor) {
                    continue;
                }

                const bool isPlayer = actor->IsPlayerRef();
                const bool isProxy =
                    !isPlayer &&
                    transport.ResolveConnection(actor->GetFormID()).has_value();
                if (!isPlayer && !isProxy) {
                    continue;
                }

                const auto actorID = actor->GetFormID();
                _actorThreads[actorID] = threadID;

                const auto chunks =
                    RaceMenuOverlayBridge::GetSingleton()
                        .CaptureMarkedOverlayChunks(
                            actor,
                            kMarker,
                            2200);
                const auto signature = BuildSignature(chunks);

                auto it = _overlaySignatures.find(actorID);
                const bool changed =
                    it == _overlaySignatures.end() ||
                    it->second != signature;
                _overlaySignatures[actorID] = signature;

                if (changed && !chunks.empty()) {
                    // OCumStateSync runs immediately before this tick. Its old
                    // proxy path may have queued a delayed hard reinstall. This
                    // light queue advances SKEEOverlayRefresh's per-actor
                    // generation immediately, coalescing that obsolete hard
                    // pass before it executes.
                    ApplyDirect(actor, chunks, "IMMEDIATE");
                    ScheduleActorReplay(actorID, threadID);
                }
            }
        }

        for (const auto threadID : staleThreads) {
            _activeThreads.erase(threadID);
            for (auto it = _actorThreads.begin(); it != _actorThreads.end();) {
                if (it->second == threadID) {
                    _overlaySignatures.erase(it->first);
                    _actorGenerations[it->first] = _nextGeneration++;
                    it = _actorThreads.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}
