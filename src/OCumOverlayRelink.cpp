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
        constexpr std::string_view kCancelHardReason = "OCUM-BODY-RELINK";
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr auto kReapplyDelay1 = std::chrono::milliseconds(180);
        constexpr auto kReapplyDelay2 = std::chrono::milliseconds(650);
        constexpr std::string_view kBodyOverlay0 = "Body [Ovl0]";

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

            class IAddonAttachmentInterface
            {
            public:
                virtual ~IAddonAttachmentInterface() = default;
            };

            class IActorUpdateManager : public IPluginInterface
            {
            public:
                virtual void AddBodyUpdate(u32 formId) = 0;
                virtual void AddTransformUpdate(u32 formId) = 0;
                virtual void AddOverlayUpdate(u32 formId) = 0;
                virtual void AddNodeOverrideUpdate(u32 formId) = 0;
                virtual void AddWeaponOverrideUpdate(u32 formId) = 0;
                virtual void AddAddonOverrideUpdate(u32 formId) = 0;
                virtual void AddSkinOverrideUpdate(u32 formId) = 0;
                virtual void Flush() = 0;
                virtual void AddInterface(IAddonAttachmentInterface*) = 0;
                virtual void RemoveInterface(IAddonAttachmentInterface*) = 0;
                using FlushCallback = void (*)(u32*, u32);
                virtual bool RegisterFlushCallback(const char*, FlushCallback) = 0;
                virtual bool UnregisterFlushCallback(const char*) = 0;
            };
        }

        SKEE::IActorUpdateManager* QueryActorUpdateManager()
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

            return static_cast<SKEE::IActorUpdateManager*>(
                exchange.interfaceMap->QueryInterface("ActorUpdateManager"));
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
                    if (auto* found = FindSceneObject(child.get(), wantedName)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        void LogBodyOverlayBinding(
            RE::Actor* actor,
            std::string_view phase,
            std::uint64_t generation)
        {
            if (!actor) {
                return;
            }

            auto* root = actor->Get3D();
            auto* object = root ? FindSceneObject(root, kBodyOverlay0) : nullptr;
            auto* geometry = object ? object->AsGeometry() : nullptr;
            if (!geometry) {
                SKSE::log::info(
                    "OSTNET OCUM BODY RELINK CHECK phase={} actor={:08X} player={} geometry=0 generation={}",
                    phase,
                    actor->GetFormID(),
                    actor->IsPlayerRef() ? 1 : 0,
                    generation);
                return;
            }

            auto& runtime = geometry->GetGeometryRuntimeData();
            auto* skin = runtime.skinInstance.get();
            auto* partition = skin ? skin->skinPartition.get() : nullptr;
            const auto vertices = partition ? partition->vertexCount : 0;
            const auto matrices = skin ? skin->numMatrices : 0;

            SKSE::log::info(
                "OSTNET OCUM BODY RELINK CHECK phase={} actor={:08X} player={} geometry=1 vertices={} matrices={} skin={} generation={}",
                phase,
                actor->GetFormID(),
                actor->IsPlayerRef() ? 1 : 0,
                vertices,
                matrices,
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
                "OSTNET OCUM BODY RELINK disabled reason=OCum-not-installed");
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
                "OSTNET OCUM BODY RELINK unavailable reason=OStim-interface-exchange");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM BODY RELINK unavailable reason=Threads-interface");
            return false;
        }

        if (!QueryActorUpdateManager()) {
            SKSE::log::warn(
                "OSTNET OCUM BODY RELINK unavailable reason=RaceMenu-ActorUpdateManager");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM BODY RELINK READY pollMs={} pipeline=BodyUpdate+OverlayUpdate+NodeOverride scope=free+furniture hardProxySupersede=1",
            kPollInterval.count());
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
            _overlaySignatures[actor->GetFormID()] = "0|";
            _actorGeneration.erase(actor->GetFormID());
        }

        SKSE::log::info(
            "OSTNET OCUM BODY RELINK START thread={} actors={} baseline=empty",
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

        auto* updates = QueryActorUpdateManager();
        if (!updates) {
            return;
        }

        const auto generation = _nextGeneration++;
        _actorGeneration[actorID] = generation;

        LogBodyOverlayBinding(actor, "BEFORE", generation);

        // RaceMenu's own ActorUpdateManager Flush order is Body -> Overlay ->
        // NodeOverride. AddBodyUpdate is the missing step that forces existing
        // Body [OvlN] meshes to relink against the body geometry currently
        // attached by OStim, instead of retaining a prior scene's skin source.
        updates->AddBodyUpdate(actorID);
        updates->AddOverlayUpdate(actorID);
        updates->AddNodeOverrideUpdate(actorID);
        updates->Flush();

        // OCumStateSync may have queued the historical proxy hard-reinstall a
        // few milliseconds earlier. Queueing a non-hard SKEE generation here
        // supersedes it before its 250 ms quiet window expires.
        SKEEOverlayRefresh::Queue(actor, kCancelHardReason);

        ScheduleMaterialReapply(actorID, generation, kReapplyDelay1, "T180");
        ScheduleMaterialReapply(actorID, generation, kReapplyDelay2, "T650");

        SKSE::log::info(
            "OSTNET OCUM BODY RELINK trigger={} thread={} actor={:08X} player={} proxy={} chunks={} generation={} action=body+overlay+nodeoverride",
            trigger,
            threadID,
            actorID,
            isPlayer ? 1 : 0,
            isProxy ? 1 : 0,
            chunks.size(),
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
                        const auto generationIt = self._actorGeneration.find(actorID);
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

                        auto chunks =
                            RaceMenuOverlayBridge::GetSingleton()
                                .CaptureMarkedOverlayChunks(
                                    actor,
                                    kMarker,
                                    2200);
                        if (chunks.empty()) {
                            return;
                        }

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

                        LogBodyOverlayBinding(actor, phaseCopy, generation);
                        SKSE::log::info(
                            "OSTNET OCUM BODY RELINK REAPPLY phase={} actor={:08X} player={} proxy={} chunks={} generation={}",
                            phaseCopy,
                            actorID,
                            isPlayer ? 1 : 0,
                            isProxy ? 1 : 0,
                            chunks.size(),
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
                if (!chunks.empty()) {
                    RelinkActor(actor, chunks, threadID, "SNAPSHOT-CHANGED");
                }
            }
        }

        for (const auto threadID : staleThreads) {
            _activeThreads.erase(threadID);
        }
    }
}
