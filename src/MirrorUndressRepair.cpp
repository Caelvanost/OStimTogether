#include "PCH.h"
#include "MirrorUndressRepair.h"
#include "OStimAPI/InterfaceExchangeMessage.h"

#include <array>

namespace OStimTogether
{
    namespace
    {
        bool IsLikelySTRRemotePlayerProxy(RE::Actor* actor)
        {
            if (!actor || actor->IsPlayerRef()) {
                return false;
            }

            auto* base = actor->GetActorBase();
            if (!base) {
                return false;
            }

            constexpr RE::FormID kDynamicMask = 0xFF000000;
            return (actor->GetFormID() & kDynamicMask) == kDynamicMask &&
                   (base->GetFormID() & kDynamicMask) == kDynamicMask;
        }

        bool HasWorn(
            RE::Actor* actor,
            RE::BGSBipedObjectForm::BipedObjectSlot slot)
        {
            // CommonLibSSE-NG 3.5.3 exposes the Actor overload used by the
            // project's Windows toolchain as GetWornArmor(slot). Do not pass
            // the newer optional no-init argument here.
            return actor && actor->GetWornArmor(slot) != nullptr;
        }
    }

    MirrorUndressRepair& MirrorUndressRepair::GetSingleton()
    {
        static MirrorUndressRepair instance;
        return instance;
    }

    void MirrorUndressRepair::StartListener::listen(OStim::Thread* thread)
    {
        MirrorUndressRepair::GetSingleton().HandleStart(thread);
    }

    bool MirrorUndressRepair::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::warn(
                "MirrorUndressRepair: no SKSE messaging interface");
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        const bool dispatched = messaging->Dispatch(
            OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
            &exchange,
            sizeof(exchange),
            nullptr);

        if (!dispatched || !exchange.interfaceMap) {
            SKSE::log::warn(
                "MirrorUndressRepair: OStim interface exchange failed dispatched={} map={}",
                dispatched ? 1 : 0,
                exchange.interfaceMap ? 1 : 0);
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(
                OStim::ThreadInterface::NAME));

        if (!_threads) {
            SKSE::log::warn(
                "MirrorUndressRepair: OStim Threads interface unavailable");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);

        SKSE::log::info(
            "MirrorUndressRepair READY threadsVersion={}",
            _threads->getVersion());
        return true;
    }

    void MirrorUndressRepair::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !_threads || thread->getActorCount() < 2) {
            return;
        }

        bool hasLocalPlayer = false;
        bool hasSTRProxy = false;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ?
                static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;

            if (!actor) {
                continue;
            }

            hasLocalPlayer = hasLocalPlayer || actor->IsPlayerRef();
            hasSTRProxy = hasSTRProxy || IsLikelySTRRemotePlayerProxy(actor);
        }

        if (!hasLocalPlayer || !hasSTRProxy) {
            return;
        }

        const auto threadID = thread->getThreadID();

        // OStim's START callback precedes some of its initial equipment work.
        // Use a small bounded set of checks so a body strip that finishes
        // after the first sample can still expose a residual helmet/glove/boot
        // regression. Fully clothed scenes remain untouched on every pass.
        constexpr std::array delays{
            std::chrono::milliseconds(180),
            std::chrono::milliseconds(600),
            std::chrono::milliseconds(1200)
        };

        for (const auto delay : delays) {
            std::thread([this, threadID, delay]() {
                std::this_thread::sleep_for(delay);

                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    return;
                }

                tasks->AddTask([this, threadID]() {
                    Repair(threadID);
                });
            }).detach();
        }
    }

    void MirrorUndressRepair::Repair(std::int32_t threadID)
    {
        if (!_threads) {
            return;
        }

        auto* thread = _threads->getThread(threadID);
        if (!thread) {
            return;
        }

        OStim::ThreadActor* playerTA = nullptr;
        RE::Actor* player = nullptr;
        bool hasSTRProxy = false;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ?
                static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;

            if (!actor) {
                continue;
            }

            if (actor->IsPlayerRef()) {
                playerTA = ta;
                player = actor;
            } else if (IsLikelySTRRemotePlayerProxy(actor)) {
                hasSTRProxy = true;
            }
        }

        if (!playerTA || !player || !hasSTRProxy) {
            return;
        }

        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        const bool body = HasWorn(player, Slot::kBody);
        const bool head = HasWorn(player, Slot::kHead);
        const bool hands = HasWorn(player, Slot::kHands) ||
                           HasWorn(player, Slot::kForearms);
        const bool feet = HasWorn(player, Slot::kFeet) ||
                          HasWorn(player, Slot::kCalves);

        // Target the observed STR mirror regression specifically: the body is
        // already stripped by OStim, but a residual primary armor piece such
        // as Elir's helmet remains equipped. Do not force fully clothed scenes
        // to undress.
        if (body || (!head && !hands && !feet)) {
            SKSE::log::info(
                "OSTNET MIRROR UNDRESS CHECK thread={} actor={:08X} body={} head={} hands={} feet={} action=none",
                threadID,
                player->GetFormID(),
                body ? 1 : 0,
                head ? 1 : 0,
                hands ? 1 : 0,
                feet ? 1 : 0);
            return;
        }

        playerTA->undress();

        SKSE::log::info(
            "OSTNET MIRROR UNDRESS REPAIR thread={} actor={:08X} body=0 head={} hands={} feet={} action=OStim-undress",
            threadID,
            player->GetFormID(),
            head ? 1 : 0,
            hands ? 1 : 0,
            feet ? 1 : 0);
    }
}
