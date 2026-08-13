#include "PCH.h"
#include "Input.h"

#include "Config.h"
#include "EquipmentLock.h"

namespace OStimTogether
{
    InputHandler& InputHandler::GetSingleton()
    {
        static InputHandler instance;
        return instance;
    }

    void InputHandler::Register()
    {
        if (auto* manager = RE::BSInputDeviceManager::GetSingleton()) {
            manager->AddEventSink(this);
            SKSE::log::info("Input event sink registered");
        }
    }

    RE::BSEventNotifyControl InputHandler::ProcessEvent(
        RE::InputEvent* const* events,
        RE::BSTEventSource<RE::InputEvent*>*)
    {
        if (!events) {
            return RE::BSEventNotifyControl::kContinue;
        }

        static const Config config = Config::Load();

        for (auto* event = *events; event; event = event->next) {
            if (event->GetEventType() !=
                RE::INPUT_EVENT_TYPE::kButton) {
                continue;
            }

            auto* button = event->AsButtonEvent();
            if (!button || !button->IsDown()) {
                continue;
            }

            if (button->GetDevice() !=
                RE::INPUT_DEVICE::kKeyboard) {
                continue;
            }

            const auto key = button->GetIDCode();

            if (key == config.toggleKey) {
                EquipmentLock::GetSingleton().ToggleCrosshairActor();
            } else if (key == config.clearKey) {
                EquipmentLock::GetSingleton().ClearManual();
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }
}
