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

        std::mutex g_queueMutex;
        std::unordered_map<RE::FormID, std::uint64_t> g_actorGeneration;
        std::atomic<std::uint64_t> g_nextGeneration{ 1 };

        SKEE::IActorUpdateManager* QueryActorUpdateManager()
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

            return static_cast<SKEE::IActorUpdateManager*>(
                exchange.interfaceMap->QueryInterface("ActorUpdateManager"));
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

        // 0.31.6 could enqueue multiple synchronous AddOverlays rebuilds for
        // every OCum packet and eventually crash inside SKEE. Keep the per-actor
        // quiet window, but use RaceMenu's own ActorUpdateManager now. Unlike
        // IOverlayInterface::AddOverlays(), ActorUpdateManager can refresh the
        // real PlayerCharacter as well as a remote STR proxy, and it sequences
        // overlay geometry followed by node-override application in RaceMenu's
        // normal task pipeline.
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

                        auto* updates = QueryActorUpdateManager();
                        if (!updates) {
                            SKSE::log::warn(
                                "OSTNET SKEE OVERLAY UPDATE reason={} actor={:08X} manager=0 generation={}",
                                reasonCopy,
                                actorID,
                                generation);
                            return;
                        }

                        updates->AddOverlayUpdate(actorID);
                        updates->AddNodeOverrideUpdate(actorID);
                        updates->Flush();

                        SKSE::log::info(
                            "OSTNET SKEE OVERLAY UPDATE reason={} actor={:08X} player={} pipeline=ActorUpdateManager overlay=1 nodeOverrides=1 coalesced=1 generation={}",
                            reasonCopy,
                            actorID,
                            actor2->IsPlayerRef() ? 1 : 0,
                            generation);
                    });
            })
            .detach();
    }
}
