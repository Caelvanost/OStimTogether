#include "PCH.h"
#include "SKEEOverlayRefresh.h"

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

            // Public SKEE IOverlayInterface vtable prefix. In RaceMenu's
            // implementation the boolean passed to AddOverlays controls
            // immediate execution: false queues SKSETaskUpdateOverlays, true
            // rebuilds synchronously on the current game thread.
            class IOverlayInterface : public IPluginInterface
            {
            public:
                virtual bool HasOverlays(RE::TESObjectREFR* reference) = 0;
                virtual void AddOverlays(
                    RE::TESObjectREFR* reference,
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

        void QueueOne(
            RE::FormID actorID,
            std::string reason,
            std::chrono::milliseconds delay,
            const char* phase)
        {
            std::thread(
                [actorID,
                 reason = std::move(reason),
                 delay,
                 phase = std::string(phase)]() {
                    std::this_thread::sleep_for(delay);

                    auto* tasks = SKSE::GetTaskInterface();
                    if (!tasks) {
                        return;
                    }

                    tasks->AddTask(
                        [actorID,
                         reason = std::move(reason),
                         phase = std::move(phase)]() {
                            auto* form = RE::TESForm::LookupByID(actorID);
                            auto* actor = form ? form->As<RE::Actor>() : nullptr;
                            if (!actor || actor->IsPlayerRef()) {
                                return;
                            }

                            auto* overlay = QueryOverlayInterface();
                            if (!overlay) {
                                SKSE::log::warn(
                                    "OSTNET SKEE OVERLAY REBUILD reason={} phase={} actor={:08X} interface=0",
                                    reason,
                                    phase,
                                    actorID);
                                return;
                            }

                            const bool hadOverlays =
                                overlay->HasOverlays(actor);

                            // AddOverlays() uses a set internally, so invoking
                            // it for an already registered proxy is safe. Run
                            // the rebuild immediately on this game-thread task
                            // instead of queueing another task behind OStim/STR
                            // geometry work. RaceMenu's build copies the live
                            // skin instance and reapplies stored node overrides
                            // to each overlay shape.
                            overlay->AddOverlays(actor, true);

                            SKSE::log::info(
                                "OSTNET SKEE OVERLAY REBUILD reason={} phase={} actor={:08X} hadOverlays={} immediate=1",
                                reason,
                                phase,
                                actorID,
                                hadOverlays ? 1 : 0);
                        });
                })
                .detach();
        }
    }

    void Queue(RE::Actor* actor, std::string_view reason)
    {
        if (!actor || actor->IsPlayerRef()) {
            return;
        }

        const auto actorID = actor->GetFormID();
        const std::string reasonCopy(reason);

        // The packet path stores SKEE overrides before reaching this helper.
        // Rebuild twice: once immediately after that store, then again after
        // the short OStim/STR geometry-settle window.
        QueueOne(
            actorID,
            reasonCopy,
            std::chrono::milliseconds(80),
            "T80");
        QueueOne(
            actorID,
            reasonCopy,
            std::chrono::milliseconds(450),
            "T450");
    }
}
