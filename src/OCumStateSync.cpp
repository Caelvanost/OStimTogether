#include "PCH.h"
#include "OCumStateSync.h"

#include "OCumOverlayVisibility.h"
#include "RaceMenuOverlayBridge.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

#include <RE/I/IFunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>
#include <RE/V/Variable.h>

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kChannel = "ocum";
        constexpr std::string_view kMarker = "CumOverlays";
        constexpr std::string_view kVaginalObject = "ocumvagmesh";
        constexpr std::string_view kAnalObject = "ocumanmesh";
        constexpr auto kPollInterval = std::chrono::milliseconds(250);
        constexpr auto k3DDiagnosticInterval = std::chrono::milliseconds(250);
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

        std::string ActorDiagnosticRole(RE::Actor* actor)
        {
            if (!actor) {
                return "missing";
            }
            if (actor->IsPlayerRef()) {
                return "local-player";
            }
            if ((actor->GetFormID() & 0xFF000000u) == 0xFF000000u) {
                return "dynamic-proxy-or-temp";
            }
            return "npc-or-scene-actor";
        }

        class ActorStringArguments final :
            public RE::BSScript::IFunctionArguments
        {
        public:
            ActorStringArguments(
                RE::Actor* actor,
                std::string_view text) :
                _actor(actor),
                _text(text)
            {}

            bool operator()(
                RE::BSScrapArray<RE::BSScript::Variable>& dst)
                const override
            {
                if (!_actor || _text.empty()) {
                    return false;
                }

                auto actor = _actor;
                auto text = _text;

                RE::BSScript::Variable actorArg;
                actorArg.Pack<RE::Actor*>(std::move(actor));

                RE::BSScript::Variable textArg;
                textArg.Pack<std::string>(std::move(text));

                dst.push_back(std::move(actorArg));
                dst.push_back(std::move(textArg));
                return true;
            }

        private:
            RE::Actor* _actor{ nullptr };
            std::string _text;
        };

        class OCum3DQueryCallback final :
            public RE::BSScript::IStackCallbackFunctor
        {
        public:
            OCum3DQueryCallback(
                std::int32_t threadID,
                RE::FormID actorID,
                std::string actorName,
                std::string role,
                std::string objectType,
                bool backingArmorWorn,
                std::string phase) :
                _threadID(threadID),
                _actorID(actorID),
                _actorName(std::move(actorName)),
                _role(std::move(role)),
                _objectType(std::move(objectType)),
                _backingArmorWorn(backingArmorWorn),
                _phase(std::move(phase))
            {}

            void operator()(RE::BSScript::Variable result) override
            {
                if (!result.IsBool()) {
                    SKSE::log::warn(
                        "OSTNET OCUM 3D DIAG thread={} phase={} actor={:08X} name=\"{}\" role={} type={} backingArmorWorn={} ostimEquipped=non-bool",
                        _threadID,
                        _phase,
                        _actorID,
                        _actorName,
                        _role,
                        _objectType,
                        _backingArmorWorn ? 1 : 0);
                    return;
                }

                SKSE::log::info(
                    "OSTNET OCUM 3D DIAG thread={} phase={} actor={:08X} name=\"{}\" role={} type={} backingArmorWorn={} ostimEquipped={}",
                    _threadID,
                    _phase,
                    _actorID,
                    _actorName,
                    _role,
                    _objectType,
                    _backingArmorWorn ? 1 : 0,
                    result.GetBool() ? 1 : 0);
            }

            bool CanSave() const override
            {
                return false;
            }

            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>&) override
            {}

        private:
            std::int32_t _threadID{ -1 };
            RE::FormID _actorID{ 0 };
            std::string _actorName;
            std::string _role;
            std::string _objectType;
            bool _backingArmorWorn{ false };
            std::string _phase;
        };

        bool DispatchOCum3DQuery(
            std::int32_t threadID,
            RE::Actor* actor,
            std::string_view objectType,
            bool backingArmorWorn,
            std::string_view phase)
        {
            if (!actor || objectType.empty()) {
                return false;
            }

            auto* skyrimVM = RE::SkyrimVM::GetSingleton();
            if (!skyrimVM || !skyrimVM->impl) {
                SKSE::log::warn(
                    "OSTNET OCUM 3D DIAG dispatch-failed thread={} phase={} actor={:08X} type={} reason=skyrim-vm-unavailable",
                    threadID,
                    phase,
                    actor->GetFormID(),
                    objectType);
                return false;
            }

            const char* rawName = actor->GetName();
            const std::string actorName = rawName ? rawName : "";
            const auto role = ActorDiagnosticRole(actor);

            ActorStringArguments args(actor, objectType);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(
                new OCum3DQueryCallback(
                    threadID,
                    actor->GetFormID(),
                    actorName,
                    role,
                    std::string(objectType),
                    backingArmorWorn,
                    std::string(phase)));

            const bool dispatched = skyrimVM->impl->DispatchStaticCall(
                RE::BSFixedString("OActor"),
                RE::BSFixedString("IsObjectEquipped"),
                &args,
                callback);

            if (!dispatched) {
                SKSE::log::warn(
                    "OSTNET OCUM 3D DIAG dispatch-failed thread={} phase={} actor={:08X} name=\"{}\" role={} type={} backingArmorWorn={} reason=papyrus-dispatch-rejected",
                    threadID,
                    phase,
                    actor->GetFormID(),
                    actorName,
                    role,
                    objectType,
                    backingArmorWorn ? 1 : 0);
            }

            return dispatched;
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
            "OSTNET OCUM LIVE READY pollMs={} mode=mirror-only localOverlayWrites=0 localNiNodeRefresh=0 liveVisibilitySync=1 mesh3DSync=disabled diagnostic3D=read-only diagnosticMs={}",
            kPollInterval.count(),
            k3DDiagnosticInterval.count());
        return true;
    }

    void OCumStateSync::Reset()
    {
        _activeThreads.clear();
        _meshStates.clear();
        _nextPoll = {};
        _next3DDiagnostic = {};
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

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor && actor->IsPlayerRef()) {
                _meshStates.erase(actor->GetFormID());
            }
        }

        SKSE::log::info(
            "OSTNET OCUM LIVE START thread={} actors={} mode=mirror-only mesh3DSync=disabled diagnostic3D=read-only",
            thread->getThreadID(),
            thread->getActorCount());

        RunThread3DDiagnostics(thread, "start");
        _next3DDiagnostic =
            std::chrono::steady_clock::now() + k3DDiagnosticInterval;
    }

    void OCumStateSync::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        bool hadLocalPlayer = false;

        // Query one final time while OStim still hands us the thread object.
        RunThread3DDiagnostics(thread, "stop");

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor || !actor->IsPlayerRef()) {
                continue;
            }
            hadLocalPlayer = true;
            _meshStates.erase(actor->GetFormID());
        }

        _activeThreads.erase(threadID);

        SKSE::log::info(
            "OSTNET OCUM LIVE STOP thread={} localPlayer={} delayedSnapshotMs={} mode=mirror-only mesh3DSync=disabled diagnostic3D=read-only",
            threadID,
            hadLocalPlayer ? 1 : 0,
            hadLocalPlayer ? kStopSnapshotDelay.count() : 0);

        if (hadLocalPlayer) {
            std::thread([]() {
                std::this_thread::sleep_for(kStopSnapshotDelay);
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() {
                        OCumStateSync::GetSingleton().SendLocalSnapshot(
                            "ostim-stop-live");
                    });
                }
            }).detach();
        }
    }

    void OCumStateSync::RunThread3DDiagnostics(
        OStim::Thread* thread,
        std::string_view phase)
    {
        if (!thread) {
            return;
        }

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

        const auto threadID = thread->getThreadID();
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            const bool vaginalWorn = IsArmorWorn(actor, vaginal);
            const bool analWorn = IsArmorWorn(actor, anal);

            DispatchOCum3DQuery(
                threadID,
                actor,
                kVaginalObject,
                vaginalWorn,
                phase);
            DispatchOCum3DQuery(
                threadID,
                actor,
                kAnalObject,
                analWorn,
                phase);
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
        std::string signature = fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:{}|", chunk.size(), chunk);
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

        // Network publication remains deliberately disabled. v0.37.6 only
        // observes local OStim/backing-armor state on all scene actors so we can
        // determine whether OCum itself creates a hidden equip-object state on
        // an STR proxy. CumOverlays continue through the supported ADDONOVR path.
        SKSE::log::trace(
            "OSTNET OCUM LIVE OBJ SUPPRESSED reason={} actor={:08X} type={} equipped={} diagnostic3D=read-only",
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

        const bool run3DDiagnostic =
            _next3DDiagnostic.time_since_epoch().count() == 0 ||
            now >= _next3DDiagnostic;
        if (run3DDiagnostic) {
            _next3DDiagnostic = now + k3DDiagnosticInterval;
        }

        std::vector<std::int32_t> staleThreads;
        std::unordered_set<RE::FormID> seenLocalPlayers;

        for (const auto threadID : _activeThreads) {
            auto* thread = _threads->getThread(threadID);
            if (!thread) {
                staleThreads.push_back(threadID);
                continue;
            }

            if (run3DDiagnostic) {
                RunThread3DDiagnostics(thread, "poll");
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor || !actor->IsPlayerRef()) {
                    continue;
                }

                const auto actorID = actor->GetFormID();
                if (!seenLocalPlayers.insert(actorID).second) {
                    continue;
                }

                auto* player = static_cast<RE::PlayerCharacter*>(actor);
                auto& state = _meshStates[actorID];

                // Read-only mirroring: capture the overrides OCum owns and add
                // wire-only metadata describing whether each live Body [OvlN]
                // is actually rendered. This lets visibility changes trigger a
                // snapshot even when texture/alpha overrides do not change.
                auto chunks = RaceMenuOverlayBridge::GetSingleton()
                    .CaptureMarkedOverlayChunks(player, kMarker, 2200);
                OCumOverlayVisibility::DecorateOutgoingSnapshot(
                    player,
                    chunks,
                    2200);
                const auto overlaySignature = BuildOverlaySignature(chunks);
                const bool overlayChanged =
                    !state.overlayInitialized ||
                    state.overlaySignature != overlaySignature;

                if (overlayChanged) {
                    state.overlayInitialized = true;
                    state.overlaySignature = overlaySignature;
                    SendLocalSnapshot(
                        state.initialized ? "overlay-change" : "active-start");

                    SKSE::log::info(
                        "OSTNET OCUM OVERLAY MIRROR thread={} actor={:08X} chunks={} empty={} action=send-only localOverlayWrites=0 liveVisibilitySync=1",
                        threadID,
                        actorID,
                        chunks.size(),
                        chunks.empty() ? 1 : 0);
                }

                // Keep observing the true local player's backing armor as an
                // existing compact state-change signal. It remains diagnostic
                // only and is never transmitted.
                const bool vaginalWorn = IsArmorWorn(actor, vaginal);
                const bool analWorn = IsArmorWorn(actor, anal);
                const bool first = !state.initialized;
                const bool vaginalChanged = state.initialized && state.vaginal != vaginalWorn;
                const bool analChanged = state.initialized && state.anal != analWorn;

                if (first || vaginalChanged) {
                    SendLocalObjectState(
                        player,
                        kVaginalObject,
                        vaginalWorn,
                        first ? "active-start" : "live-change");
                }
                if (first || analChanged) {
                    SendLocalObjectState(
                        player,
                        kAnalObject,
                        analWorn,
                        first ? "active-start" : "live-change");
                }

                if (first || vaginalChanged || analChanged) {
                    SKSE::log::info(
                        "OSTNET OCUM LIVE STATE thread={} actor={:08X} first={} localVagMesh={} localAnalMesh={} vagChanged={} analChanged={} localOverlayWrites=0 mesh3DSync=disabled diagnostic3D=read-only",
                        threadID,
                        actorID,
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
            if (!seenLocalPlayers.contains(it->first)) {
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

        auto chunks = RaceMenuOverlayBridge::GetSingleton()
            .CaptureMarkedOverlayChunks(player, kMarker, 2200);
        OCumOverlayVisibility::DecorateOutgoingSnapshot(
            player,
            chunks,
            2200);

        if (chunks.empty()) {
            STRPMTransport::GetSingleton().Send(
                fmt::format(
                    "ADDONOVR|channel={}|name={}|seq=0|count=1|props=",
                    HexEncode(kChannel),
                    HexEncode(name)));
        } else {
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
        }

        auto* vaginal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F37,
            "OCum.esp");
        auto* anal = data->LookupForm<RE::TESObjectARMO>(
            0x00000F3B,
            "OCum.esp");

        const bool vaginalEquipped = IsArmorWorn(player, vaginal);
        const bool analEquipped = IsArmorWorn(player, anal);

        // Kept as local diagnostics only. SendLocalObjectState intentionally
        // suppresses publication of these 3D meshes.
        SendLocalObjectState(player, kVaginalObject, vaginalEquipped, reason);
        SendLocalObjectState(player, kAnalObject, analEquipped, reason);

        SKSE::log::info(
            "OSTNET OCUM SNAPSHOT TX reason={} actor={:08X} name=\"{}\" overlayChunks={} emptySnapshot={} localVagMesh={} localAnalMesh={} localOverlayWrites=0 liveVisibilitySync=1 mesh3DSync=disabled diagnostic3D=read-only",
            reason,
            player->GetFormID(),
            name,
            chunks.size(),
            chunks.empty() ? 1 : 0,
            vaginalEquipped ? 1 : 0,
            analEquipped ? 1 : 0);
    }
}
