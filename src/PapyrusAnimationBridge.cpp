#include "PCH.h"
#include "PapyrusAnimationBridge.h"

#include <RE/I/IFunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>
#include <RE/V/Variable.h>

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

                // CommonLibSSE-NG 3.5.3 exposes:
                //
                //   Variable::Pack<T>(T&&)
                //
                // Because this operator is const, _reference and _eventName
                // are const expressions here.  Make mutable local copies and
                // pass them as rvalues.
                auto reference =
                    _reference;

                auto eventName =
                    _eventName;

                RE::BSScript::Variable refArg;
                refArg.Pack<RE::TESObjectREFR*>(
                    std::move(reference));

                RE::BSScript::Variable eventArg;
                eventArg.Pack<std::string>(
                    std::move(eventName));

                dst.push_back(
                    std::move(refArg));

                dst.push_back(
                    std::move(eventArg));

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
    }

    PapyrusAnimationBridge&
        PapyrusAnimationBridge::GetSingleton()
    {
        static PapyrusAnimationBridge instance;
        return instance;
    }

    bool PapyrusAnimationBridge::
        SendForcedAnimationEvent(
            RE::Actor* actor,
            std::string_view eventName)
    {
        if (!actor ||
            eventName.empty()) {
            return false;
        }

        auto* skyrimVM =
            RE::SkyrimVM::GetSingleton();

        if (!skyrimVM ||
            !skyrimVM->impl) {
            SKSE::log::error(
                "OSTNET PAPYRUS FORCE: SkyrimVM unavailable");
            return false;
        }

        // DispatchStaticCall consumes the argument functor while preparing
        // the VM call.  Keep the argument object alive for the duration of
        // DispatchStaticCall.
        DebugAnimationArguments args(
            actor,
            eventName);

        RE::BSTSmartPointer<
            RE::BSScript::IStackCallbackFunctor>
            callback;

        const RE::BSFixedString className(
            "Debug");

        const RE::BSFixedString functionName(
            "SendAnimationEvent");

        const bool dispatched =
            skyrimVM->impl->DispatchStaticCall(
                className,
                functionName,
                &args,
                callback);

        return dispatched;
    }

    bool PapyrusAnimationBridge::SetOStimObjectState(
        RE::Actor* actor,
        std::string_view objectType,
        bool equipped)
    {
        if (!actor || objectType.empty()) {
            return false;
        }

        auto* skyrimVM =
            RE::SkyrimVM::GetSingleton();

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

        const bool dispatched =
            skyrimVM->impl->DispatchStaticCall(
                className,
                functionName,
                &args,
                callback);

        SKSE::log::info(
            "OSTNET ADDON OBJ PAPYRUS actor={:08X} type={} equipped={} dispatched={}",
            actor->GetFormID(),
            objectType,
            equipped ? 1 : 0,
            dispatched ? 1 : 0);

        return dispatched;
    }
}
