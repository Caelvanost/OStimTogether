#include "PCH.h"
#include "FreeSceneRootSync.h"

#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

#include <cmath>

namespace OStimTogether
{
    namespace
    {
        constexpr wchar_t kSTRPMModuleName[] = L"STRPluginMessagingAPI.dll";
        constexpr char kRootChannel[] = "ostimtogether.root";
        constexpr auto kSendInterval = std::chrono::milliseconds(33);
        constexpr auto kRemoteStateLifetime = std::chrono::milliseconds(250);
        constexpr auto kLogInterval = std::chrono::milliseconds(500);
        constexpr float kMaxRootTranslation = 10000.0F;
        constexpr char kRootNodeName[] = "NPC Root [Root]";

        bool Finite(float value) noexcept
        {
            return std::isfinite(value);
        }
    }

    FreeSceneRootSync& FreeSceneRootSync::GetSingleton()
    {
        static FreeSceneRootSync instance;
        return instance;
    }

    FreeSceneRootSync::~FreeSceneRootSync()
    {
        StopTransport();
    }

    bool FreeSceneRootSync::RootTransform::IsFinite() const noexcept
    {
        if (!Finite(translate.x) ||
            !Finite(translate.y) ||
            !Finite(translate.z) ||
            std::abs(translate.x) > kMaxRootTranslation ||
            std::abs(translate.y) > kMaxRootTranslation ||
            std::abs(translate.z) > kMaxRootTranslation) {
            return false;
        }

        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                const auto value = rotate.entry[row][column];
                if (!Finite(value) || std::abs(value) > 2.0F) {
                    return false;
                }
            }
        }
        return true;
    }

    void FreeSceneRootSync::StartListener::listen(OStim::Thread* thread)
    {
        FreeSceneRootSync::GetSingleton().HandleStart(thread);
    }

    void FreeSceneRootSync::StopListener::listen(OStim::Thread* thread)
    {
        FreeSceneRootSync::GetSingleton().HandleStop(thread);
    }

    bool FreeSceneRootSync::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC unavailable: no SKSE messaging interface");
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
                "OSTNET ROOT SYNC unavailable: OStim interface exchange failed");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC unavailable: Threads interface missing");
            return false;
        }

        _threadInterfaceVersion = _threads->getVersion();
        if (_threadInterfaceVersion < 3) {
            SKSE::log::info(
                "OSTNET ROOT SYNC disabled threadsVersion={} reason=no-exact-furniture-state",
                _threadInterfaceVersion);
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET ROOT SYNC READY threadsVersion={} node=\"{}\" sendMs={} referenceWrites=0",
            _threadInterfaceVersion,
            kRootNodeName,
            kSendInterval.count());
        return true;
    }

    bool FreeSceneRootSync::StartTransport()
    {
        if (_transportRunning.load()) {
            return true;
        }

        const auto module = GetModuleHandleW(kSTRPMModuleName);
        if (!module) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC TRANSPORT unavailable: STRPluginMessagingAPI.dll not loaded");
            return false;
        }

        const auto queryInterface =
            reinterpret_cast<STRPMApi::QueryInterfaceFn>(
                GetProcAddress(
                    module,
                    STRPMApi::kQueryInterfaceExportName));
        if (!queryInterface) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC TRANSPORT unavailable: missing export {}",
                STRPMApi::kQueryInterfaceExportName);
            return false;
        }

        const STRPMApi::Interface* api = nullptr;
        const auto queryResult =
            queryInterface(STRPMApi::kInterfaceVersion, &api);
        if (queryResult != STRPMApi::Result::kOk ||
            !api || !api->registerChannel || !api->send) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC TRANSPORT unavailable: interface result={}",
                static_cast<std::uint32_t>(queryResult));
            return false;
        }

        STRPMApi::ListenerHandle listener{};
        const auto registerResult = api->registerChannel(
            kRootChannel,
            &FreeSceneRootSync::OnRootMessage,
            this,
            &listener);
        if (registerResult != STRPMApi::Result::kOk) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC TRANSPORT register failed channel={} result={}",
                kRootChannel,
                static_cast<std::uint32_t>(registerResult));
            return false;
        }

        _api = api;
        _rootListener = listener;
        _transportRunning.store(true);
        _lastTransportWarn = {};

        SKSE::log::info(
            "OSTNET ROOT SYNC TRANSPORT READY channel={} realtimeFlags=0 reliable=0 ordered=0",
            kRootChannel);
        return true;
    }

    void FreeSceneRootSync::StopTransport()
    {
        if (!_transportRunning.exchange(false)) {
            return;
        }

        if (_api &&
            _rootListener.value != 0 &&
            _api->unregisterChannel) {
            _api->unregisterChannel(_rootListener);
        }

        _rootListener = {};
        _api = nullptr;
        Reset();

        SKSE::log::info("OSTNET ROOT SYNC TRANSPORT stopped");
    }

    void FreeSceneRootSync::Reset()
    {
        _activePlayerThreadID = -1;
        _lastSend = {};
        _lastSendLog = {};
        _remoteStates.clear();
    }

    void FreeSceneRootSync::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        _activePlayerThreadID = thread->getThreadID();
        _lastSend = {};
        _lastSendLog = {};
        _remoteStates.clear();

        SKSE::log::info(
            "OSTNET ROOT SYNC ARM thread={} freeStanding={} actors={}",
            _activePlayerThreadID,
            IsFreeStandingThread(thread) ? 1 : 0,
            thread->getActorCount());
    }

    void FreeSceneRootSync::HandleStop(OStim::Thread* thread)
    {
        if (!thread || thread->getThreadID() != _activePlayerThreadID) {
            return;
        }

        SKSE::log::info(
            "OSTNET ROOT SYNC STOP thread={} remoteStates={}",
            _activePlayerThreadID,
            _remoteStates.size());
        Reset();
    }

    bool FreeSceneRootSync::IsFreeStandingThread(OStim::Thread* thread) const
    {
        if (!thread ||
            _threadInterfaceVersion < 3 ||
            thread->getFurnitureObject()) {
            return false;
        }

        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        if (!nodeID) {
            return false;
        }

        return std::string_view(nodeID).find("wall") == std::string_view::npos;
    }

    bool FreeSceneRootSync::ThreadContainsActor(
        OStim::Thread* thread,
        RE::Actor* actor) const
    {
        if (!thread || !actor) {
            return false;
        }

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* gameActor = threadActor ?
                static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (gameActor == actor) {
                return true;
            }
        }
        return false;
    }

    RE::NiAVObject* FreeSceneRootSync::FindAnimatedRoot(RE::Actor* actor)
    {
        if (!actor) {
            return nullptr;
        }

        auto* root = actor->Get3D();
        if (!root) {
            return nullptr;
        }

        return root->GetObjectByName(RE::BSFixedString(kRootNodeName));
    }

    void FreeSceneRootSync::UpdateNodeWorldTransform(RE::NiAVObject* node)
    {
        if (!node) {
            return;
        }

        if (node->parent) {
            node->world.rotate =
                node->local.rotate * node->parent->world.rotate;
            node->world.scale =
                node->local.scale * node->parent->world.scale;
            node->world.translate =
                node->parent->world.rotate *
                    (node->local.translate * node->parent->world.scale) +
                node->parent->world.translate;
        } else {
            node->world = node->local;
        }
    }

    void FreeSceneRootSync::UpdateTreeTransforms(RE::NiAVObject* node)
    {
        if (!node) {
            return;
        }

        UpdateNodeWorldTransform(node);

        if (auto* asNode = node->AsNode()) {
            for (auto& child : asNode->GetChildren()) {
                if (child) {
                    UpdateTreeTransforms(child.get());
                }
            }
        }
    }

    void FreeSceneRootSync::ApplyRootTransform(
        RE::Actor* actor,
        const RootTransform& transform)
    {
        auto* animatedRoot = FindAnimatedRoot(actor);
        if (!animatedRoot || !transform.IsFinite()) {
            return;
        }

        // Deliberately touch only the animated skeleton root. Do not call
        // Actor::SetPosition, TESObjectREFR::SetPosition, TranslateTo or
        // Update3DPosition: STR retains complete ownership of the network
        // reference while this local transform supplies the missing paired-
        // animation displacement for the rendered proxy.
        animatedRoot->local.translate = transform.translate;
        animatedRoot->local.rotate = transform.rotate;
        UpdateTreeTransforms(animatedRoot);
    }

    std::optional<std::string> FreeSceneRootSync::Field(
        std::string_view payload,
        std::string_view key)
    {
        const auto needle = fmt::format("{}=", key);
        std::size_t from = 0;
        while (from < payload.size()) {
            const auto pos = payload.find(needle, from);
            if (pos == std::string_view::npos) {
                return std::nullopt;
            }
            if (pos == 0 || payload[pos - 1] == '|') {
                const auto begin = pos + needle.size();
                const auto end = payload.find('|', begin);
                return std::string(payload.substr(
                    begin,
                    end == std::string_view::npos ?
                        payload.size() - begin : end - begin));
            }
            from = pos + needle.size();
        }
        return std::nullopt;
    }

    void __cdecl FreeSceneRootSync::OnRootMessage(
        const STRPMApi::Message* message,
        void* userData)
    {
        if (message && userData) {
            static_cast<FreeSceneRootSync*>(userData)->
                HandleRootMessage(*message);
        }
    }

    void FreeSceneRootSync::HandleRootMessage(
        const STRPMApi::Message& message)
    {
        if (!message.data ||
            message.size == 0 ||
            message.sender.connectionID == 0) {
            return;
        }

        const auto connectionID = message.sender.connectionID;
        std::string payload(
            static_cast<const char*>(message.data),
            message.size);

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([
                this,
                connectionID,
                payload = std::move(payload)]() {
                StoreIncoming(connectionID, payload);
            });
        }
    }

    void FreeSceneRootSync::StoreIncoming(
        STRPMApi::ConnectionID senderConnectionID,
        std::string_view payload)
    {
        if (!_threads ||
            senderConnectionID == 0 ||
            !payload.starts_with("ROOTBONE|")) {
            return;
        }

        try {
            const auto threadValue = Field(payload, "thread");
            const auto tx = Field(payload, "tx");
            const auto ty = Field(payload, "ty");
            const auto tz = Field(payload, "tz");
            if (!threadValue || !tx || !ty || !tz) {
                return;
            }

            RemoteState state{};
            state.remoteThreadID = static_cast<std::int32_t>(
                std::stol(*threadValue));
            state.transform.translate = {
                std::stof(*tx),
                std::stof(*ty),
                std::stof(*tz)
            };

            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    const auto key = fmt::format("r{}{}", row, column);
                    const auto value = Field(payload, key);
                    if (!value) {
                        return;
                    }
                    state.transform.rotate.entry[row][column] =
                        std::stof(*value);
                }
            }

            if (!state.transform.IsFinite()) {
                SKSE::log::warn(
                    "OSTNET ROOT SYNC RX reject connection={} reason=invalid-transform",
                    senderConnectionID);
                return;
            }

            state.received = std::chrono::steady_clock::now();
            const auto previous = _remoteStates.find(senderConnectionID);
            if (previous != _remoteStates.end()) {
                state.lastLog = previous->second.lastLog;
            }
            _remoteStates[senderConnectionID] = state;
        } catch (...) {
            SKSE::log::warn(
                "OSTNET ROOT SYNC RX reject connection={} reason=parse",
                senderConnectionID);
        }
    }

    void FreeSceneRootSync::Tick()
    {
        if (!_threads || _activePlayerThreadID < 0) {
            return;
        }

        auto* thread = _threads->getThread(_activePlayerThreadID);
        if (!thread || !thread->isPlayerThread()) {
            Reset();
            return;
        }

        if (!IsFreeStandingThread(thread)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        auto* player = RE::PlayerCharacter::GetSingleton();

        if (_transportRunning.load() &&
            _api &&
            _api->send &&
            player &&
            ThreadContainsActor(thread, player)) {
            if (auto* animatedRoot = FindAnimatedRoot(player)) {
                if (_lastSend.time_since_epoch().count() == 0 ||
                    now - _lastSend >= kSendInterval) {
                    RootTransform local{};
                    local.translate = animatedRoot->local.translate;
                    local.rotate = animatedRoot->local.rotate;

                    if (local.IsFinite()) {
                        const auto& r = local.rotate.entry;
                        const auto payload = fmt::format(
                            "ROOTBONE|thread={}|tx={:.6f}|ty={:.6f}|tz={:.6f}|r00={:.7f}|r01={:.7f}|r02={:.7f}|r10={:.7f}|r11={:.7f}|r12={:.7f}|r20={:.7f}|r21={:.7f}|r22={:.7f}",
                            _activePlayerThreadID,
                            local.translate.x,
                            local.translate.y,
                            local.translate.z,
                            r[0][0], r[0][1], r[0][2],
                            r[1][0], r[1][1], r[1][2],
                            r[2][0], r[2][1], r[2][2]);

                        STRPMApi::Target target{};
                        target.kind = STRPMApi::TargetKind::kAllPlayers;
                        const auto result = _api->send(
                            kRootChannel,
                            target,
                            payload.data(),
                            payload.size(),
                            STRPMApi::kMessageNone);

                        _lastSend = now;

                        if (result != STRPMApi::Result::kOk) {
                            if (_lastTransportWarn.time_since_epoch().count() == 0 ||
                                now - _lastTransportWarn >= kLogInterval) {
                                _lastTransportWarn = now;
                                SKSE::log::warn(
                                    "OSTNET ROOT SYNC TX failed result={} thread={} bytes={}",
                                    static_cast<std::uint32_t>(result),
                                    _activePlayerThreadID,
                                    payload.size());
                            }
                        } else if (
                            _lastSendLog.time_since_epoch().count() == 0 ||
                            now - _lastSendLog >= kLogInterval) {
                            _lastSendLog = now;
                            auto* node = thread->getCurrentNode();
                            SKSE::log::info(
                                "OSTNET ROOT SYNC TX thread={} node={} local=({:.3f},{:.3f},{:.3f}) channel={} referenceWrites=0",
                                _activePlayerThreadID,
                                node && node->getNodeID() ? node->getNodeID() : "",
                                local.translate.x,
                                local.translate.y,
                                local.translate.z,
                                kRootChannel);
                        }
                    }
                }
            }
        }

        for (auto it = _remoteStates.begin(); it != _remoteStates.end();) {
            auto& state = it->second;
            if (now - state.received > kRemoteStateLifetime) {
                it = _remoteStates.erase(it);
                continue;
            }

            const auto proxyFormID =
                STRPMTransport::GetSingleton().ResolveProxy(it->first);
            auto* form = proxyFormID ?
                RE::TESForm::LookupByID(*proxyFormID) : nullptr;
            auto* proxy = form ? form->As<RE::Actor>() : nullptr;

            // Scene membership is the authorization boundary for realtime
            // root state. A connected client whose proxy is not an actor in
            // this exact local OStim thread cannot affect any skeleton.
            if (!proxy || !ThreadContainsActor(thread, proxy)) {
                ++it;
                continue;
            }

            RE::NiPoint3 before{};
            bool hadBefore = false;
            if (auto* root = FindAnimatedRoot(proxy)) {
                before = root->local.translate;
                hadBefore = true;
            }

            ApplyRootTransform(proxy, state.transform);

            if (state.lastLog.time_since_epoch().count() == 0 ||
                now - state.lastLog >= kLogInterval) {
                state.lastLog = now;
                RE::NiPoint3 after{};
                bool hadAfter = false;
                if (auto* root = FindAnimatedRoot(proxy)) {
                    after = root->local.translate;
                    hadAfter = true;
                }

                SKSE::log::info(
                    "OSTNET ROOT SYNC APPLY connection={} proxy={:08X} remoteThread={} hadRoot={}/{} before=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) after=({:.3f},{:.3f},{:.3f}) referenceWrites=0",
                    it->first,
                    proxy->GetFormID(),
                    state.remoteThreadID,
                    hadBefore ? 1 : 0,
                    hadAfter ? 1 : 0,
                    before.x,
                    before.y,
                    before.z,
                    state.transform.translate.x,
                    state.transform.translate.y,
                    state.transform.translate.z,
                    after.x,
                    after.y,
                    after.z);
            }

            ++it;
        }
    }
}
