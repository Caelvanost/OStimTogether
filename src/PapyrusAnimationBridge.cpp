#include "PCH.h"
#include "PapyrusAnimationBridge.h"
#include "SKEEOverlayRefresh.h"

#include <RE/I/IFunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>
#include <RE/V/Variable.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace OStimTogether
{
    namespace
    {
        class DebugAnimationArguments final :
            public RE::BSScript::IFunctionArguments
        {
        public:
            DebugAnimationArguments(
                RE::TESObjectREFR* reference,
                std::string_view eventName) :
                _reference(reference),
                _eventName(eventName)
            {}

            bool operator()(
                RE::BSScrapArray<
                    RE::BSScript::Variable>& dst)
                const override
            {
                if (!_reference ||
                    _eventName.empty()) {
                    return false;
                }

                auto reference = _reference;
                auto eventName = _eventName;

                RE::BSScript::Variable refArg;
                refArg.Pack<RE::TESObjectREFR*>(
                    std::move(reference));

                RE::BSScript::Variable eventArg;
                eventArg.Pack<std::string>(
                    std::move(eventName));

                dst.push_back(std::move(refArg));
                dst.push_back(std::move(eventArg));
                return true;
            }

        private:
            RE::TESObjectREFR* _reference{ nullptr };
            std::string _eventName;
        };

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
                RE::BSScrapArray<
                    RE::BSScript::Variable>& dst)
                const override
            {
                if (!_actor || _text.empty()) {
                    return false;
                }

                auto actor = _actor;
                auto text = _text;

                RE::BSScript::Variable actorArg;
                actorArg.Pack<RE::Actor*>(
                    std::move(actor));

                RE::BSScript::Variable textArg;
                textArg.Pack<std::string>(
                    std::move(text));

                dst.push_back(std::move(actorArg));
                dst.push_back(std::move(textArg));
                return true;
            }

        private:
            RE::Actor* _actor{ nullptr };
            std::string _text;
        };

        struct DesiredOStimObjectState
        {
            bool equipped{ false };
            std::uint64_t generation{ 0 };
        };

        std::mutex g_ostimObjectStateMutex;
        std::unordered_map<std::string, DesiredOStimObjectState>
            g_ostimObjectStates;
        std::atomic<std::uint64_t> g_nextOStimObjectGeneration{ 1 };

        std::string MakeOStimObjectKey(
            RE::FormID actorID,
            std::string_view objectType)
        {
            return fmt::format(
                "{:08X}|{}",
                actorID,
                objectType);
        }

        std::pair<std::uint64_t, bool> SetDesiredOStimObjectState(
            RE::FormID actorID,
            std::string_view objectType,
            bool equipped)
        {
            const auto key =
                MakeOStimObjectKey(actorID, objectType);

            std::scoped_lock lock(g_ostimObjectStateMutex);

            const auto it = g_ostimObjectStates.find(key);
            if (it != g_ostimObjectStates.end() &&
                it->second.equipped == equipped) {
                return { it->second.generation, false };
            }

            const auto generation =
                g_nextOStimObjectGeneration.fetch_add(
                    1,
                    std::memory_order_relaxed);

            g_ostimObjectStates[key] =
                DesiredOStimObjectState{
                    equipped,
                    generation
                };

            return { generation, true };
        }

        bool IsDesiredOStimObjectStateCurrent(
            RE::FormID actorID,
            std::string_view objectType,
            bool equipped,
            std::uint64_t generation)
        {
            const auto key =
                MakeOStimObjectKey(actorID, objectType);

            std::scoped_lock lock(g_ostimObjectStateMutex);

            const auto it = g_ostimObjectStates.find(key);
            return it != g_ostimObjectStates.end() &&
                   it->second.equipped == equipped &&
                   it->second.generation == generation;
        }

        bool DispatchOStimObjectState(
            RE::Actor* actor,
            std::string_view objectType,
            bool equipped)
        {
            if (!actor || objectType.empty()) {
                return false;
            }

            auto* skyrimVM = RE::SkyrimVM::GetSingleton();

            if (!skyrimVM || !skyrimVM->impl) {
                SKSE::log::error(
                    "OSTNET ADDON OBJ PAPYRUS: SkyrimVM unavailable actor={:08X} type={}",
                    actor->GetFormID(),
                    objectType);
                return false;
            }

            ActorStringArguments args(actor, objectType);
            RE::BSTSmartPointer<
                RE::BSScript::IStackCallbackFunctor> callback;

            const RE::BSFixedString className("OActor");
            const RE::BSFixedString functionName(
                equipped ? "EquipObject" : "UnequipObject");

            return skyrimVM->impl->DispatchStaticCall(
                className,
                functionName,
                &args,
                callback);
        }

        void QueueOStimObjectRetry(
            RE::FormID actorID,
            std::string objectType,
            bool equipped,
            std::uint64_t generation,
            std::uint32_t attempt,
            std::chrono::milliseconds delay)
        {
            std::thread(
                [actorID,
                 objectType = std::move(objectType),
                 equipped,
                 generation,
                 attempt,
                 delay]() mutable {
                    std::this_thread::sleep_for(delay);

                    if (!IsDesiredOStimObjectStateCurrent(
                            actorID,
                            objectType,
                            equipped,
                            generation)) {
                        return;
                    }

                    auto* tasks = SKSE::GetTaskInterface();
                    if (!tasks) {
                        SKSE::log::warn(
                            "OSTNET ADDON OBJ RETRY no-task-interface actor={:08X} type={} attempt={}",
                            actorID,
                            objectType,
                            attempt);
                        return;
                    }

                    tasks->AddTask(
                        [actorID,
                         objectType = std::move(objectType),
                         equipped,
                         generation,
                         attempt]() {
                            if (!IsDesiredOStimObjectStateCurrent(
                                    actorID,
                                    objectType,
                                    equipped,
                                    generation)) {
                                SKSE::log::trace(
                                    "OSTNET ADDON OBJ RETRY cancelled actor={:08X} type={} attempt={} generation={}",
                                    actorID,
                                    objectType,
                                    attempt,
                                    generation);
                                return;
                            }

                            auto* form = RE::TESForm::LookupByID(actorID);
                            auto* actor = form ? form->As<RE::Actor>() : nullptr;

                            if (!actor) {
                                SKSE::log::warn(
                                    "OSTNET ADDON OBJ RETRY missing-actor actor={:08X} type={} attempt={} generation={}",
                                    actorID,
                                    objectType,
                                    attempt,
                                    generation);
                                return;
                            }

                            const bool dispatched =
                                DispatchOStimObjectState(
                                    actor,
                                    objectType,
                                    equipped);

                            SKSE::log::info(
                                "OSTNET ADDON OBJ RETRY actor={:08X} type={} equipped={} attempt={} generation={} dispatched={}",
                                actorID,
                                objectType,
                                equipped ? 1 : 0,
                                attempt,
                                generation,
                                dispatched ? 1 : 0);
                        });
                })
                .detach();
        }
    }

    PapyrusAnimationBridge&
        PapyrusAnimationBridge::GetSingleton()
    {
        static PapyrusAnimationBridge instance;
        return instance;
    }

    bool PapyrusAnimationBridge::SendForcedAnimationEvent(
        RE::Actor* actor,
        std::string_view eventName)
    {
        if (!actor || eventName.empty()) {
            return false;
        }

        auto* skyrimVM = RE::SkyrimVM::GetSingleton();

        if (!skyrimVM || !skyrimVM->impl) {
            SKSE::log::error(
                "OSTNET PAPYRUS FORCE: SkyrimVM unavailable");
            return false;
        }

        DebugAnimationArguments args(actor, eventName);

        RE::BSTSmartPointer<
            RE::BSScript::IStackCallbackFunctor> callback;

        const RE::BSFixedString className("Debug");
        const RE::BSFixedString functionName("SendAnimationEvent");

        return skyrimVM->impl->DispatchStaticCall(
            className,
            functionName,
            &args,
            callback);
    }

    bool PapyrusAnimationBridge::SetOStimObjectState(
        RE::Actor* actor,
        std::string_view objectType,
        bool equipped)
    {
        if (!actor || objectType.empty()) {
            return false;
        }

        const auto actorID = actor->GetFormID();
        const auto [generation, changed] =
            SetDesiredOStimObjectState(
                actorID,
                objectType,
                equipped);

        const bool dispatched =
            DispatchOStimObjectState(
                actor,
                objectType,
                equipped);

        SKSE::log::info(
            "OSTNET ADDON OBJ PAPYRUS actor={:08X} type={} equipped={} generation={} changed={} dispatched={}",
            actorID,
            objectType,
            equipped ? 1 : 0,
            generation,
            changed ? 1 : 0,
            dispatched ? 1 : 0);

        // 0.31.6 crash evidence showed that repeated unchanged OBJ packets
        // were causing a SKEE AddOverlays rebuild storm on the remote proxy.
        // Only a genuine equip-object transition may request a geometry rebuild.
        // Repeated idempotent state repairs still dispatch OActor so a rebuilt
        // OStim actor can recover the desired mesh state, but they no longer
        // recreate RaceMenu overlay geometry.
        if (changed) {
            SKEEOverlayRefresh::Queue(
                actor,
                "ADDON-OBJECT-CHANGED");
        }

        // OActor.EquipObject only succeeds once the actor is registered in an
        // active OStim thread. STRPM addon state can arrive a few frames before
        // StartRemoteMirror has finished registering the remote proxy, so an
        // accepted VM dispatch is not proof that the native OStim call actually
        // equipped the mesh. Retry the same idempotent desired state over a
        // short bounded window. A generation guard cancels stale retries as soon
        // as a newer equipped/unequipped state arrives.
        if (changed) {
            if (equipped) {
                constexpr std::array delays{
                    std::chrono::milliseconds(150),
                    std::chrono::milliseconds(350),
                    std::chrono::milliseconds(700),
                    std::chrono::milliseconds(1100)
                };

                for (std::size_t i = 0; i < delays.size(); ++i) {
                    QueueOStimObjectRetry(
                        actorID,
                        std::string(objectType),
                        true,
                        generation,
                        static_cast<std::uint32_t>(i + 1),
                        delays[i]);
                }
            } else {
                constexpr std::array delays{
                    std::chrono::milliseconds(150),
                    std::chrono::milliseconds(500)
                };

                for (std::size_t i = 0; i < delays.size(); ++i) {
                    QueueOStimObjectRetry(
                        actorID,
                        std::string(objectType),
                        false,
                        generation,
                        static_cast<std::uint32_t>(i + 1),
                        delays[i]);
                }
            }
        }

        return dispatched;
    }
}
