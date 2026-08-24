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
        constexpr std::string_view kOCumOverlayChanged =
            "OCUM-OVERLAY-CHANGED";

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

                        const bool hardProxyReinstall =
                            !actor2->IsPlayerRef() &&
                            reasonCopy == kOCumOverlayChanged;

                        bool hadOverlays = false;
                        bool reinstalled = false;

                        if (hardProxyReinstall) {
                            auto* overlay = QueryOverlayInterface(map);
                            if (overlay) {
                                hadOverlays = overlay->HasOverlays(actor2);

                                // RaceMenu queues these tasks in order. Remove
                                // first so stale proxy overlay geometry is
                                // detached, then AddOverlays queues a fresh
                                // build against the actor's current skin.
                                overlay->RemoveOverlays(actor2, false);
                                overlay->AddOverlays(actor2, false);
                                reinstalled = true;

                                // The fresh InstallOverlay path already applies
                                // persisted overrides, and this additional
                                // manager pass guarantees the current node state
                                // is replayed after the queued geometry work.
                                updates->AddNodeOverrideUpdate(actorID);
                                updates->Flush();
                            } else {
                                // Safe fallback if only ActorUpdateManager is
                                // available from this RaceMenu build.
                                updates->AddOverlayUpdate(actorID);
                                updates->AddNodeOverrideUpdate(actorID);
                                updates->Flush();
                            }
                        } else {
                            updates->AddOverlayUpdate(actorID);
                            updates->AddNodeOverrideUpdate(actorID);
                            updates->Flush();
                        }

                        SKSE::log::info(
                            "OSTNET SKEE OVERLAY UPDATE reason={} actor={:08X} player={} pipeline={} overlay=1 nodeOverrides=1 hardProxyReinstall={} hadOverlays={} coalesced=1 generation={}",
                            reasonCopy,
                            actorID,
                            actor2->IsPlayerRef() ? 1 : 0,
                            reinstalled ?
                                "RemoveAdd+ActorUpdateManager" :
                                "ActorUpdateManager",
                            hardProxyReinstall ? 1 : 0,
                            hadOverlays ? 1 : 0,
                            generation);
                    });
            })
            .detach();
    }
}
