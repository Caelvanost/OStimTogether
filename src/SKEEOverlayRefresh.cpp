#include "PCH.h"
#include "SKEEOverlayRefresh.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace OStimTogether::SKEEOverlayRefresh
{
    namespace
    {
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

            // Only the leading public Overlay ABI methods used here are
            // declared. Their virtual order matches RaceMenu's
            // IPluginInterface.h exactly.
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
            };

            // Public RaceMenu/SKEE ActorUpdateManager interface. Keep virtual
            // order identical to IPluginInterface.h. This is the same update
            // pipeline RaceMenu uses after save-load and 3D rebuilds.
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
                virtual bool RegisterFlushCallback(
                    const char* key,
                    FlushCallback cb) = 0;
                virtual bool UnregisterFlushCallback(const char* key) = 0;
            };
        }

        constexpr auto kQuietDelay = std::chrono::milliseconds(250);
        constexpr auto kProxySpatialDelay1 = std::chrono::milliseconds(150);
        constexpr auto kProxySpatialDelay2 = std::chrono::milliseconds(650);
        constexpr std::string_view kOCumOverlayChanged =
            "OCUM-OVERLAY-CHANGED";
        constexpr std::string_view kBodyOverlay0 = "Body [Ovl0]";

        std::mutex g_queueMutex;
        std::unordered_map<RE::FormID, std::uint64_t> g_actorGeneration;
        std::atomic<std::uint64_t> g_nextGeneration{ 1 };

        SKEE::IInterfaceMap* QueryInterfaceMap()
        {
            auto* messaging = SKSE::GetMessagingInterface();
            if (!messaging) {
                return nullptr;
            }

            SKEE::InterfaceExchangeMessage exchange{};
            const bool dispatched = messaging->Dispatch(
                SKEE::InterfaceExchangeMessage::kMessageExchangeInterface,
                &exchange,
                sizeof(exchange),
                "skee");

            if (!dispatched || !exchange.interfaceMap) {
                return nullptr;
            }

            return exchange.interfaceMap;
        }

        SKEE::IActorUpdateManager* QueryActorUpdateManager(
            SKEE::IInterfaceMap* map)
        {
            return map ?
                static_cast<SKEE::IActorUpdateManager*>(
                    map->QueryInterface("ActorUpdateManager")) :
                nullptr;
        }

        SKEE::IOverlayInterface* QueryOverlayInterface(
            SKEE::IInterfaceMap* map)
        {
            return map ?
                static_cast<SKEE::IOverlayInterface*>(
                    map->QueryInterface("Overlay")) :
                nullptr;
        }

        bool IsGenerationCurrent(
            RE::FormID actorID,
            std::uint64_t generation)
        {
            std::scoped_lock lock(g_queueMutex);
            const auto it = g_actorGeneration.find(actorID);
            return it != g_actorGeneration.end() &&
                   it->second == generation;
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
                            child.get(), wantedName)) {
                        return found;
                    }
                }
            }

            return nullptr;
        }

        void LogOverlayGeometryDiagnostic(
            RE::Actor* actor,
            std::string_view phase,
            std::uint64_t generation)
        {
            if (!actor) {
                return;
            }

            auto* root = actor->Get3D();
            auto* object = root ?
                FindSceneObject(root, kBodyOverlay0) :
                nullptr;
            auto* geometry = object ? object->AsGeometry() : nullptr;

            if (!geometry) {
                SKSE::log::info(
                    "OSTNET SKEE OVERLAY GEOMETRY phase={} actor={:08X} player={} root={} node={} geometry=0 generation={}",
                    phase,
                    actor->GetFormID(),
                    actor->IsPlayerRef() ? 1 : 0,
                    root ? 1 : 0,
                    object ? 1 : 0,
                    generation);
                return;
            }

            auto& runtime = geometry->GetGeometryRuntimeData();
            auto* skin = runtime.skinInstance.get();
            auto* skinPartition = skin ? skin->skinPartition.get() : nullptr;

            std::uint32_t partitionCount = 0;
            std::uint32_t vertexCount = 0;
            bool partitionBuffer = false;
            if (skinPartition) {
                partitionCount = skinPartition->numPartitions;
                vertexCount = skinPartition->vertexCount;
                if (partitionCount > 0) {
                    partitionBuffer =
                        skinPartition->partitions[0].buffData != nullptr;
                }
            }

            bool shader = false;
            bool shaderSkinned = false;
            auto* effect =
                runtime.properties[RE::BSGeometry::States::kEffect].get();
            if (effect &&
                effect->GetType() == RE::NiShadeProperty::Type::kShade) {
                auto* lighting = static_cast<RE::BSLightingShaderProperty*>(effect);
                shader = lighting != nullptr;
                if (lighting) {
                    shaderSkinned = lighting->flags.any(
                        RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
                }
            }

            const auto& flags = geometry->GetFlags();
            const char* rawRTTI =
                geometry->GetRTTI() ? geometry->GetRTTI()->name : nullptr;

            SKSE::log::info(
                "OSTNET SKEE OVERLAY GEOMETRY phase={} actor={:08X} player={} node=1 geometry=1 rtti={} parent={} skin={} skinData={} skinPartition={} partitions={} vertices={} partitionBuffer={} rendererData={} shader={} shaderSkinned={} appCull={} hidden={} disableSorting={} alwaysDraw={} generation={}",
                phase,
                actor->GetFormID(),
                actor->IsPlayerRef() ? 1 : 0,
                rawRTTI ? rawRTTI : "unknown",
                object->parent ? 1 : 0,
                skin ? 1 : 0,
                skin && skin->skinData ? 1 : 0,
                skinPartition ? 1 : 0,
                partitionCount,
                vertexCount,
                partitionBuffer ? 1 : 0,
                runtime.rendererData ? 1 : 0,
                shader ? 1 : 0,
                shaderSkinned ? 1 : 0,
                object->GetAppCulled() ? 1 : 0,
                flags.all(RE::NiAVObject::Flag::kHidden) ? 1 : 0,
                flags.all(RE::NiAVObject::Flag::kDisableSorting) ? 1 : 0,
                flags.all(RE::NiAVObject::Flag::kAlwaysDraw) ? 1 : 0,
                generation);
        }

        void QueueGeometryPass(
            RE::FormID actorID,
            std::uint64_t generation,
            std::chrono::milliseconds delay,
            std::string phase,
            bool spatialRefresh)
        {
            // RaceMenu's RemoveOverlays/AddOverlays(false) are asynchronous.
            // Do not enqueue this pass immediately from the same SKSE task: in
            // practice that can still execute before the fresh overlay holder
            // has been installed. Sleep off-thread first, then return to the
            // game thread and inspect/update the geometry that now exists.
            std::thread(
                [actorID,
                 generation,
                 delay,
                 phase = std::move(phase),
                 spatialRefresh]() {
                    std::this_thread::sleep_for(delay);

                    auto* tasks = SKSE::GetTaskInterface();
                    if (!tasks) {
                        return;
                    }

                    tasks->AddTask(
                        [actorID,
                         generation,
                         phase,
                         spatialRefresh]() {
                            auto* form = RE::TESForm::LookupByID(actorID);
                            auto* actor = form ? form->As<RE::Actor>() : nullptr;
                            if (!actor) {
                                return;
                            }

                            const bool generationCurrent =
                                IsGenerationCurrent(actorID, generation);

                            auto* root = actor->Get3D();
                            if (spatialRefresh &&
                                root &&
                                !actor->IsPlayerRef()) {
                                RE::NiUpdateData updateData{};
                                root->UpdateTransformAndBounds(updateData);
                                root->UpdateWorldBound();

                                SKSE::log::info(
                                    "OSTNET SKEE PROXY SPATIAL REFRESH phase={} actor={:08X} root=1 transformBounds=1 worldBound=1 generation={} current={}",
                                    phase,
                                    actorID,
                                    generation,
                                    generationCurrent ? 1 : 0);
                            }

                            LogOverlayGeometryDiagnostic(
                                actor,
                                phase,
                                generation);
                        });
                })
                .detach();
        }
    }

    void Queue(RE::Actor* actor, std::string_view reason)
    {
        if (!actor) {
            return;
        }

        const auto actorID = actor->GetFormID();
        const std::string reasonCopy(reason);
        const auto generation =
            g_nextGeneration.fetch_add(1, std::memory_order_relaxed);

        {
            std::scoped_lock lock(g_queueMutex);
            g_actorGeneration[actorID] = generation;
        }

        // Requests are coalesced by actor. For the real player, RaceMenu's
        // ActorUpdateManager update is sufficient and is the path validated in
        // 0.33.2. A remote STR proxy is different: OStim/STR can rebuild its
        // body while RaceMenu retains a Body [OvlN] geometry object that is
        // still registered but skinned/relinked to an older source body. In
        // that state all node overrides, texture and alpha values can look
        // correct while the overlay remains visually absent.
        //
        // Only when CumOverlays actually changes on a NON-player actor, remove
        // and add the RaceMenu overlay holder once. RaceMenu then creates or
        // rebinds the overlay geometry against the proxy's current body before
        // node overrides are applied. This is deliberately NOT periodic: the
        // quiet-window generation guard keeps the old rebuild-storm crash from
        // returning.
        std::thread(
            [actorID,
             reasonCopy,
             generation]() {
                std::this_thread::sleep_for(kQuietDelay);

                if (!IsGenerationCurrent(actorID, generation)) {
                    return;
                }

                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    return;
                }

                tasks->AddTask(
                    [actorID,
                     reasonCopy,
                     generation]() {
                        if (!IsGenerationCurrent(actorID, generation)) {
                            return;
                        }

                        auto* form = RE::TESForm::LookupByID(actorID);
                        auto* actor2 = form ? form->As<RE::Actor>() : nullptr;
                        if (!actor2) {
                            return;
                        }

                        auto* map = QueryInterfaceMap();
                        auto* updates = QueryActorUpdateManager(map);
                        if (!updates) {
                            SKSE::log::warn(
                                "OSTNET SKEE OVERLAY UPDATE reason={} actor={:08X} manager=0 generation={}",
                                reasonCopy,
                                actorID,
                                generation);
                            return;
                        }

                        const bool isOCumOverlayChange =
                            reasonCopy == kOCumOverlayChanged;
                        const bool hardProxyReinstall =
                            !actor2->IsPlayerRef() &&
                            isOCumOverlayChange;

                        bool hadOverlays = false;
                        bool reinstalled = false;

                        if (hardProxyReinstall) {
                            auto* overlay = QueryOverlayInterface(map);
                            if (overlay) {
                                hadOverlays = overlay->HasOverlays(actor2);

                                // These calls QUEUE RaceMenu work. The delayed
                                // post-passes below intentionally wait before
                                // touching transforms/bounds on the fresh nodes.
                                overlay->RemoveOverlays(actor2, false);
                                overlay->AddOverlays(actor2, false);
                                reinstalled = true;

                                updates->AddNodeOverrideUpdate(actorID);
                                updates->Flush();
                            } else {
                                updates->AddOverlayUpdate(actorID);
                                updates->AddNodeOverrideUpdate(actorID);
                                updates->Flush();
                            }
                        } else {
                            updates->AddOverlayUpdate(actorID);
                            updates->AddNodeOverrideUpdate(actorID);
                            updates->Flush();
                        }

                        if (hardProxyReinstall) {
                            QueueGeometryPass(
                                actorID,
                                generation,
                                kProxySpatialDelay1,
                                "PROXY-T150",
                                true);
                            QueueGeometryPass(
                                actorID,
                                generation,
                                kProxySpatialDelay2,
                                "PROXY-T650",
                                true);
                        } else if (
                            actor2->IsPlayerRef() &&
                            isOCumOverlayChange) {
                            // Comparison sample from the known-good local path.
                            QueueGeometryPass(
                                actorID,
                                generation,
                                kProxySpatialDelay1,
                                "LOCAL-T150",
                                false);
                        }

                        SKSE::log::info(
                            "OSTNET SKEE OVERLAY UPDATE reason={} actor={:08X} player={} pipeline={} overlay=1 nodeOverrides=1 hardProxyReinstall={} hadOverlays={} delayedSpatial={} coalesced=1 generation={}",
                            reasonCopy,
                            actorID,
                            actor2->IsPlayerRef() ? 1 : 0,
                            reinstalled ?
                                "RemoveAdd+ActorUpdateManager" :
                                "ActorUpdateManager",
                            hardProxyReinstall ? 1 : 0,
                            hadOverlays ? 1 : 0,
                            hardProxyReinstall ? 1 : 0,
                            generation);
                    });
            })
            .detach();
    }
}
