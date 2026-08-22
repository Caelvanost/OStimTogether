#include "PCH.h"
#include "FreeScenePhaseSync.h"

#include "OStimBridge.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr wchar_t kSTRPMModuleName[] = L"STRPluginMessagingAPI.dll";
        constexpr char kChannel[] = "ostimtogether.phase";
        constexpr auto kReadyDelayStart = std::chrono::milliseconds(40);
        constexpr auto kReadyDelayNode = std::chrono::milliseconds(60);
        constexpr auto kReplayDelay = std::chrono::milliseconds(150);
        constexpr auto kProxyReleaseDelay = std::chrono::milliseconds(220);

        void StopReferenceTranslation(RE::TESObjectREFR* object)
        {
            if (!object) {
                return;
            }

            using func_t = void(
                RE::BSScript::IVirtualMachine*,
                RE::VMStackID,
                RE::TESObjectREFR*);

            static REL::Relocation<func_t> func{
                RELOCATION_ID(55712, 56243)
            };

            func(nullptr, 0, object);
        }
    }

    FreeScenePhaseSync& FreeScenePhaseSync::GetSingleton()
    {
        static FreeScenePhaseSync instance;
        return instance;
    }

    FreeScenePhaseSync::~FreeScenePhaseSync()
    {
        StopTransport();
    }

    void FreeScenePhaseSync::StartListener::listen(OStim::Thread* thread)
    {
        FreeScenePhaseSync::GetSingleton().HandleStart(thread);
    }

    void FreeScenePhaseSync::NodeListener::listen(OStim::Thread* thread)
    {
        FreeScenePhaseSync::GetSingleton().HandleNode(thread);
    }

    void FreeScenePhaseSync::StopListener::listen(OStim::Thread* thread)
    {
        FreeScenePhaseSync::GetSingleton().HandleStop(thread);
    }

    std::optional<std::string> FreeScenePhaseSync::Field(
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
                    end == std::string_view::npos ? payload.size() - begin : end - begin));
            }
            from = pos + needle.size();
        }
        return std::nullopt;
    }

    std::optional<std::int32_t> FreeScenePhaseSync::ParseInt(
        std::string_view payload,
        std::string_view key)
    {
        const auto value = Field(payload, key);
        if (!value) {
            return std::nullopt;
        }
        try {
            return static_cast<std::int32_t>(std::stol(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::uint64_t> FreeScenePhaseSync::ParseUInt64(
        std::string_view payload,
        std::string_view key)
    {
        const auto value = Field(payload, key);
        if (!value) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    bool FreeScenePhaseSync::LoadOStimAPIs()
    {
        auto* messaging = SKSE::GetMessagingInterface();
        const auto module = GetModuleHandleW(L"OStim.dll");
        if (!messaging || !module) {
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        if (!messaging->Dispatch(
                OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
                &exchange,
                sizeof(exchange),
                nullptr) ||
            !exchange.interfaceMap) {
            return false;
        }

        auto* base = exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME);
        _threads = base ? static_cast<OStim::ThreadInterface*>(base) : nullptr;
        if (!_threads) {
            return false;
        }
        _threadInterfaceVersion = _threads->getVersion();

        const auto requestThread = reinterpret_cast<OStimModAPI::Thread::RequestAPI>(
            reinterpret_cast<void*>(GetProcAddress(module, "RequestPluginAPI_Thread")));
        if (!requestThread) {
            return false;
        }

        auto* declaration = SKSE::PluginDeclaration::GetSingleton();
        const auto version = declaration ? declaration->GetVersion() : REL::Version{ 0, 29, 0, 0 };
        const auto pluginName = declaration ? std::string(declaration->GetName()) : std::string("OStimTogether");
        _threadControl = requestThread(
            OStimModAPI::Thread::InterfaceVersion::V1,
            pluginName.c_str(),
            version);
        return _threadControl != nullptr;
    }

    bool FreeScenePhaseSync::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads && _threadControl;
        }
        if (!LoadOStimAPIs()) {
            SKSE::log::warn("OSTNET PHASE SYNC unavailable: OStim APIs missing");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET PHASE SYNC READY threadsVersion={} mode=free-scene-barrier nativeAlign=1 skeletonWrites=0 replayDelayMs={}",
            _threadInterfaceVersion,
            kReplayDelay.count());
        return true;
    }

    bool FreeScenePhaseSync::StartTransport()
    {
        if (_transportRunning.load()) {
            return true;
        }

        const auto module = GetModuleHandleW(kSTRPMModuleName);
        if (!module) {
            return false;
        }
        const auto query = reinterpret_cast<STRPMApi::QueryInterfaceFn>(
            GetProcAddress(module, STRPMApi::kQueryInterfaceExportName));
        if (!query) {
            return false;
        }

        const STRPMApi::Interface* api = nullptr;
        const auto queryResult = query(STRPMApi::kInterfaceVersion, &api);
        if (queryResult != STRPMApi::Result::kOk || !api || !api->registerChannel || !api->send) {
            return false;
        }

        STRPMApi::ListenerHandle listener{};
        const auto result = api->registerChannel(kChannel, &FreeScenePhaseSync::OnMessage, this, &listener);
        if (result != STRPMApi::Result::kOk) {
            SKSE::log::warn(
                "OSTNET PHASE SYNC transport register failed result={}",
                static_cast<std::uint32_t>(result));
            return false;
        }

        _api = api;
        _listener = listener;
        _transportRunning.store(true);
        SKSE::log::info(
            "OSTNET PHASE SYNC TRANSPORT READY channel={} reliable=1 ordered=1",
            kChannel);
        return true;
    }

    void FreeScenePhaseSync::StopTransport()
    {
        if (!_transportRunning.exchange(false)) {
            return;
        }
        if (_api && _api->unregisterChannel && _listener.value != 0) {
            _api->unregisterChannel(_listener);
        }
        _listener = {};
        _api = nullptr;
        Reset();
    }

    void FreeScenePhaseSync::Reset()
    {
        _ownerPhase.reset();
        _remotePreps.clear();
        _lastReadyToken.clear();
        _startedThreads.clear();
    }

    bool FreeScenePhaseSync::IsDynamicSTRProxy(RE::Actor* actor) const
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

    bool FreeScenePhaseSync::ThreadContainsActor(OStim::Thread* thread, RE::Actor* actor) const
    {
        if (!thread || !actor) {
            return false;
        }
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* gameActor = threadActor ? static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (gameActor == actor) {
                return true;
            }
        }
        return false;
    }

    bool FreeScenePhaseSync::IsFreeStandingThread(OStim::Thread* thread) const
    {
        if (!thread || !thread->isPlayerThread() || _threadInterfaceVersion < 3) {
            return false;
        }
        if (thread->getFurnitureObject()) {
            return false;
        }
        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        return nodeID && std::string_view(nodeID).find("wall") == std::string_view::npos;
    }

    std::unordered_set<STRPMApi::ConnectionID> FreeScenePhaseSync::ResolveParticipants(OStim::Thread* thread) const
    {
        std::unordered_set<STRPMApi::ConnectionID> result;
        if (!thread) {
            return result;
        }
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ? static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
            if (!IsDynamicSTRProxy(actor)) {
                continue;
            }
            if (const auto connection = STRPMTransport::GetSingleton().ResolveConnection(actor->GetFormID())) {
                result.insert(*connection);
            }
        }
        return result;
    }

    bool FreeScenePhaseSync::SendTo(
        STRPMApi::ConnectionID connectionID,
        std::string_view payload)
    {
        if (!_transportRunning.load() || !_api || !_api->send || connectionID == 0 || payload.empty()) {
            return false;
        }
        STRPMApi::Target target{};
        target.kind = STRPMApi::TargetKind::kPlayer;
        target.connectionID = connectionID;
        const auto result = _api->send(
            kChannel,
            target,
            payload.data(),
            payload.size(),
            STRPMApi::kMessageReliable | STRPMApi::kMessageOrdered);
        return result == STRPMApi::Result::kOk;
    }

    void FreeScenePhaseSync::BeginOwnerPhase(OStim::Thread* thread, std::string_view reason)
    {
        if (!IsFreeStandingThread(thread) || OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(thread->getThreadID())) {
            return;
        }
        auto participants = ResolveParticipants(thread);
        if (participants.empty()) {
            return;
        }

        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        if (!nodeID) {
            return;
        }

        OwnerPhase phase{};
        phase.threadID = thread->getThreadID();
        phase.token = _nextToken++;
        phase.nodeID = nodeID;
        phase.reason = std::string(reason);
        phase.expected = std::move(participants);
        _ownerPhase = phase;

        const auto payload = fmt::format(
            "PHASE_PREP|thread={}|token={}|node={}|reason={}",
            phase.threadID,
            phase.token,
            phase.nodeID,
            phase.reason);
        for (const auto connectionID : phase.expected) {
            SendTo(connectionID, payload);
        }

        SKSE::log::info(
            "OSTNET PHASE PREP TX thread={} token={} node={} reason={} participants={}",
            phase.threadID,
            phase.token,
            phase.nodeID,
            phase.reason,
            phase.expected.size());
    }

    void FreeScenePhaseSync::HandleStart(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _startedThreads.insert(threadID);

        if (!IsFreeStandingThread(thread)) {
            return;
        }

        if (OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(threadID)) {
            MaybeReadyRemotePhase(thread, kReadyDelayStart);
        } else {
            BeginOwnerPhase(thread, "START");
        }
    }

    void FreeScenePhaseSync::HandleNode(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        const auto threadID = thread->getThreadID();
        if (!_startedThreads.contains(threadID)) {
            SKSE::log::trace(
                "OSTNET PHASE NODE ignored thread={} reason=before-thread-start",
                threadID);
            return;
        }

        if (!IsFreeStandingThread(thread)) {
            return;
        }
        if (OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(threadID)) {
            MaybeReadyRemotePhase(thread, kReadyDelayNode);
        } else {
            BeginOwnerPhase(thread, "NODE");
        }
    }

    void FreeScenePhaseSync::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }
        const auto tid = thread->getThreadID();
        _startedThreads.erase(tid);
        if (_ownerPhase && _ownerPhase->threadID == tid) {
            _ownerPhase.reset();
        }

        if (thread->isPlayerThread()) {
            _remotePreps.clear();
            _lastReadyToken.clear();
        }
    }

    void FreeScenePhaseSync::MaybeReadyRemotePhase(
        OStim::Thread* thread,
        std::chrono::milliseconds delay)
    {
        if (!thread || !OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(thread->getThreadID())) {
            return;
        }
        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        if (!nodeID) {
            return;
        }

        for (const auto& prep : _remotePreps) {
            if (prep.nodeID != nodeID) {
                continue;
            }
            const auto proxyForm = STRPMTransport::GetSingleton().ResolveProxy(prep.ownerConnectionID);
            auto* form = proxyForm ? RE::TESForm::LookupByID(*proxyForm) : nullptr;
            auto* proxy = form ? form->As<RE::Actor>() : nullptr;
            if (proxy && ThreadContainsActor(thread, proxy)) {
                QueueReady(thread->getThreadID(), prep, delay);
            }
        }
    }

    void FreeScenePhaseSync::QueueReady(
        std::int32_t localThreadID,
        RemotePrep prep,
        std::chrono::milliseconds delay)
    {
        const auto readyKey = fmt::format("{}|{}", prep.ownerConnectionID, prep.ownerThreadID);
        const auto it = _lastReadyToken.find(readyKey);
        if (it != _lastReadyToken.end() && it->second == prep.token) {
            return;
        }
        _lastReadyToken[readyKey] = prep.token;

        std::thread([this, localThreadID, prep = std::move(prep), delay]() mutable {
            std::this_thread::sleep_for(delay);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, localThreadID, prep = std::move(prep)]() {
                    if (!_threads || !_threadControl ||
                        !_threadControl->IsThreadValid(static_cast<std::uint32_t>(localThreadID))) {
                        return;
                    }
                    auto* thread = _threads->getThread(localThreadID);
                    if (!thread || !IsFreeStandingThread(thread) ||
                        !_startedThreads.contains(localThreadID) ||
                        !OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(localThreadID)) {
                        return;
                    }
                    auto* node = thread->getCurrentNode();
                    const auto* nodeID = node ? node->getNodeID() : nullptr;
                    if (!nodeID || prep.nodeID != nodeID) {
                        return;
                    }
                    const auto proxyForm = STRPMTransport::GetSingleton().ResolveProxy(prep.ownerConnectionID);
                    auto* form = proxyForm ? RE::TESForm::LookupByID(*proxyForm) : nullptr;
                    auto* proxy = form ? form->As<RE::Actor>() : nullptr;
                    if (!proxy || !ThreadContainsActor(thread, proxy)) {
                        return;
                    }
                    const auto speed = _threadControl->GetCurrentSpeed(static_cast<std::uint32_t>(localThreadID));
                    const auto payload = fmt::format(
                        "PHASE_READY|thread={}|token={}|node={}|speed={}",
                        prep.ownerThreadID,
                        prep.token,
                        prep.nodeID,
                        speed);
                    SendTo(prep.ownerConnectionID, payload);
                    SKSE::log::info(
                        "OSTNET PHASE READY TX localThread={} ownerThread={} token={} node={} speed={}",
                        localThreadID,
                        prep.ownerThreadID,
                        prep.token,
                        prep.nodeID,
                        speed);
                });
            }
        }).detach();
    }

    void __cdecl FreeScenePhaseSync::OnMessage(const STRPMApi::Message* message, void* userData)
    {
        if (message && userData) {
            static_cast<FreeScenePhaseSync*>(userData)->HandleMessage(*message);
        }
    }

    void FreeScenePhaseSync::HandleMessage(const STRPMApi::Message& message)
    {
        if (!message.data || message.size == 0 || message.sender.connectionID == 0) {
            return;
        }
        const auto sender = message.sender.connectionID;
        std::string payload(static_cast<const char*>(message.data), message.size);
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([this, sender, payload = std::move(payload)]() mutable {
                HandleMessageGameThread(sender, std::move(payload));
            });
        }
    }

    void FreeScenePhaseSync::HandleMessageGameThread(
        STRPMApi::ConnectionID senderConnectionID,
        std::string payload)
    {
        if (payload.starts_with("PHASE_PREP|")) {
            const auto ownerThread = ParseInt(payload, "thread");
            const auto token = ParseUInt64(payload, "token");
            const auto node = Field(payload, "node");
            const auto reason = Field(payload, "reason");
            if (!ownerThread || !token || !node) {
                return;
            }
            RemotePrep prep{
                senderConnectionID,
                *ownerThread,
                *token,
                *node,
                reason.value_or("UNKNOWN") };

            _remotePreps.erase(
                std::remove_if(
                    _remotePreps.begin(),
                    _remotePreps.end(),
                    [&](const RemotePrep& existing) {
                        return existing.ownerConnectionID == senderConnectionID &&
                               existing.ownerThreadID == *ownerThread;
                    }),
                _remotePreps.end());
            _remotePreps.push_back(prep);

            if (_threads && _threadControl) {
                const auto playerThreadID =
                    static_cast<std::int32_t>(_threadControl->GetPlayerThreadID());
                if (playerThreadID >= 0 && _startedThreads.contains(playerThreadID)) {
                    auto* thread = _threads->getThread(playerThreadID);
                    if (thread && IsFreeStandingThread(thread) &&
                        OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(playerThreadID)) {
                        MaybeReadyRemotePhase(thread, prep.reason == "START" ? kReadyDelayStart : kReadyDelayNode);
                    }
                }
            }
            return;
        }

        if (payload.starts_with("PHASE_READY|")) {
            HandleReady(senderConnectionID, payload);
            return;
        }

        if (payload.starts_with("PHASE_COMMIT|")) {
            const auto ownerThread = ParseInt(payload, "thread");
            const auto token = ParseUInt64(payload, "token");
            const auto node = Field(payload, "node");
            const auto speed = ParseInt(payload, "speed");
            const auto delayMs = ParseInt(payload, "delayMs");
            if (!ownerThread || !token || !node || !speed || !delayMs || !_threads || !_threadControl) {
                return;
            }

            const auto localThreadID = static_cast<std::int32_t>(_threadControl->GetPlayerThreadID());
            if (localThreadID < 0 || !_startedThreads.contains(localThreadID)) {
                return;
            }
            auto* thread = _threads->getThread(localThreadID);
            const auto proxyForm = STRPMTransport::GetSingleton().ResolveProxy(senderConnectionID);
            auto* form = proxyForm ? RE::TESForm::LookupByID(*proxyForm) : nullptr;
            auto* proxy = form ? form->As<RE::Actor>() : nullptr;
            if (!thread || !proxy || !ThreadContainsActor(thread, proxy) ||
                !OStimBridge::GetSingleton().IsRemoteMirrorForAlignment(localThreadID)) {
                return;
            }

            QueueReplay(
                localThreadID,
                *token,
                *node,
                *speed,
                std::chrono::milliseconds(std::max(0, *delayMs)),
                true);
            return;
        }
    }

    void FreeScenePhaseSync::HandleReady(
        STRPMApi::ConnectionID senderConnectionID,
        std::string_view payload)
    {
        const auto threadID = ParseInt(payload, "thread");
        const auto token = ParseUInt64(payload, "token");
        const auto node = Field(payload, "node");
        if (!_ownerPhase || !threadID || !token || !node ||
            _ownerPhase->threadID != *threadID ||
            _ownerPhase->token != *token ||
            _ownerPhase->nodeID != *node ||
            !_ownerPhase->expected.contains(senderConnectionID) ||
            _ownerPhase->committed) {
            return;
        }

        _ownerPhase->ready.insert(senderConnectionID);
        SKSE::log::info(
            "OSTNET PHASE READY RX thread={} token={} node={} connection={} ready={}/{}",
            _ownerPhase->threadID,
            _ownerPhase->token,
            _ownerPhase->nodeID,
            senderConnectionID,
            _ownerPhase->ready.size(),
            _ownerPhase->expected.size());

        if (_ownerPhase->ready.size() == _ownerPhase->expected.size()) {
            CommitOwnerPhase();
        }
    }

    void FreeScenePhaseSync::CommitOwnerPhase()
    {
        if (!_ownerPhase || _ownerPhase->committed || !_threadControl || !_threads) {
            return;
        }
        auto& phase = *_ownerPhase;
        if (!_startedThreads.contains(phase.threadID) ||
            !_threadControl->IsThreadValid(static_cast<std::uint32_t>(phase.threadID))) {
            return;
        }
        auto* thread = _threads->getThread(phase.threadID);
        if (!thread || !IsFreeStandingThread(thread)) {
            return;
        }
        auto* node = thread->getCurrentNode();
        const auto* nodeID = node ? node->getNodeID() : nullptr;
        if (!nodeID || phase.nodeID != nodeID) {
            return;
        }

        phase.committed = true;
        const auto speed = _threadControl->GetCurrentSpeed(static_cast<std::uint32_t>(phase.threadID));
        const auto payload = fmt::format(
            "PHASE_COMMIT|thread={}|token={}|node={}|speed={}|delayMs={}",
            phase.threadID,
            phase.token,
            phase.nodeID,
            speed,
            kReplayDelay.count());
        for (const auto connectionID : phase.expected) {
            SendTo(connectionID, payload);
        }

        QueueReplay(
            phase.threadID,
            phase.token,
            phase.nodeID,
            speed,
            kReplayDelay,
            false);

        SKSE::log::info(
            "OSTNET PHASE COMMIT TX thread={} token={} node={} speed={} participants={} delayMs={} action=native-realign-replay",
            phase.threadID,
            phase.token,
            phase.nodeID,
            speed,
            phase.expected.size(),
            kReplayDelay.count());
    }

    void FreeScenePhaseSync::QueueReplay(
        std::int32_t localThreadID,
        std::uint64_t token,
        std::string nodeID,
        std::int32_t speed,
        std::chrono::milliseconds delay,
        bool mirror)
    {
        std::thread([this, localThreadID, token, nodeID = std::move(nodeID), speed, delay, mirror]() mutable {
            std::this_thread::sleep_for(delay);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, localThreadID, token, nodeID = std::move(nodeID), speed, mirror]() {
                    ReplayNow(localThreadID, token, nodeID, speed, mirror);
                });
            }
        }).detach();
    }

    void FreeScenePhaseSync::ReplayNow(
        std::int32_t localThreadID,
        std::uint64_t token,
        std::string_view nodeID,
        std::int32_t speed,
        bool mirror)
    {
        if (!_threads || !_threadControl ||
            !_startedThreads.contains(localThreadID) ||
            !_threadControl->IsThreadValid(static_cast<std::uint32_t>(localThreadID))) {
            return;
        }
        auto* thread = _threads->getThread(localThreadID);
        if (!thread || !IsFreeStandingThread(thread)) {
            return;
        }
        auto* node = thread->getCurrentNode();
        const auto* currentNode = node ? node->getNodeID() : nullptr;
        if (!currentNode || nodeID != currentNode) {
            SKSE::log::info(
                "OSTNET PHASE REPLAY DROP localThread={} token={} expectedNode={} currentNode={} reason=node-changed",
                localThreadID,
                token,
                nodeID,
                currentNode ? currentNode : "");
            return;
        }

        const auto tid = static_cast<std::uint32_t>(localThreadID);
        const auto actorCount = _threadControl->GetActorCount(tid);
        std::uint32_t aligned = 0;
        for (std::uint32_t i = 0; i < actorCount; ++i) {
            OStimModAPI::Thread::ActorAlignmentData alignment{};
            if (_threadControl->GetActorAlignment(tid, i, &alignment) &&
                _threadControl->SetActorAlignment(tid, i, &alignment) == OStimModAPI::Thread::APIResult::OK) {
                ++aligned;
            }
        }

        const auto speedResult = _threadControl->SetSpeed(tid, speed);
        QueueProxyTranslationRelease(localThreadID, token);

        SKSE::log::info(
            "OSTNET PHASE REPLAY localThread={} token={} node={} mirror={} aligned={}/{} speed={} speedResult={} directPositionWrites=0 skeletonWrites=0",
            localThreadID,
            token,
            currentNode,
            mirror ? 1 : 0,
            aligned,
            actorCount,
            speed,
            static_cast<int>(speedResult));
    }

    void FreeScenePhaseSync::QueueProxyTranslationRelease(
        std::int32_t localThreadID,
        std::uint64_t token)
    {
        std::thread([this, localThreadID, token]() {
            std::this_thread::sleep_for(kProxyReleaseDelay);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, localThreadID, token]() {
                    if (!_threads || !_threadControl ||
                        !_startedThreads.contains(localThreadID) ||
                        !_threadControl->IsThreadValid(static_cast<std::uint32_t>(localThreadID))) {
                        return;
                    }
                    auto* thread = _threads->getThread(localThreadID);
                    if (!thread || !IsFreeStandingThread(thread)) {
                        return;
                    }
                    std::uint32_t released = 0;
                    for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                        auto* threadActor = thread->getActor(i);
                        auto* actor = threadActor ? static_cast<RE::Actor*>(threadActor->getGameActor()) : nullptr;
                        if (IsDynamicSTRProxy(actor)) {
                            StopReferenceTranslation(actor);
                            ++released;
                        }
                    }
                    SKSE::log::info(
                        "OSTNET PHASE RELEASE localThread={} token={} proxies={} action=stop-translation-only directPositionWrites=0",
                        localThreadID,
                        token,
                        released);
                });
            }
        }).detach();
    }
}
