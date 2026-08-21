#include "PCH.h"
#include "CoopSessionManager.h"

#include "SafeMessageBox.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"

namespace OStimTogether
{
    namespace
    {
        constexpr const char* kPluginName = "OStimTogether";
        constexpr auto kConsentTimeout = std::chrono::seconds(30);
        constexpr auto kRestartRetryDelay = std::chrono::milliseconds(100);

        std::string GetNodeID(OStim::Thread* thread)
        {
            if (!thread) {
                return {};
            }
            auto* node = thread->getCurrentNode();
            const auto* id = node ? node->getNodeID() : nullptr;
            return id ? id : "";
        }
    }

    CoopSessionManager& CoopSessionManager::GetSingleton()
    {
        static CoopSessionManager instance;
        return instance;
    }

    void CoopSessionManager::StartListener::listen(OStim::Thread* thread)
    {
        CoopSessionManager::GetSingleton().HandleThreadStart(thread);
    }

    void CoopSessionManager::NodeListener::listen(OStim::Thread* thread)
    {
        CoopSessionManager::GetSingleton().HandleThreadNode(thread);
    }

    void CoopSessionManager::SpeedListener::listen(OStim::Thread* thread)
    {
        CoopSessionManager::GetSingleton().HandleThreadSpeed(thread);
    }

    void CoopSessionManager::StopListener::listen(OStim::Thread* thread)
    {
        CoopSessionManager::GetSingleton().HandleThreadStop(thread);
    }

