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

            class IOverlayInterface : public IPluginInterface
            {
            public:
                virtual bool HasOverlays(RE::TESObjectREFR* reference) = 0;
                virtual void AddOverlays(
                    RE::TESObjectREFR* reference,
                    bool immediate = false) = 0;
            };
        }

        constexpr auto kQuietDelay = std::chrono::milliseconds(250);

        std::mutex g_queueMutex;
        std::unordered_map<RE::FormID, std::uint64_t> g_actorGeneration;
        std::atomic<std::uint64_t> g_nextGeneration{ 1 };

        SKEE::IOverlayInterface* QueryOverlayInterface()
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

            return static_cast<SKEE::IOverlayInterface*>(
                exchange.interfaceMap->QueryInterface("Overlay"));
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
        if (!actor || actor->IsPlayerRef()) {
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

        // 0.31.6 could enqueue two synchronous rebuilds for every OBJ packet.
        // OCum's live poll then produced more than a thousand RaceMenu rebuilds
        // in one scene and Player1 crashed inside skee64 while installing the
        // remote proxy's face overlay. Debounce by actor: only the newest request
        // survives a quiet window, and let RaceMenu execute through its normal
        // queued path instead of rebuilding synchronously inside our game task.
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
                        if (!actor2 || actor2->IsPlayerRef()) {
                            return;
                        }

                        auto* overlay = QueryOverlayInterface();
                        if (!overlay) {
                            SKSE::log::warn(
                                "OSTNET SKEE OVERLAY REBUILD reason={} actor={:08X} interface=0 generation={}",
                                reasonCopy,
                                actorID,
                                generation);
                            return;
                        }

                        const bool hadOverlays =
                            overlay->HasOverlays(actor2);

                        overlay->AddOverlays(actor2, false);

                        SKSE::log::info(
                            "OSTNET SKEE OVERLAY REBUILD reason={} actor={:08X} hadOverlays={} immediate=0 coalesced=1 generation={}",
                            reasonCopy,
                            actorID,
                            hadOverlays ? 1 : 0,
                            generation);
                    });
            })
            .detach();
    }
}
