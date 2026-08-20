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

            // Only the public vtable prefix through AddOverlays is required.
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

                            // RaceMenu AddOverlays() inserts the actor into a
                            // set and always queues QueueOverlayBuild(). Calling
                            // it for an already-registered proxy therefore
                            // refreshes geometry without deleting the holder or
                            // any unrelated user overlays.
                            overlay->AddOverlays(actor, false);

                            SKSE::log::info(
                                "OSTNET SKEE OVERLAY REBUILD reason={} phase={} actor={:08X} hadOverlays={} queued=1",
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

        // First pass lets the remote override packet finish storing its SKEE
        // node properties. The second pass covers OStim/STR body rebuilds that
        // replace the proxy geometry shortly afterwards.
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