    std::optional<std::string> CoopSessionManager::Field(
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

    std::optional<std::int32_t> CoopSessionManager::ThreadID(
        std::string_view payload)
    {
        const auto value = Field(payload, "thread");
        if (!value) {
            return std::nullopt;
        }
        try {
            return static_cast<std::int32_t>(std::stol(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::uint64_t> CoopSessionManager::SessionID(
        std::string_view payload)
    {
        const auto value = Field(payload, "session");
        if (!value) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string CoopSessionManager::SafeLabel(std::string_view value)
    {
        std::string result(value);
        for (auto& ch : result) {
            if (ch == '|' || ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        return result;
    }

    std::string CoopSessionManager::MirrorKey(
        STRPMApi::ConnectionID ownerConnectionID,
        std::int32_t ownerThreadID)
    {
        return fmt::format("{}|{}", ownerConnectionID, ownerThreadID);
    }

    bool CoopSessionManager::LoadOStimAPIs()
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

        auto* declaration = SKSE::PluginDeclaration::GetSingleton();
        const auto version = declaration ? declaration->GetVersion() : REL::Version{ 0, 24, 0, 0 };
        const auto pluginName = declaration ? std::string(declaration->GetName()) : std::string(kPluginName);

        const auto requestThread = reinterpret_cast<OStimModAPI::Thread::RequestAPI>(
            reinterpret_cast<void*>(GetProcAddress(module, "RequestPluginAPI_Thread")));
        const auto requestScene = reinterpret_cast<OStimModAPI::Scene::RequestAPI>(
            reinterpret_cast<void*>(GetProcAddress(module, "RequestPluginAPI_Scene")));

        if (requestThread) {
            _threadControl = requestThread(
                OStimModAPI::Thread::InterfaceVersion::V1,
                pluginName.c_str(),
                version);
        }
        if (requestScene) {
            _sceneControl = requestScene(
                OStimModAPI::Scene::InterfaceVersion::V1,
                pluginName.c_str(),
                version);
        }

        return _threads && _threadControl && _sceneControl;
    }

    bool CoopSessionManager::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads && _threadControl && _sceneControl;
        }
        if (!LoadOStimAPIs()) {
            SKSE::log::error("OSTNET COOP unavailable: OStim thread/scene APIs missing");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerSpeedChangedListener(&_speedListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET COOP READY consent=safe-messagebox preflight=1 sharedControls=1 stopAnyParticipant=1 threadsVersion={}",
            _threads->getVersion());
        return true;
    }

    void CoopSessionManager::Reset()
    {
        std::scoped_lock lock(_mutex);
        _ownerSessions.clear();
        _pendingOwnerByThread.clear();
        _activeOwnerByThread.clear();
        _approvedReplayArmed.reset();
        _pendingMirrorStarts.clear();
        _mirrorRoutes.clear();
        _mirrorByRemote.clear();
        _mirrorSuppressions.clear();
    }

    bool CoopSessionManager::IsDynamicSTRProxy(RE::Actor* actor) const
    {
        if (!actor || actor->IsPlayerRef()) {
            return false;
        }
        auto* base = actor->GetActorBase();
        if (!base) {
            return false;
        }
        constexpr RE::FormID kDynamicMask = 0xFF000000;
        return
            (actor->GetFormID() & kDynamicMask) == kDynamicMask &&
            (base->GetFormID() & kDynamicMask) == kDynamicMask;
    }

    std::unordered_set<STRPMApi::ConnectionID>
        CoopSessionManager::ResolveRemoteParticipants(OStim::Thread* thread) const
    {
        std::unordered_set<STRPMApi::ConnectionID> result;
        if (!thread || !thread->isPlayerThread()) {
            return result;
        }

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!IsDynamicSTRProxy(actor)) {
                continue;
            }
            if (const auto connection =
                    STRPMTransport::GetSingleton().ResolveConnection(actor->GetFormID())) {
                result.insert(*connection);
            }
        }
        return result;
    }

    std::optional<CoopSessionManager::PendingMirrorStart>
        CoopSessionManager::TakePendingMirrorStart(
            OStim::Thread* thread,
            std::string_view nodeID)
    {
        if (!thread || !thread->isPlayerThread()) {
            return std::nullopt;
        }

        const auto now = std::chrono::steady_clock::now();
        _pendingMirrorStarts.erase(
            std::remove_if(
                _pendingMirrorStarts.begin(),
                _pendingMirrorStarts.end(),
                [now](const PendingMirrorStart& entry) {
                    return now - entry.created > std::chrono::seconds(5);
                }),
            _pendingMirrorStarts.end());

        auto it = std::find_if(
            _pendingMirrorStarts.begin(),
            _pendingMirrorStarts.end(),
            [nodeID](const PendingMirrorStart& entry) {
                return entry.nodeID.empty() || nodeID.empty() || entry.nodeID == nodeID;
            });
        if (it == _pendingMirrorStarts.end()) {
            return std::nullopt;
        }

        const auto result = *it;
        _pendingMirrorStarts.erase(it);
        return result;
    }

    void CoopSessionManager::BeginOwnerPreflight(
        OStim::Thread* thread,
        std::unordered_set<STRPMApi::ConnectionID> participants,
        std::string nodeID)
    {
        if (!thread || participants.empty()) {
            return;
        }

        OwnerSession session{};
        session.sessionID = _nextSessionID.fetch_add(1);
        session.preflightThreadID = thread->getThreadID();
        session.nodeID = std::move(nodeID);
        session.participants = std::move(participants);

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                session.actorFormIDs.push_back(actor->GetFormID());
            }
        }

        if (_threads && _threads->getVersion() >= 3) {
            auto* furniture = static_cast<RE::TESObjectREFR*>(thread->getFurnitureObject());
            if (furniture) {
                session.furnitureFormID = furniture->GetFormID();
            }
        }

        const auto sessionID = session.sessionID;
        const auto preflightThreadID = session.preflightThreadID;
        const auto participantCount = session.participants.size();

        {
            std::scoped_lock lock(_mutex);
            _ownerSessions[sessionID] = std::move(session);
            _pendingOwnerByThread[preflightThreadID] = sessionID;
        }

        SKSE::log::info(
            "OSTNET COOP PREFLIGHT CAPTURE session={} thread={} participants={} node={} action=stop-before-consent",
            sessionID,
            preflightThreadID,
            participantCount,
            _ownerSessions[sessionID].nodeID);

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([this, sessionID]() {
                StopPreflightAndInvite(sessionID);
            });
        }
    }

    void CoopSessionManager::StopPreflightAndInvite(std::uint64_t sessionID)
    {
        OwnerSession snapshot{};
        {
            std::scoped_lock lock(_mutex);
            const auto it = _ownerSessions.find(sessionID);
            if (it == _ownerSessions.end() || it->second.canceled) {
                return;
            }
            snapshot = it->second;
            it->second.invitesSent = true;
        }

        if (_sceneControl && snapshot.preflightThreadID >= 0) {
            const auto result = _sceneControl->StopScene(
                kPluginName,
                static_cast<std::uint32_t>(snapshot.preflightThreadID));
            SKSE::log::info(
                "OSTNET COOP PREFLIGHT STOP session={} thread={} result={}",
                sessionID,
                snapshot.preflightThreadID,
                static_cast<int>(result));
        }

        std::string inviter = "Player";
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            const auto* name = player->GetName();
            if (name && *name) {
                inviter = SafeLabel(name);
            }
        }

        for (const auto connectionID : snapshot.participants) {
            STRPMTransport::GetSingleton().SendTo(
                connectionID,
                fmt::format(
                    "INVITE|session={}|inviter={}|node={}",
                    sessionID,
                    inviter,
                    SafeLabel(snapshot.nodeID)));
        }

        SKSE::log::info(
            "OSTNET COOP INVITE TX session={} participants={} node={} localSceneActive=0",
            sessionID,
            snapshot.participants.size(),
            snapshot.nodeID);
        QueueOwnerTimeout(sessionID);
    }

