#include "PCH.h"
#include "OCumStateSync.h"

#include "RaceMenuOverlayBridge.h"
#include "SKEEOverlayRefresh.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kChannel = "ocum";
        constexpr std::string_view kMarker = "CumOverlays";
        constexpr std::string_view kVaginalObject = "ocumvagmesh";
        constexpr std::string_view kAnalObject = "ocumanmesh";
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr auto kVisualRefreshInterval = std::chrono::milliseconds(500);
        constexpr auto kOverlayPollInterval = std::chrono::milliseconds(500);
        constexpr auto kStopSnapshotDelay = std::chrono::milliseconds(700);

        bool IsArmorWorn(RE::Actor* actor, RE::TESObjectARMO* armor)
        {
            if (!actor || !armor) {
                return false;
            }

            const auto inventory = actor->GetInventory();
            const auto it = inventory.find(armor);
            return it != inventory.end() &&
                   it->second.second &&
                   it->second.second->IsWorn();
        }

        bool QueueNiNodeUpdate(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* skyrimVM = RE::SkyrimVM::GetSingleton();
            if (!skyrimVM || !skyrimVM->impl) {
                return false;
            }

            RE::BSTSmartPointer<
                RE::BSScript::IStackCallbackFunctor> callback;
            auto args = RE::MakeFunctionArguments();
            const auto handle =
                skyrimVM->handlePolicy.GetHandleForObject(
                    static_cast<RE::VMTypeID>(RE::Actor::FORMTYPE),
                    actor);

            return skyrimVM->impl->DispatchMethodCall2(
                handle,
                "Actor",
                "QueueNiNodeUpdate",
                args,
                callback);
        }
    }

    OCumStateSync& OCumStateSync::GetSingleton()
    {
        static OCumStateSync instance;
        return instance;
    }

    void OCumStateSync::StartListener::listen(OStim::Thread* thread)
    {
        OCumStateSync::GetSingleton().HandleStart(thread);
    }

    void OCumStateSync::StopListener::listen(OStim::Thread* thread)
    {
        OCumStateSync::GetSingleton().HandleStop(thread);
    }

    bool OCumStateSync::Initialize()
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
                "OSTNET OCUM LIVE unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM LIVE unavailable: Threads interface missing");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM LIVE READY pollMs={} visualRefreshMs={} overlayPollMs={} authority=real-local-player meshSource=worn-OCum-armor overlayRepair=ActorUpdateManager-on-change",
            kPollInterval.count(),
            kVisualRefreshInterval.count(),
            kOverlayPollInterval.count());
        return true;
    }

    void OCumStateSync::Reset()
    {
        _activeThreads.clear();
        _meshStates.clear();
        _nextPoll = {};
    }

    void OCumStateSync::HandleStart(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || !data->LookupModByName("OCum.esp")) {
            return;
        }

        _activeThreads.insert(thread->getThreadID());

        // Force a fresh baseline for every participant. If a persistent OCum
        // mesh or overlay already exists before START, the first live poll will
        // see it as a new state for this scene and materialize it through the
        // normal RaceMenu update pipeline.
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ?
                static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                _meshStates.erase(actor->GetFormID());
            }
        }

        SKSE::log::info(
            "OSTNET OCUM LIVE START thread={} actors={}",
            thread->getThreadID(),
            thread->getActorCount());
    }

    void OCumStateSync::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        bool hadLocalPlayer = false;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ?
                static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            hadLocalPlayer = hadLocalPlayer || actor->IsPlayerRef();
            _meshStates.erase(actor->GetFormID());
        }

        _activeThreads.erase(threadID);

        SKSE::log::info(
            "OSTNET OCUM LIVE STOP thread={} localPlayer={} delayedSnapshotMs={}",
            threadID,
            hadLocalPlayer ? 1 : 0,
            hadLocalPlayer ? kStopSnapshotDelay.count() : 0);

        if (hadLocalPlayer) {
            std::thread([]() {
                std::this_thread::sleep_for(kStopSnapshotDelay);
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() {
                        OCumStateSync::GetSingleton().
                            SendLocalSnapshot("ostim-stop-live");
                    });
                }
            }).detach();
        }
    }

    std::string OCumStateSync::HexEncode(std::string_view value)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 2);
        for (const auto ch : value) {
            const auto u = static_cast<unsigned char>(ch);
            out.push_back(kHex[(u >> 4) & 0x0F]);
            out.push_back(kHex[u & 0x0F]);
        }
        return out;
    }

    std::string OCumStateSync::BuildOverlaySignature(
        const std::vector<std::string>& chunks)
    {
        std::string signature;
        std::size_t reserve = 32;
        for (const auto& chunk : chunks) {
            reserve += chunk.size() + 24;
        }
        signature.reserve(reserve);
        signature += fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:", chunk.size());
            signature += chunk;
            signature += '|';
        }
        return signature;
    }

    void OCumStateSync::SendLocalObjectState(
        RE::PlayerCharacter* player,
        std::string_view type,
        bool equipped,
        std::string_view reason)
    {
        if (!player) {
            return;
        }

        const char* rawName = player->GetName();
        const std::string name = rawName ? rawName : "";
        if (name.empty()) {
            return;
        }

        STRPMTransport::GetSingleton().Send(
            fmt::format(
                "ADDONOBJ|channel={}|name={}|type={}|equipped={}",
                HexEncode(kChannel),
                HexEncode(name),
                HexEncode(type),
                equipped ? 1 : 0));

        SKSE::log::info(
            "OSTNET OCUM LIVE OBJ TX reason={} actor={:08X} type={} equipped={}",
            reason,
            player->GetFormID(),
            type,
            equipped ? 1 : 0);
    }

    void OCumStateSync::Tick()
    {
        if (!_threads || _activeThreads.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_nextPoll.time_since_epoch().count() != 0 && now < _nextPoll) {
            return;
        }
        _nextPoll = now + kPollInterval;

        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || !data->LookupModByName("OCum.esp")) {
            return;
        }

        auto* vaginal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F37,
            "OCum.esp");
        auto* anal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F3B,
            "OCum.esp");
        if (!vaginal && !anal) {
            return;
        }

        std::vector<std::int32_t> staleThreads;
        std::unordered_set<RE::FormID> activeActors;
        auto& transport = STRPMTransport::GetSingleton();

        for (const auto threadID : _activeThreads) {
            auto* thread = _threads->getThread(threadID);
            if (!thread) {
                staleThreads.push_back(threadID);
                continue;
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ?
                    static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor) {
                    continue;
                }

                const auto actorID = actor->GetFormID();
                if (!activeActors.insert(actorID).second) {
                    continue;
                }

                const bool vaginalWorn = IsArmorWorn(actor, vaginal);
                const bool analWorn = IsArmorWorn(actor, anal);
                auto& state = _meshStates[actorID];

                // OCum/RaceMenu overlay data can be correct while the live 3D
                // holder is stale after an OStim body rebuild. Watch the actual
                // CumOverlays override snapshot for every participant (real
                // player, remote STR proxy, NPC) and request RaceMenu's own
                // overlay+node update only when that snapshot changes.
                const bool overlayPollDue =
                    state.lastOverlayPoll.time_since_epoch().count() == 0 ||
                    now - state.lastOverlayPoll >= kOverlayPollInterval;

                if (overlayPollDue) {
                    const auto chunks =
                        RaceMenuOverlayBridge::GetSingleton()
                            .CaptureMarkedOverlayChunks(
                                actor,
                                kMarker,
                                2200);
                    const auto signature = BuildOverlaySignature(chunks);
                    const bool firstOverlayPoll = !state.overlayInitialized;
                    const bool overlayChanged =
                        state.overlayInitialized &&
                        signature != state.overlaySignature;
                    const bool initialOverlayPresent =
                        firstOverlayPoll && !chunks.empty();

                    if (overlayChanged || initialOverlayPresent) {
                        SKEEOverlayRefresh::Queue(
                            actor,
                            initialOverlayPresent ?
                                "OCUM-OVERLAY-INITIAL" :
                                "OCUM-OVERLAY-CHANGED");

                        SKSE::log::info(
                            "OSTNET OCUM OVERLAY CHANGE thread={} actor={:08X} player={} chunks={} first={} action=queue-racemenu-update",
                            threadID,
                            actorID,
                            actor->IsPlayerRef() ? 1 : 0,
                            chunks.size(),
                            firstOverlayPoll ? 1 : 0);
                    }

                    state.overlayInitialized = true;
                    state.overlaySignature = signature;
                    state.lastOverlayPoll = now;
                }

                const bool first = !state.initialized;
                const bool vaginalChanged =
                    state.initialized && state.vaginal != vaginalWorn;
                const bool analChanged =
                    state.initialized && state.anal != analWorn;
                const bool vaginalBecameWorn =
                    vaginalWorn && (first || !state.vaginal);
                const bool analBecameWorn =
                    analWorn && (first || !state.anal);

                if (actor->IsPlayerRef()) {
                    auto* player = static_cast<RE::PlayerCharacter*>(actor);

                    // Initial false was already covered by consent/start
                    // snapshots. A pre-existing true mesh, or any later live
                    // transition, must be published immediately so a stale
                    // cached false cannot keep the remote proxy unequipped.
                    if ((first && vaginalWorn) || vaginalChanged) {
                        SendLocalObjectState(
                            player,
                            kVaginalObject,
                            vaginalWorn,
                            first ? "active-start" : "live-change");
                    }
                    if ((first && analWorn) || analChanged) {
                        SendLocalObjectState(
                            player,
                            kAnalObject,
                            analWorn,
                            first ? "active-start" : "live-change");
                    }
                }

                const bool isSTRProxy =
                    !actor->IsPlayerRef() &&
                    transport.ResolveConnection(actorID).has_value();

                const bool refreshDue =
                    (vaginalWorn || analWorn) &&
                    !isSTRProxy &&
                    (state.lastVisualRefresh.time_since_epoch().count() == 0 ||
                     now - state.lastVisualRefresh >= kVisualRefreshInterval);

                if (refreshDue) {
                    const bool queued = QueueNiNodeUpdate(actor);
                    state.lastVisualRefresh = now;

                    SKSE::log::info(
                        "OSTNET OCUM LIVE 3D REFRESH thread={} actor={:08X} player={} vagMesh={} analMesh={} queued={} reason={} intervalMs={}",
                        threadID,
                        actorID,
                        actor->IsPlayerRef() ? 1 : 0,
                        vaginalWorn ? 1 : 0,
                        analWorn ? 1 : 0,
                        queued ? 1 : 0,
                        (vaginalBecameWorn || analBecameWorn) ?
                            "mesh-became-worn" : "mesh-still-worn",
                        kVisualRefreshInterval.count());
                }

                if (first || vaginalChanged || analChanged) {
                    SKSE::log::info(
                        "OSTNET OCUM LIVE STATE thread={} actor={:08X} player={} proxy={} first={} vagMesh={} analMesh={} vagChanged={} analChanged={}",
                        threadID,
                        actorID,
                        actor->IsPlayerRef() ? 1 : 0,
                        isSTRProxy ? 1 : 0,
                        first ? 1 : 0,
                        vaginalWorn ? 1 : 0,
                        analWorn ? 1 : 0,
                        vaginalChanged ? 1 : 0,
                        analChanged ? 1 : 0);
                }

                state.initialized = true;
                state.vaginal = vaginalWorn;
                state.anal = analWorn;
            }
        }

        for (const auto threadID : staleThreads) {
            _activeThreads.erase(threadID);
        }

        for (auto it = _meshStates.begin(); it != _meshStates.end();) {
            if (!activeActors.contains(it->first)) {
                it = _meshStates.erase(it);
            } else {
                ++it;
            }
        }
    }

    void OCumStateSync::SendLocalSnapshot(std::string_view reason)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!data || !player || !data->LookupModByName("OCum.esp")) {
            return;
        }

        const char* rawName = player->GetName();
        const std::string name = rawName ? rawName : "";
        if (name.empty()) {
            return;
        }

        const auto chunks = RaceMenuOverlayBridge::GetSingleton()
            .CaptureMarkedOverlayChunks(player, kMarker, 2200);

        for (std::size_t i = 0; i < chunks.size(); ++i) {
            STRPMTransport::GetSingleton().Send(
                fmt::format(
                    "ADDONOVR|channel={}|name={}|seq={}|count={}|props={}",
                    HexEncode(kChannel),
                    HexEncode(name),
                    i,
                    chunks.size(),
                    chunks[i]));
        }

        auto* vaginal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F37,
            "OCum.esp");
        auto* anal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F3B,
            "OCum.esp");

        const bool vaginalEquipped = IsArmorWorn(player, vaginal);
        const bool analEquipped = IsArmorWorn(player, anal);

        SendLocalObjectState(
            player,
            kVaginalObject,
            vaginalEquipped,
            reason);
        SendLocalObjectState(
            player,
            kAnalObject,
            analEquipped,
            reason);

        SKSE::log::info(
            "OSTNET OCUM SNAPSHOT TX reason={} actor={:08X} name=\"{}\" overlayChunks={} vagMesh={} analMesh={}",
            reason,
            player->GetFormID(),
            name,
            chunks.size(),
            vaginalEquipped ? 1 : 0,
            analEquipped ? 1 : 0);
    }
}
