#include "PCH.h"
#include "CoopSessionManager.h"

#include "Config.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"

#include <cctype>

namespace OStimTogether
{
    namespace
    {
        constexpr const char* kPluginName = "OStimTogether";
        constexpr auto kConsentTimeout = std::chrono::seconds(30);
        constexpr auto kInviteDispatchDelay = std::chrono::milliseconds(150);

        bool IsDynamicSTRProxy(RE::Actor* actor)
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
                    end == std::string_view::npos ?
                        payload.size() - begin : end - begin));
            }

            from = pos + needle.size();
        }

        return std::nullopt;
    }

    std::optional<std::int32_t> CoopSessionManager::ThreadID(
        std::string_view payload)
    {
        const auto value = Field(payload, "thread");
        if (!value || value->empty()) {
            return std::nullopt;
        }

        try {
            return static_cast<std::int32_t>(std::stol(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string CoopSessionManager::MirrorKey(
        STRPMApi::ConnectionID ownerConnectionID,
        std::int32_t remoteThreadID)
    {
        return fmt::format("{}|{}", ownerConnectionID, remoteThreadID);
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

    bool CoopSessionManager::LoadOStimAPIs()
    {
        auto* messaging = SKSE::GetMessagingInterface();
        const auto module = GetModuleHandleW(L"OStim.dll");
        if (!messaging || !module) {
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        const bool exchanged = messaging->Dispatch(
            OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
            &exchange,
            sizeof(exchange),
            nullptr);

        if (!exchanged || !exchange.interfaceMap) {
            return false;
        }

        auto* base = exchange.interfaceMap->queryInterface(
            OStim::ThreadInterface::NAME);
        _threads = base ? static_cast<OStim::ThreadInterface*>(base) : nullptr;

        auto* declaration = SKSE::PluginDeclaration::GetSingleton();
        const auto version = declaration ?
            declaration->GetVersion() : REL::Version{ 0, 23, 1, 0 };
        const auto pluginName = declaration ?
            std::string(declaration->GetName()) : std::string(kPluginName);

        const auto requestThread = reinterpret_cast<
            OStimModAPI::Thread::RequestAPI>(
                reinterpret_cast<void*>(
                    GetProcAddress(module, "RequestPluginAPI_Thread")));
        const auto requestScene = reinterpret_cast<
            OStimModAPI::Scene::RequestAPI>(
                reinterpret_cast<void*>(
                    GetProcAddress(module, "RequestPluginAPI_Scene")));

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
            SKSE::log::error(
                "OSTNET COOP unavailable: OStim thread/scene APIs missing");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerNodeChangedListener(&_nodeListener);
        _threads->registerSpeedChangedListener(&_speedListener);
        _threads->registerThreadStopListener(&_stopListener);

        const auto cfg = Config::Load();
        SKSE::log::info(
            "OSTNET COOP READY consent=nonmodal sharedControls=1 stopAnyParticipant=1 acceptKey={} declineKey={} threadsVersion={}",
            cfg.consentAcceptKey,
            cfg.consentDeclineKey,
            _threads->getVersion());
        return true;
    }

    void CoopSessionManager::Reset()
    {
        std::scoped_lock lock(_mutex);
        _authoritative.clear();
        _pendingInvites.clear();
        _pendingMirrorStarts.clear();
        _mirrorRoutes.clear();
        _mirrorByRemote.clear();
        _mirrorSuppressions.clear();
        _generation.fetch_add(1);
    }

    bool CoopSessionManager::HandleConsentKey(std::uint32_t keyCode)
    {
        static const Config config = Config::Load();
        const bool accept = keyCode == config.consentAcceptKey;
        const bool decline = keyCode == config.consentDeclineKey;
        if (!accept && !decline) {
            return false;
        }

        {
            std::scoped_lock lock(_mutex);
            if (_pendingInvites.empty()) {
                return false;
            }
        }

        AnswerOldestInvite(accept);
        return true;
    }

    std::unordered_set<STRPMApi::ConnectionID>
        CoopSessionManager::ResolveSceneParticipants(
            std::string_view startPayload) const
    {
        std::unordered_set<STRPMApi::ConnectionID> result;
        const auto actors = Field(startPayload, "actors");
        if (!actors || actors->empty()) {
            return result;
        }

        std::size_t begin = 0;
        while (begin <= actors->size()) {
            const auto end = actors->find(',', begin);
            const auto entry = std::string_view(*actors).substr(
                begin,
                end == std::string::npos ?
                    actors->size() - begin : end - begin);

            const auto firstColon = entry.find(':');
            const auto secondColon = firstColon == std::string_view::npos ?
                std::string_view::npos : entry.find(':', firstColon + 1);

            if (firstColon != std::string_view::npos &&
                secondColon != std::string_view::npos) {
                const auto role = entry.substr(
                    firstColon + 1,
                    secondColon - firstColon - 1);

                // On the authoritative client the actual local player is
                // role=player. Every other STR participant is represented by
                // a dynamic proxy whose FormID can be reversed to ConnectionID.
                if (role != "player") {
                    try {
                        const auto formID = static_cast<RE::FormID>(
                            std::stoul(
                                std::string(entry.substr(0, firstColon)),
                                nullptr,
                                16));
                        if (const auto connection =
                                STRPMTransport::GetSingleton().ResolveConnection(formID)) {
                            result.insert(*connection);
                        }
                    } catch (...) {
                    }
                }
            }

            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }

        return result;
    }

    bool CoopSessionManager::BeginConsent(std::string_view startPayload)
    {
        const auto threadID = ThreadID(startPayload);
        if (!threadID) {
            return false;
        }

        auto participants = ResolveSceneParticipants(startPayload);
        if (participants.empty()) {
            return false;
        }

        AuthoritativeSession session{};
        session.threadID = *threadID;
        session.startPayload = std::string(startPayload);
        session.participants = std::move(participants);
        session.generation = _generation.fetch_add(1);

        const auto generation = session.generation;
        const auto participantCount = session.participants.size();
        {
            std::scoped_lock lock(_mutex);
            _authoritative[*threadID] = std::move(session);
        }

        SKSE::log::info(
            "OSTNET COOP START CAPTURED thread={} participants={} dispatchDelayMs={} callbackSafe=1",
            *threadID,
            participantCount,
            kInviteDispatchDelay.count());

        // Important: do not call STRPM send while we are still inside OStim's
        // START callback/fade path. The worker only sleeps; actual session work
        // is queued back onto Skyrim's task interface after the callback has
        // fully unwound.
        std::thread([this, threadID = *threadID, generation]() {
            std::this_thread::sleep_for(kInviteDispatchDelay);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, threadID, generation]() {
                    DispatchInvitesDeferred(threadID, generation);
                });
            }
        }).detach();

        QueueConsentTimeout(*threadID, generation);
        return true;
    }

    void CoopSessionManager::DispatchInvitesDeferred(
        std::int32_t threadID,
        std::uint64_t generation)
    {
        std::unordered_set<STRPMApi::ConnectionID> participants;
        std::string node;
        std::string inviter = "Player";

        {
            std::scoped_lock lock(_mutex);
            const auto it = _authoritative.find(threadID);
            if (it == _authoritative.end() ||
                it->second.canceled ||
                it->second.active ||
                it->second.invitesSent ||
                it->second.generation != generation) {
                return;
            }
            it->second.invitesSent = true;
            participants = it->second.participants;
            node = Field(it->second.startPayload, "node").value_or("");
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            const auto* name = player->GetName();
            if (name && *name) {
                inviter = SafeLabel(name);
            }
        }

        for (const auto connectionID : participants) {
            STRPMTransport::GetSingleton().SendTo(
                connectionID,
                fmt::format(
                    "INVITE|thread={}|inviter={}|node={}",
                    threadID,
                    inviter,
                    SafeLabel(node)));
        }

        SKSE::log::info(
            "OSTNET COOP INVITE TX DEFERRED thread={} participants={} node={} status=pending",
            threadID,
            participants.size(),
            node);
        RE::DebugNotification(
            "OStim Together: waiting for remote consent");
    }

    void CoopSessionManager::QueueConsentTimeout(
        std::int32_t threadID,
        std::uint64_t generation)
    {
        std::thread([this, threadID, generation]() {
            std::this_thread::sleep_for(kConsentTimeout);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, threadID, generation]() {
                    bool timedOut = false;
                    {
                        std::scoped_lock lock(_mutex);
                        const auto it = _authoritative.find(threadID);
                        timedOut =
                            it != _authoritative.end() &&
                            !it->second.active &&
                            !it->second.canceled &&
                            it->second.generation == generation;
                    }
                    if (timedOut) {
                        CancelAuthoritativeSession(
                            threadID,
                            "consent-timeout",
                            true);
                        RE::DebugNotification(
                            "OStim Together: consent request timed out");
                    }
                });
            }
        }).detach();
    }

    void CoopSessionManager::RegisterIncomingInvite(
        STRPMApi::ConnectionID ownerConnectionID,
        std::int32_t remoteThreadID,
        std::string sender)
    {
        const auto senderLabel = SafeLabel(sender);
        bool inserted = false;
        {
            std::scoped_lock lock(_mutex);
            const auto duplicate = std::find_if(
                _pendingInvites.begin(),
                _pendingInvites.end(),
                [ownerConnectionID, remoteThreadID](const PendingInvite& invite) {
                    return invite.ownerConnectionID == ownerConnectionID &&
                           invite.remoteThreadID == remoteThreadID;
                });
            if (duplicate == _pendingInvites.end()) {
                _pendingInvites.push_back(PendingInvite{
                    ownerConnectionID,
                    remoteThreadID,
                    senderLabel,
                    std::chrono::steady_clock::now() });
                inserted = true;
            }
        }

        if (!inserted) {
            return;
        }

        const auto cfg = Config::Load();
        RE::DebugNotification(fmt::format(
            "OStim Together: {} invites you - Y accept / N decline",
            senderLabel).c_str());

        SKSE::log::info(
            "OSTNET COOP INVITE NONMODAL ownerConnection={} thread={} sender=\"{}\" acceptKey={} declineKey={}",
            ownerConnectionID,
            remoteThreadID,
            senderLabel,
            cfg.consentAcceptKey,
            cfg.consentDeclineKey);

        QueueIncomingInviteTimeout(ownerConnectionID, remoteThreadID);
    }

    void CoopSessionManager::QueueIncomingInviteTimeout(
        STRPMApi::ConnectionID ownerConnectionID,
        std::int32_t remoteThreadID)
    {
        std::thread([this, ownerConnectionID, remoteThreadID]() {
            std::this_thread::sleep_for(kConsentTimeout);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([this, ownerConnectionID, remoteThreadID]() {
                    bool expired = false;
                    {
                        std::scoped_lock lock(_mutex);
                        const auto it = std::find_if(
                            _pendingInvites.begin(),
                            _pendingInvites.end(),
                            [ownerConnectionID, remoteThreadID](const PendingInvite& invite) {
                                return invite.ownerConnectionID == ownerConnectionID &&
                                       invite.remoteThreadID == remoteThreadID;
                            });
                        if (it != _pendingInvites.end()) {
                            _pendingInvites.erase(it);
                            expired = true;
                        }
                    }
                    if (expired) {
                        STRPMTransport::GetSingleton().SendTo(
                            ownerConnectionID,
                            fmt::format(
                                "INVITE_RESPONSE|thread={}|accepted=0",
                                remoteThreadID));
                        RE::DebugNotification(
                            "OStim Together: scene invitation expired");
                        SKSE::log::info(
                            "OSTNET COOP INVITE EXPIRED ownerConnection={} thread={}",
                            ownerConnectionID,
                            remoteThreadID);
                    }
                });
            }
        }).detach();
    }

    void CoopSessionManager::AnswerOldestInvite(bool accepted)
    {
        std::optional<PendingInvite> invite;
        {
            std::scoped_lock lock(_mutex);
            if (_pendingInvites.empty()) {
                return;
            }
            invite = _pendingInvites.front();
            _pendingInvites.erase(_pendingInvites.begin());
        }

        if (invite) {
            AnswerConsent(
                invite->ownerConnectionID,
                invite->remoteThreadID,
                accepted);
        }
    }

    void CoopSessionManager::AnswerConsent(
        STRPMApi::ConnectionID ownerConnectionID,
        std::int32_t remoteThreadID,
        bool accepted)
    {
        STRPMTransport::GetSingleton().SendTo(
            ownerConnectionID,
            fmt::format(
                "INVITE_RESPONSE|thread={}|accepted={}",
                remoteThreadID,
                accepted ? 1 : 0));

        RE::DebugNotification(
            accepted ?
                "OStim Together: scene accepted" :
                "OStim Together: scene declined");

        SKSE::log::info(
            "OSTNET COOP INVITE RESPONSE TX ownerConnection={} thread={} accepted={} source=keyboard",
            ownerConnectionID,
            remoteThreadID,
            accepted ? 1 : 0);
    }

    void CoopSessionManager::HandleConsentResponse(
        STRPMApi::ConnectionID participantConnectionID,
        std::int32_t threadID,
        bool accepted)
    {
        bool activate = false;
        bool reject = false;

        {
            std::scoped_lock lock(_mutex);
            const auto it = _authoritative.find(threadID);
            if (it == _authoritative.end() ||
                it->second.active ||
                it->second.canceled ||
                !it->second.participants.contains(participantConnectionID)) {
                return;
            }

            if (!accepted) {
                it->second.canceled = true;
                reject = true;
            } else {
                it->second.accepted.insert(participantConnectionID);
                activate =
                    it->second.accepted.size() ==
                    it->second.participants.size();
            }
        }

        SKSE::log::info(
            "OSTNET COOP INVITE RESPONSE RX thread={} connection={} accepted={} activate={}",
            threadID,
            participantConnectionID,
            accepted ? 1 : 0,
            activate ? 1 : 0);

        if (reject) {
            CancelAuthoritativeSession(threadID, "declined", true);
            RE::DebugNotification(
                "OStim Together: remote player declined the scene");
        } else if (activate) {
            ActivateAuthoritativeSession(threadID);
        }
    }

    void CoopSessionManager::ActivateAuthoritativeSession(
        std::int32_t threadID)
    {
        std::unordered_set<STRPMApi::ConnectionID> participants;
        std::string startPayload;
        std::string nodePayload;
        std::string speedPayload;

        {
            std::scoped_lock lock(_mutex);
            const auto it = _authoritative.find(threadID);
            if (it == _authoritative.end() ||
                it->second.canceled ||
                it->second.active) {
                return;
            }
            it->second.active = true;
            participants = it->second.participants;
            startPayload = it->second.startPayload;
            nodePayload = it->second.latestNodePayload;
            speedPayload = it->second.latestSpeedPayload;
        }

        for (const auto connectionID : participants) {
            STRPMTransport::GetSingleton().SendTo(connectionID, startPayload);
            if (!nodePayload.empty()) {
                STRPMTransport::GetSingleton().SendTo(connectionID, nodePayload);
            }
            if (!speedPayload.empty()) {
                STRPMTransport::GetSingleton().SendTo(connectionID, speedPayload);
            }
        }

        SKSE::log::info(
            "OSTNET COOP ACTIVE thread={} participants={} cachedNode={} cachedSpeed={}",
            threadID,
            participants.size(),
            nodePayload.empty() ? 0 : 1,
            speedPayload.empty() ? 0 : 1);
        RE::DebugNotification(
            "OStim Together: remote player accepted");
    }

    void CoopSessionManager::CancelAuthoritativeSession(
        std::int32_t threadID,
        std::string_view reason,
        bool stopLocalThread)
    {
        std::unordered_set<STRPMApi::ConnectionID> participants;
        {
            std::scoped_lock lock(_mutex);
            const auto it = _authoritative.find(threadID);
            if (it == _authoritative.end()) {
                return;
            }
            it->second.canceled = true;
            participants = it->second.participants;
        }

        for (const auto connectionID : participants) {
            STRPMTransport::GetSingleton().SendTo(
                connectionID,
                fmt::format(
                    "INVITE_CANCEL|thread={}|reason={}",
                    threadID,
                    SafeLabel(reason)));
        }

        SKSE::log::info(
            "OSTNET COOP CANCEL thread={} reason={} participants={} stopLocal={}",
            threadID,
            reason,
            participants.size(),
            stopLocalThread ? 1 : 0);

        if (stopLocalThread && _sceneControl) {
            const auto result = _sceneControl->StopScene(
                kPluginName,
                static_cast<std::uint32_t>(threadID));
            SKSE::log::info(
                "OSTNET COOP CANCEL STOP thread={} result={}",
                threadID,
                static_cast<int>(result));
        }
    }

    bool CoopSessionManager::RouteAuthoritativePayload(
        std::string_view payload)
    {
        const auto threadID = ThreadID(payload);
        if (!threadID) {
            return false;
        }

        std::unordered_set<STRPMApi::ConnectionID> participants;
        bool active = false;
        bool canceled = false;
        const bool isStop = payload.starts_with("STOP|");
        const bool isNode = payload.starts_with("NODE|");
        const bool isSpeed = payload.starts_with("SPEED|");

        {
            std::scoped_lock lock(_mutex);
            auto it = _authoritative.find(*threadID);
            if (it == _authoritative.end()) {
                return false;
            }

            active = it->second.active;
            canceled = it->second.canceled;
            participants = it->second.participants;

            if (!active && !canceled) {
                if (isNode) {
                    it->second.latestNodePayload = std::string(payload);
                } else if (isSpeed) {
                    it->second.latestSpeedPayload = std::string(payload);
                }
            }

            if (isStop) {
                _authoritative.erase(it);
            }
        }

        if (canceled) {
            return true;
        }

        if (!active) {
            if (isStop) {
                for (const auto connectionID : participants) {
                    STRPMTransport::GetSingleton().SendTo(
                        connectionID,
                        fmt::format(
                            "INVITE_CANCEL|thread={}|reason=local-stop",
                            *threadID));
                }
            }
            return true;
        }

        for (const auto connectionID : participants) {
            STRPMTransport::GetSingleton().SendTo(connectionID, payload);
        }
        return true;
    }

    bool CoopSessionManager::InterceptOutgoing(std::string_view payload)
    {
        if (payload.starts_with("START|")) {
            return BeginConsent(payload);
        }
        if (payload.starts_with("NODE|") ||
            payload.starts_with("SPEED|") ||
            payload.starts_with("STOP|")) {
            return RouteAuthoritativePayload(payload);
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

        bool authorized = false;
        {
            std::scoped_lock lock(_mutex);
            const auto it = _authoritative.find(*threadID);
            authorized =
                it != _authoritative.end() &&
                it->second.active &&
                !it->second.canceled &&
                it->second.participants.contains(senderConnectionID);
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
            if (node && !node->empty() && _threadControl) {
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
            const auto value = Field(payload, "speed");
            if (value && _threadControl) {
                try {
                    const auto speed = static_cast<std::int32_t>(std::stol(*value));
                    const auto result = _threadControl->SetSpeed(
                        static_cast<std::uint32_t>(*threadID), speed);
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
            const auto speed = Field(payload, "speed");
            if (speed) {
                try {
                    suppression.expectedSpeed =
                        static_cast<std::int32_t>(std::stol(*speed));
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
            if (const auto threadID = ThreadID(payload)) {
                RegisterIncomingInvite(
                    senderConnectionID,
                    *threadID,
                    std::string(sender));
            }
            return true;
        }

        if (payload.starts_with("INVITE_RESPONSE|")) {
            const auto threadID = ThreadID(payload);
            const auto accepted = Field(payload, "accepted");
            if (threadID && accepted) {
                HandleConsentResponse(
                    senderConnectionID,
                    *threadID,
                    *accepted == "1");
            }
            return true;
        }

        if (payload.starts_with("INVITE_CANCEL|")) {
            if (const auto threadID = ThreadID(payload)) {
                std::scoped_lock lock(_mutex);
                _pendingInvites.erase(
                    std::remove_if(
                        _pendingInvites.begin(),
                        _pendingInvites.end(),
                        [senderConnectionID, threadID](const PendingInvite& invite) {
                            return invite.ownerConnectionID == senderConnectionID &&
                                   invite.remoteThreadID == *threadID;
                        }),
                    _pendingInvites.end());
            }
            RE::DebugNotification(
                "OStim Together: scene invitation canceled");
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

    bool CoopSessionManager::LooksLikeRemoteMirror(OStim::Thread* thread) const
    {
        if (!thread || !thread->isPlayerThread()) {
            return false;
        }

        bool hasSelf = false;
        bool hasProxy = false;
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ?
                static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (!actor) {
                continue;
            }
            hasSelf = hasSelf || actor->IsPlayerRef();
            hasProxy = hasProxy || IsDynamicSTRProxy(actor);
        }
        return hasSelf && hasProxy;
    }

    void CoopSessionManager::HandleThreadStart(OStim::Thread* thread)
    {
        if (!LooksLikeRemoteMirror(thread)) {
            return;
        }

        const auto localThreadID = thread->getThreadID();
        auto* node = thread->getCurrentNode();
        const std::string nodeID =
            node && node->getNodeID() ? node->getNodeID() : "";

        std::optional<PendingMirrorStart> chosen;
        {
            std::scoped_lock lock(_mutex);
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
                [&nodeID](const PendingMirrorStart& entry) {
                    return entry.nodeID.empty() || nodeID.empty() ||
                           entry.nodeID == nodeID;
                });
            if (it == _pendingMirrorStarts.end() && !_pendingMirrorStarts.empty()) {
                it = _pendingMirrorStarts.begin();
            }
            if (it != _pendingMirrorStarts.end()) {
                chosen = *it;
                _pendingMirrorStarts.erase(it);
            }

            if (chosen) {
                _mirrorRoutes[localThreadID] = MirrorRoute{
                    chosen->ownerConnectionID,
                    chosen->remoteThreadID };
                _mirrorByRemote[MirrorKey(
                    chosen->ownerConnectionID,
                    chosen->remoteThreadID)] = localThreadID;

                auto& suppression = _mirrorSuppressions[localThreadID];
                if (!chosen->nodeID.empty()) {
                    suppression.expectedNode = chosen->nodeID;
                }
                // Do not call GetCurrentSpeed() from OStim's START callback.
                // OStim may still hold its internal thread lock here. The
                // authoritative START/SPEED packets establish suppression
                // state after the callback has unwound.
            }
        }

        if (chosen) {
            SKSE::log::info(
                "OSTNET COOP MIRROR ROUTE localThread={} ownerConnection={} ownerThread={} node={}",
                localThreadID,
                chosen->ownerConnectionID,
                chosen->remoteThreadID,
                nodeID);
        }
    }

    void CoopSessionManager::HandleThreadNode(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto localThreadID = thread->getThreadID();
        auto* node = thread->getCurrentNode();
        const std::string nodeID =
            node && node->getNodeID() ? node->getNodeID() : "";
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
            if (suppression.expectedNode &&
                *suppression.expectedNode == nodeID) {
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
                route->remoteThreadID,
                SafeLabel(nodeID)));
        SKSE::log::info(
            "OSTNET COOP CONTROL NODE TX localThread={} ownerConnection={} ownerThread={} node={}",
            localThreadID,
            route->ownerConnectionID,
            route->remoteThreadID,
            nodeID);
    }

    void CoopSessionManager::HandleThreadSpeed(OStim::Thread* thread)
    {
        if (!thread || !_threadControl) {
            return;
        }

        const auto localThreadID = thread->getThreadID();

        // First determine whether this is actually a cooperative mirror using
        // only our own state. Never re-enter OStim ModAPI for ordinary local,
        // NPC, or auxiliary threads.
        {
            std::scoped_lock lock(_mutex);
            if (!_mirrorRoutes.contains(localThreadID)) {
                return;
            }
        }

        // OStim invokes speed listeners while it may still own its internal
        // thread lock. GetCurrentSpeed() re-enters that lock and can deadlock
        // the game (the same rule already enforced by OStimBridge::HandleSpeed).
        // Defer all ModAPI access until the listener has fully returned.
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            SKSE::log::warn(
                "OSTNET COOP SPEED defer unavailable localThread={}",
                localThreadID);
            return;
        }

        tasks->AddTask([this, localThreadID]() {
            if (!_threadControl) {
                return;
            }

            std::optional<MirrorRoute> route;
            {
                std::scoped_lock lock(_mutex);
                const auto routeIt = _mirrorRoutes.find(localThreadID);
                if (routeIt == _mirrorRoutes.end()) {
                    return;
                }
                route = routeIt->second;
            }

            const auto speed = _threadControl->GetCurrentSpeed(
                static_cast<std::uint32_t>(localThreadID));

            bool suppressed = false;
            {
                std::scoped_lock lock(_mutex);
                const auto routeIt = _mirrorRoutes.find(localThreadID);
                if (routeIt == _mirrorRoutes.end()) {
                    return;
                }
                route = routeIt->second;
                auto& suppression = _mirrorSuppressions[localThreadID];
                if (suppression.expectedSpeed &&
                    *suppression.expectedSpeed == speed) {
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
                    route->remoteThreadID,
                    speed));
            SKSE::log::info(
                "OSTNET COOP CONTROL SPEED TX DEFERRED localThread={} ownerConnection={} ownerThread={} speed={}",
                localThreadID,
                route->ownerConnectionID,
                route->remoteThreadID,
                speed);
        });
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
            if (routeIt == _mirrorRoutes.end()) {
                return;
            }

            route = routeIt->second;
            const auto suppressionIt = _mirrorSuppressions.find(localThreadID);
            if (suppressionIt != _mirrorSuppressions.end()) {
                suppressed = suppressionIt->second.stop;
                _mirrorSuppressions.erase(suppressionIt);
            }

            _mirrorByRemote.erase(MirrorKey(
                route->ownerConnectionID,
                route->remoteThreadID));
            _mirrorRoutes.erase(routeIt);
        }

        if (!suppressed && route) {
            STRPMTransport::GetSingleton().SendTo(
                route->ownerConnectionID,
                fmt::format(
                    "CONTROL_STOP|thread={}",
                    route->remoteThreadID));
            SKSE::log::info(
                "OSTNET COOP CONTROL STOP TX localThread={} ownerConnection={} ownerThread={}",
                localThreadID,
                route->ownerConnectionID,
                route->remoteThreadID);
        }
    }
}