    void CoopSessionManager::QueueOwnerTimeout(std::uint64_t sessionID)
    {
        std::thread([this, sessionID]() {
            std::this_thread::sleep_for(kConsentTimeout);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, sessionID]() {
                    bool timeout = false;
                    {
                        std::scoped_lock lock(_mutex);
                        const auto it = _ownerSessions.find(sessionID);
                        timeout = it != _ownerSessions.end() &&
                                  !it->second.active &&
                                  !it->second.restarting &&
                                  !it->second.canceled;
                    }
                    if (timeout) {
                        CancelOwnerSession(sessionID, "timeout");
                    }
                });
            }
        }).detach();
    }

    void CoopSessionManager::ShowInviteMessageBox(
        STRPMApi::ConnectionID ownerConnectionID,
        std::uint64_t sessionID,
        std::string sender)
    {
        const auto senderLabel = SafeLabel(sender);
        SKSE::log::info(
            "OSTNET COOP INVITE MESSAGEBOX ownerConnection={} session={} sender=\"{}\"",
            ownerConnectionID,
            sessionID,
            senderLabel);

        ShowSafeMessageBox(
            fmt::format("{} wants to start an OStim scene with you.", senderLabel),
            "Accept",
            "Decline",
            [this, ownerConnectionID, sessionID](unsigned int choice) {
                SendInviteResponse(ownerConnectionID, sessionID, choice == 0);
            });
    }

    void CoopSessionManager::SendInviteResponse(
        STRPMApi::ConnectionID ownerConnectionID,
        std::uint64_t sessionID,
        bool accepted)
    {
        STRPMTransport::GetSingleton().SendTo(
            ownerConnectionID,
            fmt::format(
                "INVITE_RESPONSE|session={}|accepted={}",
                sessionID,
                accepted ? 1 : 0));
        SKSE::log::info(
            "OSTNET COOP INVITE RESPONSE TX ownerConnection={} session={} accepted={} source=safe-messagebox",
            ownerConnectionID,
            sessionID,
            accepted ? 1 : 0);
    }

    void CoopSessionManager::HandleInviteResponse(
        STRPMApi::ConnectionID participantConnectionID,
        std::uint64_t sessionID,
        bool accepted)
    {
        bool start = false;
        bool cancel = false;
        {
            std::scoped_lock lock(_mutex);
            const auto it = _ownerSessions.find(sessionID);
            if (it == _ownerSessions.end() ||
                it->second.canceled ||
                it->second.active ||
                !it->second.participants.contains(participantConnectionID)) {
                return;
            }

            if (!accepted) {
                it->second.canceled = true;
                cancel = true;
            } else {
                it->second.accepted.insert(participantConnectionID);
                start = it->second.accepted.size() == it->second.participants.size();
                if (start) {
                    it->second.restarting = true;
                }
            }
        }

        SKSE::log::info(
            "OSTNET COOP INVITE RESPONSE RX session={} connection={} accepted={} start={}",
            sessionID,
            participantConnectionID,
            accepted ? 1 : 0,
            start ? 1 : 0);

        if (cancel) {
            CancelOwnerSession(sessionID, "declined");
        } else if (start) {
            StartApprovedOwnerSession(sessionID);
        }
    }

    void CoopSessionManager::StartApprovedOwnerSession(std::uint64_t sessionID)
    {
        OwnerSession snapshot{};
        {
            std::scoped_lock lock(_mutex);
            const auto it = _ownerSessions.find(sessionID);
            if (it == _ownerSessions.end() || it->second.canceled) {
                return;
            }
            snapshot = it->second;
        }

        if (_threadControl &&
            snapshot.preflightThreadID >= 0 &&
            _threadControl->IsThreadValid(
                static_cast<std::uint32_t>(snapshot.preflightThreadID))) {
            RetryStartApprovedOwnerSession(sessionID);
            return;
        }

        std::array<RE::Actor*, 256> actors{};
        std::size_t actorCount = 0;
        for (const auto formID : snapshot.actorFormIDs) {
            auto* form = RE::TESForm::LookupByID(formID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor || actorCount >= actors.size() - 1) {
                CancelOwnerSession(sessionID, "actor-missing");
                return;
            }
            actors[actorCount++] = actor;
        }

        RE::TESObjectREFR* furniture = nullptr;
        if (snapshot.furnitureFormID != 0) {
            auto* form = RE::TESForm::LookupByID(snapshot.furnitureFormID);
            furniture = form ? form->As<RE::TESObjectREFR>() : nullptr;
        }

        {
            std::scoped_lock lock(_mutex);
            _approvedReplayArmed = sessionID;
        }

        std::uint32_t newThreadID = 0;
        const auto result = _sceneControl->StartScene(
            kPluginName,
            furniture,
            snapshot.nodeID.c_str(),
            actors.data(),
            &newThreadID);

        SKSE::log::info(
            "OSTNET COOP APPROVED START session={} requestedNode={} actors={} furniture={:08X} result={} returnedThread={}",
            sessionID,
            snapshot.nodeID,
            actorCount,
            snapshot.furnitureFormID,
            static_cast<int>(result),
            newThreadID);

        if (result != OStimModAPI::Scene::APIResult::OK) {
            std::scoped_lock lock(_mutex);
            if (_approvedReplayArmed && *_approvedReplayArmed == sessionID) {
                _approvedReplayArmed.reset();
            }
            auto it = _ownerSessions.find(sessionID);
            if (it != _ownerSessions.end()) {
                it->second.canceled = true;
            }
        }
    }

    void CoopSessionManager::RetryStartApprovedOwnerSession(std::uint64_t sessionID)
    {
        std::thread([this, sessionID]() {
            std::this_thread::sleep_for(kRestartRetryDelay);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, sessionID]() {
                    StartApprovedOwnerSession(sessionID);
                });
            }
        }).detach();
    }

    void CoopSessionManager::CancelOwnerSession(
        std::uint64_t sessionID,
        std::string_view reason)
    {
        OwnerSession snapshot{};
        {
            std::scoped_lock lock(_mutex);
            const auto it = _ownerSessions.find(sessionID);
            if (it == _ownerSessions.end()) {
                return;
            }
            it->second.canceled = true;
            snapshot = it->second;
            if (snapshot.preflightThreadID >= 0) {
                _pendingOwnerByThread.erase(snapshot.preflightThreadID);
            }
            if (snapshot.activeThreadID >= 0) {
                _activeOwnerByThread.erase(snapshot.activeThreadID);
            }
        }

        for (const auto connectionID : snapshot.participants) {
            STRPMTransport::GetSingleton().SendTo(
                connectionID,
                fmt::format(
                    "INVITE_CANCEL|session={}|reason={}",
                    sessionID,
                    SafeLabel(reason)));
        }
        SKSE::log::info(
            "OSTNET COOP CANCEL session={} reason={}",
            sessionID,
            reason);
    }

    bool CoopSessionManager::RouteOwnerPayload(std::string_view payload)
    {
        const auto threadID = ThreadID(payload);
        if (!threadID) {
            return false;
        }

        std::uint64_t sessionID = 0;
        bool pending = false;
        OwnerSession snapshot{};
        {
            std::scoped_lock lock(_mutex);
            const auto pendingIt = _pendingOwnerByThread.find(*threadID);
            if (pendingIt != _pendingOwnerByThread.end()) {
                sessionID = pendingIt->second;
                pending = true;
            } else {
                const auto activeIt = _activeOwnerByThread.find(*threadID);
                if (activeIt != _activeOwnerByThread.end()) {
                    sessionID = activeIt->second;
                }
            }

            if (sessionID == 0) {
                return false;
            }
            const auto sessionIt = _ownerSessions.find(sessionID);
            if (sessionIt == _ownerSessions.end()) {
                return false;
            }
            snapshot = sessionIt->second;

            if (pending && payload.starts_with("STOP|")) {
                _pendingOwnerByThread.erase(*threadID);
            }
        }

        if (pending || !snapshot.active || snapshot.canceled) {
            SKSE::log::trace(
                "OSTNET COOP suppress preflight TX session={} thread={} payload={}",
                sessionID,
                *threadID,
                payload.substr(0, payload.find('|')));
            return true;
        }

        for (const auto connectionID : snapshot.participants) {
            STRPMTransport::GetSingleton().SendTo(connectionID, payload);
        }
        return true;
    }

    bool CoopSessionManager::InterceptOutgoing(std::string_view payload)
    {
        if (payload.starts_with("START|") ||
            payload.starts_with("NODE|") ||
            payload.starts_with("SPEED|") ||
            payload.starts_with("STOP|")) {
            return RouteOwnerPayload(payload);
        }
        return false;
    }

    bool CoopSessionManager::HandleControlRequest(
        STRPMApi::ConnectionID senderConnectionID,
        std::string_view payload)
    {
        const auto threadID = ThreadID(payload);
        if (!threadID) {
            return true;
        }

        OwnerSession snapshot{};
        bool authorized = false;
        {
            std::scoped_lock lock(_mutex);
            const auto activeIt = _activeOwnerByThread.find(*threadID);
            if (activeIt != _activeOwnerByThread.end()) {
                const auto sessionIt = _ownerSessions.find(activeIt->second);
                if (sessionIt != _ownerSessions.end()) {
                    snapshot = sessionIt->second;
                    authorized = snapshot.active &&
                                 !snapshot.canceled &&
                                 snapshot.participants.contains(senderConnectionID);
                }
            }
        }

        if (!authorized) {
            SKSE::log::warn(
                "OSTNET COOP CONTROL reject connection={} thread={} payload={}",
                senderConnectionID,
                *threadID,
                payload);
            return true;
        }

        if (payload.starts_with("CONTROL_NODE|")) {
            const auto node = Field(payload, "node");
            if (node && _threadControl) {
                const auto result = _threadControl->NavigateToScene(
                    static_cast<std::uint32_t>(*threadID),
                    node->c_str());
                SKSE::log::info(
                    "OSTNET COOP CONTROL NODE connection={} thread={} node={} result={}",
                    senderConnectionID,
                    *threadID,
                    *node,
                    static_cast<int>(result));
            }
            return true;
        }

        if (payload.starts_with("CONTROL_SPEED|")) {
            const auto speedValue = Field(payload, "speed");
            if (speedValue && _threadControl) {
                try {
                    const auto speed = static_cast<std::int32_t>(std::stol(*speedValue));
                    const auto result = _threadControl->SetSpeed(
                        static_cast<std::uint32_t>(*threadID),
                        speed);
                    SKSE::log::info(
                        "OSTNET COOP CONTROL SPEED connection={} thread={} speed={} result={}",
                        senderConnectionID,
                        *threadID,
                        speed,
                        static_cast<int>(result));
                } catch (...) {
                }
            }
            return true;
        }

        if (payload.starts_with("CONTROL_STOP|")) {
            if (_sceneControl) {
                const auto result = _sceneControl->StopScene(
                    kPluginName,
                    static_cast<std::uint32_t>(*threadID));
                SKSE::log::info(
                    "OSTNET COOP CONTROL STOP connection={} thread={} result={}",
                    senderConnectionID,
                    *threadID,
                    static_cast<int>(result));
            }
            return true;
        }
        return false;
    }

    void CoopSessionManager::NoteIncomingAuthoritative(
        STRPMApi::ConnectionID ownerConnectionID,
        std::string_view payload)
    {
        const auto threadID = ThreadID(payload);
        if (!threadID) {
            return;
        }
        const auto key = MirrorKey(ownerConnectionID, *threadID);

        std::scoped_lock lock(_mutex);
        if (payload.starts_with("START|")) {
            _pendingMirrorStarts.push_back(PendingMirrorStart{
                ownerConnectionID,
                *threadID,
                Field(payload, "node").value_or(""),
                std::chrono::steady_clock::now() });
            return;
        }

        const auto routeIt = _mirrorByRemote.find(key);
        if (routeIt == _mirrorByRemote.end()) {
            return;
        }
        auto& suppression = _mirrorSuppressions[routeIt->second];
        if (payload.starts_with("NODE|")) {
            suppression.expectedNode = Field(payload, "node");
        } else if (payload.starts_with("SPEED|")) {
            if (const auto speedValue = Field(payload, "speed")) {
                try {
                    suppression.expectedSpeed = static_cast<std::int32_t>(std::stol(*speedValue));
                } catch (...) {
                }
            }
        } else if (payload.starts_with("STOP|")) {
            suppression.stop = true;
        }
    }

    bool CoopSessionManager::HandleIncoming(
        STRPMApi::ConnectionID senderConnectionID,
        std::string_view sender,
        std::string_view payload)
    {
        if (payload.starts_with("INVITE|")) {
            if (const auto sessionID = SessionID(payload)) {
                ShowInviteMessageBox(
                    senderConnectionID,
                    *sessionID,
                    std::string(sender));
            }
            return true;
        }

        if (payload.starts_with("INVITE_RESPONSE|")) {
            const auto sessionID = SessionID(payload);
            const auto accepted = Field(payload, "accepted");
            if (sessionID && accepted) {
                HandleInviteResponse(
                    senderConnectionID,
                    *sessionID,
                    *accepted == "1");
            }
            return true;
        }

        if (payload.starts_with("INVITE_CANCEL|")) {
            RE::DebugNotification("OStim Together: scene invitation canceled");
            return true;
        }

        if (payload.starts_with("CONTROL_NODE|") ||
            payload.starts_with("CONTROL_SPEED|") ||
            payload.starts_with("CONTROL_STOP|")) {
            return HandleControlRequest(senderConnectionID, payload);
        }

        if (payload.starts_with("START|") ||
            payload.starts_with("NODE|") ||
            payload.starts_with("SPEED|") ||
            payload.starts_with("STOP|")) {
            NoteIncomingAuthoritative(senderConnectionID, payload);
        }
        return false;
    }

    void CoopSessionManager::HandleThreadStart(OStim::Thread* thread)
    {
        if (!thread || !thread->isPlayerThread()) {
            return;
        }

        const auto threadID = thread->getThreadID();
        const auto nodeID = GetNodeID(thread);

        {
            std::scoped_lock lock(_mutex);
            if (const auto pendingMirror = TakePendingMirrorStart(thread, nodeID)) {
                _mirrorRoutes[threadID] = MirrorRoute{
                    pendingMirror->ownerConnectionID,
                    pendingMirror->ownerThreadID };
                _mirrorByRemote[MirrorKey(
                    pendingMirror->ownerConnectionID,
                    pendingMirror->ownerThreadID)] = threadID;
                auto& suppression = _mirrorSuppressions[threadID];
                if (!pendingMirror->nodeID.empty()) {
                    suppression.expectedNode = pendingMirror->nodeID;
                }
                SKSE::log::info(
                    "OSTNET COOP MIRROR ROUTE localThread={} ownerConnection={} ownerThread={} node={}",
                    threadID,
                    pendingMirror->ownerConnectionID,
                    pendingMirror->ownerThreadID,
                    nodeID);
                return;
            }

            if (_approvedReplayArmed) {
                const auto sessionID = *_approvedReplayArmed;
                const auto it = _ownerSessions.find(sessionID);
                if (it != _ownerSessions.end() && !it->second.canceled) {
                    it->second.activeThreadID = threadID;
                    it->second.active = true;
                    it->second.restarting = false;
                    _activeOwnerByThread[threadID] = sessionID;
                    _approvedReplayArmed.reset();
                    SKSE::log::info(
                        "OSTNET COOP APPROVED THREAD LIVE session={} thread={} node={}",
                        sessionID,
                        threadID,
                        nodeID);
                    return;
                }
                _approvedReplayArmed.reset();
            }
        }

        auto participants = ResolveRemoteParticipants(thread);
        if (participants.empty()) {
            return;
        }
        BeginOwnerPreflight(thread, std::move(participants), nodeID);
    }

    void CoopSessionManager::HandleThreadNode(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }
        const auto localThreadID = thread->getThreadID();
        const auto nodeID = GetNodeID(thread);
        if (nodeID.empty()) {
            return;
        }

        std::optional<MirrorRoute> route;
        bool suppressed = false;
        {
            std::scoped_lock lock(_mutex);
            const auto routeIt = _mirrorRoutes.find(localThreadID);
            if (routeIt == _mirrorRoutes.end()) {
                return;
            }
            route = routeIt->second;
            auto& suppression = _mirrorSuppressions[localThreadID];
            if (suppression.expectedNode && *suppression.expectedNode == nodeID) {
                suppression.expectedNode.reset();
                suppressed = true;
            }
        }

        if (suppressed || !route) {
            return;
        }
        STRPMTransport::GetSingleton().SendTo(
            route->ownerConnectionID,
            fmt::format(
                "CONTROL_NODE|thread={}|node={}",
                route->ownerThreadID,
                SafeLabel(nodeID)));
        SKSE::log::info(
            "OSTNET COOP CONTROL NODE TX localThread={} ownerThread={} node={}",
            localThreadID,
            route->ownerThreadID,
            nodeID);
    }

    void CoopSessionManager::HandleThreadSpeed(OStim::Thread* thread)
    {
        if (!thread || !_threadControl) {
            return;
        }
        const auto localThreadID = thread->getThreadID();
        {
            std::scoped_lock lock(_mutex);
            if (!_mirrorRoutes.contains(localThreadID)) {
                return;
            }
        }

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([this, localThreadID]() {
                HandleDeferredMirrorSpeed(localThreadID);
            });
        }
    }

    void CoopSessionManager::HandleDeferredMirrorSpeed(std::int32_t localThreadID)
    {
        if (!_threadControl || !_threadControl->IsThreadValid(
                static_cast<std::uint32_t>(localThreadID))) {
            return;
        }
        const auto speed = _threadControl->GetCurrentSpeed(
            static_cast<std::uint32_t>(localThreadID));

        std::optional<MirrorRoute> route;
        bool suppressed = false;
        {
            std::scoped_lock lock(_mutex);
            const auto routeIt = _mirrorRoutes.find(localThreadID);
            if (routeIt == _mirrorRoutes.end()) {
                return;
            }
            route = routeIt->second;
            auto& suppression = _mirrorSuppressions[localThreadID];
            if (suppression.expectedSpeed && *suppression.expectedSpeed == speed) {
                suppression.expectedSpeed.reset();
                suppressed = true;
            }
        }

        if (suppressed || !route) {
            return;
        }
        STRPMTransport::GetSingleton().SendTo(
            route->ownerConnectionID,
            fmt::format(
                "CONTROL_SPEED|thread={}|speed={}",
                route->ownerThreadID,
                speed));
        SKSE::log::info(
            "OSTNET COOP CONTROL SPEED TX DEFERRED localThread={} ownerThread={} speed={}",
            localThreadID,
            route->ownerThreadID,
            speed);
    }

    void CoopSessionManager::HandleThreadStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }
        const auto localThreadID = thread->getThreadID();

        std::optional<MirrorRoute> route;
        bool suppressed = false;
        {
            std::scoped_lock lock(_mutex);
            const auto routeIt = _mirrorRoutes.find(localThreadID);
            if (routeIt != _mirrorRoutes.end()) {
                route = routeIt->second;
                const auto suppressionIt = _mirrorSuppressions.find(localThreadID);
                if (suppressionIt != _mirrorSuppressions.end()) {
                    suppressed = suppressionIt->second.stop;
                    _mirrorSuppressions.erase(suppressionIt);
                }
                _mirrorByRemote.erase(MirrorKey(
                    route->ownerConnectionID,
                    route->ownerThreadID));
                _mirrorRoutes.erase(routeIt);
            }
        }

        if (route && !suppressed) {
            STRPMTransport::GetSingleton().SendTo(
                route->ownerConnectionID,
                fmt::format(
                    "CONTROL_STOP|thread={}",
                    route->ownerThreadID));
            SKSE::log::info(
                "OSTNET COOP CONTROL STOP TX localThread={} ownerThread={}",
                localThreadID,
                route->ownerThreadID);
        }
    }
}
