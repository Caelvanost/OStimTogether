#include "PCH.h"
#include "OCumOverlayRelink.h"

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
        constexpr std::string_view kCancelHardReason =
            "OCUM-NATIVE-RELINK-CANCEL-HARD";
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr auto kReapplyDelay1 = std::chrono::milliseconds(100);
        constexpr auto kReapplyDelay2 = std::chrono::milliseconds(400);
        constexpr std::uint32_t kBodyMask = 4;

        namespace SKEE
        {
            using u32 = std::uint32_t;

            class IPluginInterface
            {
            public:
                virtual ~IPluginInterface() = default;
                virtual u32 GetVersion() = 0;
                virtual void Revert() = 0;
            };

            class IInterfaceMap
            {
            public:
                virtual IPluginInterface* QueryInterface(const char* name) = 0;
                virtual bool AddInterface(const char* name, IPluginInterface*) = 0;
                virtual IPluginInterface* RemoveInterface(const char* name) = 0;
            };

            struct InterfaceExchangeMessage
            {
                static constexpr std::uint32_t kMessageExchangeInterface =
                    0x9E3779B9;
                IInterfaceMap* interfaceMap{ nullptr };
            };

            class IOverlayInterface : public IPluginInterface
            {
            public:
                virtual bool HasOverlays(RE::TESObjectREFR* reference) = 0;
                virtual void AddOverlays(
                    RE::TESObjectREFR* reference,
                    bool immediate = false) = 0;
                virtual void RemoveOverlays(
                    RE::TESObjectREFR* reference,
                    bool immediate = false) = 0;
                virtual void RevertOverlays(
                    RE::TESObjectREFR* reference,
                    bool resetDiffuse,
                    bool immediate = false) = 0;
                virtual void RevertOverlay(
                    RE::TESObjectREFR* reference,
                    const char* nodeName,
                    u32 armorMask,
                    u32 addonMask,
                    bool resetDiffuse,
                    bool immediate = false) = 0;
            };
        }

        SKEE::IOverlayInterface* QueryOverlayInterface()
        {
            auto* messaging = SKSE::GetMessagingInterface();
            if (!messaging) {
                return nullptr;
            }

            SKEE::InterfaceExchangeMessage exchange{};
            if (!messaging->Dispatch(
                    SKEE::InterfaceExchangeMessage::kMessageExchangeInterface,
                    &exchange,
                    sizeof(exchange),
                    "skee") ||
                !exchange.interfaceMap) {
                return nullptr;
            }

            return static_cast<SKEE::IOverlayInterface*>(
                exchange.interfaceMap->QueryInterface("Overlay"));
        }

        int HexNibble(char ch)
        {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
            if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
            return -1;
        }

        std::optional<std::string> HexDecode(std::string_view value)
        {
            if ((value.size() % 2) != 0) {
                return std::nullopt;
            }

            std::string out;
            out.reserve(value.size() / 2);
            for (std::size_t i = 0; i < value.size(); i += 2) {
                const int hi = HexNibble(value[i]);
                const int lo = HexNibble(value[i + 1]);
                if (hi < 0 || lo < 0) {
                    return std::nullopt;
                }
                out.push_back(static_cast<char>((hi << 4) | lo));
            }
            return out;
        }

        std::vector<std::string_view> SplitView(
            std::string_view text,
            char delimiter)
        {
            std::vector<std::string_view> result;
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto pos = text.find(delimiter, start);
                if (pos == std::string_view::npos) {
                    result.push_back(text.substr(start));
                    break;
                }
                result.push_back(text.substr(start, pos - start));
                start = pos + 1;
            }
            return result;
        }

        bool IsBodyOverlayNode(std::string_view node)
        {
            return node.starts_with("Body [Ovl") ||
                   node.starts_with("Body [SOvl");
        }

        std::vector<std::string> DecodeBodyOverlayNodes(
            const std::vector<std::string>& chunks)
        {
            std::unordered_set<std::string> unique;

            for (const auto& chunk : chunks) {
                for (const auto token : SplitView(chunk, ';')) {
                    if (token.empty()) {
                        continue;
                    }

                    const auto fields = SplitView(token, ',');
                    if (fields.size() < 2) {
                        continue;
                    }

                    const auto decoded = HexDecode(fields[1]);
                    if (!decoded || !IsBodyOverlayNode(*decoded)) {
                        continue;
                    }
                    unique.insert(*decoded);
                }
            }

            std::vector<std::string> nodes(unique.begin(), unique.end());
            std::sort(nodes.begin(), nodes.end());
            return nodes;
        }

        RE::NiAVObject* FindSceneObject(
            RE::NiAVObject* object,
            std::string_view wantedName)
        {
            if (!object || wantedName.empty()) {
                return nullptr;
            }

            const char* rawName = object->name.c_str();
            if (rawName && std::string_view(rawName) == wantedName) {
                return object;
            }

            if (auto* node = object->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (!child) {
                        continue;
                    }
                    if (auto* found = FindSceneObject(
                            child.get(),
                            wantedName)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        void LogBinding(
            RE::Actor* actor,
            const std::vector<std::string>& nodes,
            std::string_view phase,
            std::uint64_t generation)
        {
            if (!actor || nodes.empty()) {
                return;
            }

            auto* root = actor->Get3D();
            auto* object = root ? FindSceneObject(root, nodes.front()) : nullptr;
            auto* geometry = object ? object->AsGeometry() : nullptr;
            if (!geometry) {
                SKSE::log::info(
                    "OSTNET OCUM NATIVE RELINK CHECK phase={} actor={:08X} player={} node=\"{}\" geometry=0 generation={}",
                    phase,
                    actor->GetFormID(),
                    actor->IsPlayerRef() ? 1 : 0,
                    nodes.front(),
                    generation);
                return;
            }

            auto& runtime = geometry->GetGeometryRuntimeData();
            auto* skin = runtime.skinInstance.get();
            auto* partition = skin ? skin->skinPartition.get() : nullptr;

            SKSE::log::info(
                "OSTNET OCUM NATIVE RELINK CHECK phase={} actor={:08X} player={} node=\"{}\" geometry=1 vertices={} matrices={} skin={} generation={}",
                phase,
                actor->GetFormID(),
                actor->IsPlayerRef() ? 1 : 0,
                nodes.front(),
                partition ? partition->vertexCount : 0,
                skin ? skin->numMatrices : 0,
                skin ? 1 : 0,
                generation);
        }
    }

    OCumOverlayRelink& OCumOverlayRelink::GetSingleton()
    {
        static OCumOverlayRelink singleton;
        return singleton;
    }

    void OCumOverlayRelink::StartListener::listen(OStim::Thread* thread)
    {
        OCumOverlayRelink::GetSingleton().HandleStart(thread);
    }

    void OCumOverlayRelink::StopListener::listen(OStim::Thread* thread)
    {
        OCumOverlayRelink::GetSingleton().HandleStop(thread);
    }

    bool OCumOverlayRelink::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || !data->LookupModByName("OCum.esp")) {
            SKSE::log::info(
                "OSTNET OCUM NATIVE RELINK disabled reason=OCum-not-installed");
            return false;
        }

        if (!QueryOverlayInterface()) {
            SKSE::log::warn(
                "OSTNET OCUM NATIVE RELINK unavailable reason=RaceMenu-Overlay-interface");
            return false;
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
                "OSTNET OCUM NATIVE RELINK unavailable reason=OStim-interface-exchange");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM NATIVE RELINK unavailable reason=Threads-interface");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM NATIVE RELINK READY pollMs={} reapply={}+{} bodyMask={} addonMask={} resetDiffuse=0 deferred=1 scope=free+furniture",
            kPollInterval.count(),
            kReapplyDelay1.count(),
            kReapplyDelay2.count(),
            kBodyMask,
            kBodyMask);
        return true;
    }

    void OCumOverlayRelink::HandleStart(OStim::Thread* thread)
    {
        if (!thread) {
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

            // Force a native relink even when an OCum overlay persists from a
            // previous scene. The body source may have been rebuilt between
            // scenes while RaceMenu kept the same serialized Body [OvlN].
            _overlaySignatures[actor->GetFormID()] = "0|";
            _actorGeneration.erase(actor->GetFormID());
        }

        SKSE::log::info(
            "OSTNET OCUM NATIVE RELINK START thread={} actors={} baseline=empty",
            threadID,
            thread->getActorCount());
    }

    void OCumOverlayRelink::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        _activeThreads.erase(thread->getThreadID());
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }
            _overlaySignatures.erase(actor->GetFormID());
            _actorGeneration.erase(actor->GetFormID());
        }
    }

    std::string OCumOverlayRelink::BuildSignature(
        const std::vector<std::string>& chunks)
    {
        std::string signature = fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:{}|", chunk.size(), chunk);
        }
        return signature;
    }

    void OCumOverlayRelink::RelinkActor(
        RE::Actor* actor,
        const std::vector<std::string>& chunks,
        std::int32_t threadID,
        std::string_view trigger)
    {
        if (!actor || chunks.empty()) {
            return;
        }

        const auto actorID = actor->GetFormID();
        const bool isPlayer = actor->IsPlayerRef();
        const bool isProxy =
            !isPlayer &&
            STRPMTransport::GetSingleton().ResolveConnection(actorID).has_value();
        if (!isPlayer && !isProxy) {
            return;
        }

        auto* overlay = QueryOverlayInterface();
        if (!overlay || !overlay->HasOverlays(actor)) {
            return;
        }

        const auto nodes = DecodeBodyOverlayNodes(chunks);
        if (nodes.empty()) {
            SKSE::log::warn(
                "OSTNET OCUM NATIVE RELINK skipped trigger={} thread={} actor={:08X} reason=no-body-nodes chunks={}",
                trigger,
                threadID,
                actorID,
                chunks.size());
            return;
        }

        const auto generation = _nextGeneration++;
        _actorGeneration[actorID] = generation;

        LogBinding(actor, nodes, "BEFORE", generation);

        // Use RaceMenu's own public overlay repair path. RevertOverlay with
        // resetDiffuse=false preserves the OCum diffuse while selecting the
        // actor's CURRENT skin form and armor addon, then RaceMenu resets the
        // existing Body [OvlN] against that live source. The call is deferred
        // through RaceMenu's task system; unlike the retired 0.35.2 experiment,
        // OStim Together never clones or writes NiSkinInstance directly.
        for (const auto& node : nodes) {
            overlay->RevertOverlay(
                actor,
                node.c_str(),
                kBodyMask,
                kBodyMask,
                false,
                false);
        }

        // OCumStateSync can queue the historical proxy hard reinstall in the
        // same keepalive tick. Advance the SKEE generation immediately with a
        // LIGHT reason so RemoveOverlays/AddOverlays cannot run afterward.
        SKEEOverlayRefresh::Queue(actor, kCancelHardReason);

        ScheduleMaterialReapply(
            actorID,
            generation,
            kReapplyDelay1,
            "T100");
        ScheduleMaterialReapply(
            actorID,
            generation,
            kReapplyDelay2,
            "T400");

        SKSE::log::info(
            "OSTNET OCUM NATIVE RELINK trigger={} thread={} actor={:08X} player={} proxy={} chunks={} nodes={} bodyMask={} addonMask={} resetDiffuse=0 generation={} action=RaceMenu-RevertOverlay",
            trigger,
            threadID,
            actorID,
            isPlayer ? 1 : 0,
            isProxy ? 1 : 0,
            chunks.size(),
            nodes.size(),
            kBodyMask,
            kBodyMask,
            generation);
    }

    void OCumOverlayRelink::ScheduleMaterialReapply(
        RE::FormID actorID,
        std::uint64_t generation,
        std::chrono::milliseconds delay,
        std::string_view phase)
    {
        const std::string phaseCopy(phase);
        std::thread(
            [actorID, generation, delay, phaseCopy]() {
                std::this_thread::sleep_for(delay);

                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    return;
                }

                tasks->AddTask(
                    [actorID, generation, phaseCopy]() {
                        auto& self = OCumOverlayRelink::GetSingleton();
                        const auto generationIt =
                            self._actorGeneration.find(actorID);
                        if (generationIt == self._actorGeneration.end() ||
                            generationIt->second != generation) {
                            return;
                        }

                        auto* form = RE::TESForm::LookupByID(actorID);
                        auto* actor = form ? form->As<RE::Actor>() : nullptr;
                        if (!actor) {
                            return;
                        }

                        const bool isPlayer = actor->IsPlayerRef();
                        const bool isProxy =
                            !isPlayer &&
                            STRPMTransport::GetSingleton()
                                .ResolveConnection(actorID).has_value();
                        if (!isPlayer && !isProxy) {
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

                        const auto nodes = DecodeBodyOverlayNodes(chunks);

                        if (isPlayer) {
                            RaceMenuOverlayBridge::GetSingleton()
                                .RefreshLocalOverlayGeometry(
                                    actor,
                                    kChannel,
                                    chunks);
                        } else {
                            for (const auto& chunk : chunks) {
                                RaceMenuOverlayBridge::GetSingleton()
                                    .ApplyRemoteOverlayChunk(
                                        actor,
                                        kChannel,
                                        chunk);
                            }
                        }

                        // Light ActorUpdateManager pass only. Because this
                        // reason is not OCUM-OVERLAY-CHANGED, the proxy-specific
                        // RemoveOverlays/AddOverlays branch is never selected.
                        SKEEOverlayRefresh::Queue(
                            actor,
                            isPlayer ?
                                "OCUM-NATIVE-RELINK-PLAYER" :
                                "OCUM-NATIVE-RELINK-PROXY");

                        LogBinding(actor, nodes, phaseCopy, generation);
                        SKSE::log::info(
                            "OSTNET OCUM NATIVE RELINK REAPPLY phase={} actor={:08X} player={} proxy={} chunks={} nodes={} generation={}",
                            phaseCopy,
                            actorID,
                            isPlayer ? 1 : 0,
                            isProxy ? 1 : 0,
                            chunks.size(),
                            nodes.size(),
                            generation);
                    });
            })
            .detach();
    }

    void OCumOverlayRelink::Tick()
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
            if (!thread) {
                staleThreads.push_back(threadID);
                continue;
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor) {
                    continue;
                }

                const auto actorID = actor->GetFormID();
                const bool isPlayer = actor->IsPlayerRef();
                const bool isProxy =
                    !isPlayer && transport.ResolveConnection(actorID).has_value();
                if (!isPlayer && !isProxy) {
                    continue;
                }

                const auto chunks =
                    RaceMenuOverlayBridge::GetSingleton()
                        .CaptureMarkedOverlayChunks(
                            actor,
                            kMarker,
                            2200);
                const auto signature = BuildSignature(chunks);
                auto [it, inserted] =
                    _overlaySignatures.try_emplace(actorID, "0|");

                if (it->second == signature) {
                    continue;
                }

                it->second = signature;
                if (chunks.empty()) {
                    // Invalidate any delayed material pass from the previous
                    // state when OCum clears its body overlays.
                    _actorGeneration[actorID] = _nextGeneration++;
                    continue;
                }

                RelinkActor(
                    actor,
                    chunks,
                    threadID,
                    inserted ? "INITIAL" : "CHANGED");
            }
        }

        for (const auto threadID : staleThreads) {
            _activeThreads.erase(threadID);
        }
    }
}
