#include "PCH.h"
#include "ProxyOnlyThreadCleanup.h"

#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr const char* kPluginName = "OStimTogether";
    }

    ProxyOnlyThreadCleanup& ProxyOnlyThreadCleanup::GetSingleton()
    {
        static ProxyOnlyThreadCleanup instance;
        return instance;
    }

    void ProxyOnlyThreadCleanup::StartListener::listen(OStim::Thread* thread)
    {
        ProxyOnlyThreadCleanup::GetSingleton().HandleStart(thread);
    }

    bool ProxyOnlyThreadCleanup::LoadSceneAPI()
    {
        const auto module = GetModuleHandleA("OStim.dll");
        if (!module) {
            return false;
        }

        auto requestScene = reinterpret_cast<OStimModAPI::Scene::RequestAPI>(
            reinterpret_cast<void*>(
                GetProcAddress(module, "RequestPluginAPI_Scene")));
        if (!requestScene) {
            return false;
        }

        auto* declaration = SKSE::PluginDeclaration::GetSingleton();
        const auto version = declaration ?
            declaration->GetVersion() : REL::Version{ 0, 30, 0, 0 };
        const auto pluginName = declaration ?
            std::string(declaration->GetName()) : std::string(kPluginName);

        _sceneControl = requestScene(
            OStimModAPI::Scene::InterfaceVersion::V1,
            pluginName.c_str(),
            version);
        return _sceneControl != nullptr;
    }

    bool ProxyOnlyThreadCleanup::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr && _sceneControl != nullptr;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
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
                "OSTNET AUX CLEANUP unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));

        if (!_threads || !LoadSceneAPI()) {
            SKSE::log::warn(
                "OSTNET AUX CLEANUP unavailable threads={} sceneControl={}",
                _threads ? 1 : 0,
                _sceneControl ? 1 : 0);
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        SKSE::log::info(
            "OSTNET AUX CLEANUP READY rule=proxy-present-and-no-local-player action=stop-thread");
        return true;
    }

    void ProxyOnlyThreadCleanup::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !_sceneControl) {
            return;
        }

        bool hasLocalPlayer = false;
        std::uint32_t mappedProxyCount = 0;

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }

            if (actor->IsPlayerRef()) {
                hasLocalPlayer = true;
            } else if (STRPMTransport::GetSingleton().ResolveConnection(
                           actor->GetFormID())) {
                ++mappedProxyCount;
            }
        }

        if (hasLocalPlayer || mappedProxyCount == 0) {
            return;
        }

        const auto threadID = thread->getThreadID();
        SKSE::log::warn(
            "OSTNET AUX CLEANUP QUEUED thread={} actors={} mappedProxies={} reason=no-local-player",
            threadID,
            thread->getActorCount(),
            mappedProxyCount);

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([this, threadID]() {
                if (!_sceneControl || threadID < 0) {
                    return;
                }

                const auto result = _sceneControl->StopScene(
                    kPluginName,
                    static_cast<std::uint32_t>(threadID));

                SKSE::log::warn(
                    "OSTNET AUX CLEANUP STOP thread={} result={} reason=proxy-only-thread",
                    threadID,
                    static_cast<std::uint32_t>(result));
            });
        }
    }
}
