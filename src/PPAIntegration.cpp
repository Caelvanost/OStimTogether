#include "PCH.h"
#include "PPAIntegration.h"

#include "OStimAPI/InterfaceExchangeMessage.h"
#include "STRPMTransport.h"

#include <cstring>

namespace OStimTogether
{
    namespace
    {
        constexpr wchar_t kSTRPMModuleName[] =
            L"STRPluginMessagingAPI.dll";
        constexpr char kPPAChannel[] =
            "ostimtogether.ppa";
        constexpr char kMarkerPath[] =
            "Data/SKSE/Plugins/OStimTogether_PPA.ini";
        constexpr char kPluginName[] =
            "OStimTogether";

        // Exact AccuratePenetration.dll supplied for the 0.33.0 integration.
        // SHA-256:
        // 0BD68B935E54211EEA71BE064AEB628B2AA268A7F35229FF554ED65A88EED087
        constexpr std::uint32_t kSupportedPETimestamp = 0x6A633AE8;
        constexpr std::uint32_t kSupportedImageSize = 0x00239000;
        constexpr std::uintptr_t kGetAnimationTaggerRVA = 0x0004B9B0;
        constexpr std::uintptr_t kSetInteractionRVA = 0x00057D80;
        constexpr std::uintptr_t kSetTargetRVA = 0x00058070;

        constexpr std::array<std::uint8_t, 18> kGetAnimationTaggerSignature{
            0x48, 0x83, 0xEC, 0x28, 0x8B, 0x0D, 0x8E, 0xAA, 0x1D,
            0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00
        };

        constexpr std::array<std::uint8_t, 18> kSetInteractionSignature{
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
            0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE1, 0x48, 0x81
        };

        constexpr std::array<std::uint8_t, 18> kSetTargetSignature{
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE9
        };

        std::optional<std::string> Field(
            std::string_view payload,
            std::string_view key)
        {
            const auto needle = fmt::format("{}=", key);
            std::size_t searchFrom = 0;

            while (searchFrom < payload.size()) {
                const auto pos = payload.find(needle, searchFrom);
                if (pos == std::string_view::npos) {
                    return std::nullopt;
                }

                if (pos == 0 || payload[pos - 1] == '|') {
                    const auto begin = pos + needle.size();
                    const auto end = payload.find('|', begin);
                    return std::string(
                        payload.substr(
                            begin,
                            end == std::string_view::npos ?
                                payload.size() - begin :
                                end - begin));
                }

                searchFrom = pos + needle.size();
            }

            return std::nullopt;
        }

        std::optional<std::uint32_t> ParseUInt(
            std::string_view payload,
            std::string_view key)
        {
            const auto value = Field(payload, key);
            if (!value || value->empty()) {
                return std::nullopt;
            }

            try {
                return static_cast<std::uint32_t>(
                    std::stoul(*value));
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<std::int32_t> ParseInt(
            std::string_view payload,
            std::string_view key)
        {
            const auto value = Field(payload, key);
            if (!value || value->empty()) {
                return std::nullopt;
            }

            try {
                return static_cast<std::int32_t>(
                    std::stol(*value));
            } catch (...) {
                return std::nullopt;
            }
        }

        template <std::size_t N>
        bool MatchSignature(
            std::uintptr_t address,
            const std::array<std::uint8_t, N>& signature)
        {
            return address != 0 &&
                   std::memcmp(
                       reinterpret_cast<const void*>(address),
                       signature.data(),
                       signature.size()) == 0;
        }
    }

    PPAIntegration& PPAIntegration::GetSingleton()
    {
        static PPAIntegration singleton;
        return singleton;
    }

    PPAIntegration::~PPAIntegration()
    {
        Disconnect();
    }

    const char* PPAIntegration::TargetName(
        std::uint8_t target) noexcept
    {
        switch (static_cast<ActionTarget>(target)) {
        case ActionTarget::Auto:
            return "Auto";
        case ActionTarget::None:
            return "None";
        case ActionTarget::Vagina:
            return "Vagina";
        case ActionTarget::Anus:
            return "Anus";
        case ActionTarget::Mouth:
            return "Mouth";
        case ActionTarget::Hand:
            return "Hand";
        case ActionTarget::LeftHand:
            return "L Hand";
        case ActionTarget::RightHand:
            return "R Hand";
        default:
            return "Unknown";
        }
    }

    std::string PPAIntegration::HexEncode(std::string_view value)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(value.size() * 2);
        for (const auto ch : value) {
            const auto u = static_cast<unsigned char>(ch);
            out.push_back(kHex[(u >> 4) & 0x0F]);
            out.push_back(kHex[u & 0x0F]);
        }
        return out;
    }

    std::optional<std::string> PPAIntegration::HexDecode(
        std::string_view value)
    {
        if ((value.size() % 2) != 0) {
            return std::nullopt;
        }

        const auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f') {
                return 10 + ch - 'a';
            }
            if (ch >= 'A' && ch <= 'F') {
                return 10 + ch - 'A';
            }
            return -1;
        };

        std::string out;
        out.reserve(value.size() / 2);
        for (std::size_t i = 0; i < value.size(); i += 2) {
            const auto hi = nibble(value[i]);
            const auto lo = nibble(value[i + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
        return out;
    }

    bool PPAIntegration::IsOptionalIntegrationInstalled() const
    {
        std::error_code ec;
        return std::filesystem::exists(kMarkerPath, ec) && !ec;
    }

    bool PPAIntegration::ConnectOStim()
    {
        auto* messaging = SKSE::GetMessagingInterface();
        const auto module = GetModuleHandleW(L"OStim.dll");
        if (!messaging || !module) {
            SKSE::log::warn(
                "OSTNET PPA OStim bridge unavailable: OStim.dll or messaging missing");
            return false;
        }

        OStim::InterfaceExchangeMessage exchange{};
        if (!messaging->Dispatch(
                OStim::InterfaceExchangeMessage::MESSAGE_TYPE,
                &exchange,
                sizeof(exchange),
                nullptr) ||
            !exchange.interfaceMap) {
            SKSE::log::warn(
                "OSTNET PPA OStim bridge unavailable: interface exchange failed");
            return false;
        }

        auto* base =
            exchange.interfaceMap->queryInterface(
                OStim::ThreadInterface::NAME);
        _threads = base ?
            static_cast<OStim::ThreadInterface*>(base) :
            nullptr;

        const auto requestThread =
            reinterpret_cast<OStimModAPI::Thread::RequestAPI>(
                reinterpret_cast<void*>(
                    GetProcAddress(
                        module,
                        "RequestPluginAPI_Thread")));

        auto* declaration =
            SKSE::PluginDeclaration::GetSingleton();
        const auto version = declaration ?
            declaration->GetVersion() :
            REL::Version{ 0, 33, 0, 0 };
        const auto pluginName = declaration ?
            std::string(declaration->GetName()) :
            std::string(kPluginName);

        _threadControl = requestThread ?
            requestThread(
                OStimModAPI::Thread::InterfaceVersion::V1,
                pluginName.c_str(),
                version) :
            nullptr;

        if (!_threads || !_threadControl) {
            SKSE::log::warn(
                "OSTNET PPA OStim bridge unavailable: thread APIs missing");
            return false;
        }

        SKSE::log::info(
            "OSTNET PPA OSTIM READY threadsVersion={}",
            _threads->getVersion());
        return true;
    }

    bool PPAIntegration::ValidateExactPPABuild(HMODULE module) const
    {
        if (!module) {
            return false;
        }

        const auto base =
            reinterpret_cast<std::uintptr_t>(module);
        const auto* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + static_cast<std::uintptr_t>(dos->e_lfanew));

        if (!nt ||
            nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return false;
        }

        if (nt->FileHeader.TimeDateStamp != kSupportedPETimestamp ||
            nt->OptionalHeader.SizeOfImage != kSupportedImageSize) {
            SKSE::log::warn(
                "OSTNET PPA INTERNAL unsupported build timestamp=0x{:08X} imageSize=0x{:X} expectedTimestamp=0x{:08X} expectedImageSize=0x{:X}",
                nt->FileHeader.TimeDateStamp,
                nt->OptionalHeader.SizeOfImage,
                kSupportedPETimestamp,
                kSupportedImageSize);
            return false;
        }

        const bool getterOK =
            MatchSignature(
                base + kGetAnimationTaggerRVA,
                kGetAnimationTaggerSignature);
        const bool interactionOK =
            MatchSignature(
                base + kSetInteractionRVA,
                kSetInteractionSignature);
        const bool targetOK =
            MatchSignature(
                base + kSetTargetRVA,
                kSetTargetSignature);

        if (!getterOK || !interactionOK || !targetOK) {
            SKSE::log::warn(
                "OSTNET PPA INTERNAL unsupported build signature getter={} setInteraction={} setTarget={} action=disable-no-hook",
                getterOK ? 1 : 0,
                interactionOK ? 1 : 0,
                targetOK ? 1 : 0);
            return false;
        }

        return true;
    }

    bool PPAIntegration::InstallPPAHooks(HMODULE module)
    {
        if (_hooksInstalled) {
            return true;
        }

        if (!ValidateExactPPABuild(module)) {
            return false;
        }

        const auto base =
            reinterpret_cast<std::uintptr_t>(module);

        _getAnimationTagger =
            reinterpret_cast<GetAnimationTaggerFn>(
                base + kGetAnimationTaggerRVA);

        // Each target function begins with exactly five bytes of whole
        // instructions before the next instruction boundary. CommonLib's
        // five-byte trampoline therefore preserves the displaced prologue and
        // gives us a callable original function for remote application.
        SKSE::AllocTrampoline(64);
        auto& trampoline = SKSE::GetTrampoline();

        const auto originalInteraction =
            trampoline.write_branch<5>(
                base + kSetInteractionRVA,
                &PPAIntegration::SetInteractionHook);
        const auto originalTarget =
            trampoline.write_branch<5>(
                base + kSetTargetRVA,
                &PPAIntegration::SetTargetHook);

        _setInteractionOriginal =
            reinterpret_cast<SetInteractionFn>(
                originalInteraction);
        _setTargetOriginal =
            reinterpret_cast<SetTargetFn>(
                originalTarget);

        if (!_getAnimationTagger ||
            !_setInteractionOriginal ||
            !_setTargetOriginal) {
            SKSE::log::error(
                "OSTNET PPA INTERNAL hook installation failed getter={} interactionOriginal={} targetOriginal={}",
                _getAnimationTagger ? 1 : 0,
                _setInteractionOriginal ? 1 : 0,
                _setTargetOriginal ? 1 : 0);
            return false;
        }

        _hooksInstalled = true;

        SKSE::log::info(
            "OSTNET PPA INTERNAL READY timestamp=0x{:08X} imageSize=0x{:X} getterRva=0x{:X} setInteractionRva=0x{:X} setTargetRva=0x{:X} exactBuild=1 targetRead=1 targetWrite=1",
            kSupportedPETimestamp,
            kSupportedImageSize,
            kGetAnimationTaggerRVA,
            kSetInteractionRVA,
            kSetTargetRVA);
        return true;
    }

    bool PPAIntegration::ConnectPPA()
    {
        const auto module =
            GetModuleHandleW(
                AccuratePenetration::API::kPluginDLL);

        if (!module) {
            SKSE::log::warn(
                "OSTNET PPA unavailable: AccuratePenetration.dll is not loaded");
            return false;
        }

        using GetAPIFn =
            const AccuratePenetration::API::InterfaceV1*(__cdecl*)();

        const auto getAPI =
            reinterpret_cast<GetAPIFn>(
                GetProcAddress(
                    module,
                    AccuratePenetration::API::kGetAPIFunctionNameV1));
        const auto* api = getAPI ? getAPI() : nullptr;

        // The public API remains our stable identity/ABI check even though PPA
        // V1 does not expose target setters. Runtime writes below are enabled
        // only for the exact binary whose internal functions were verified.
        if (!api ||
            api->version != AccuratePenetration::API::kVersion ||
            api->size < sizeof(AccuratePenetration::API::InterfaceV1)) {
            SKSE::log::warn(
                "OSTNET PPA API unavailable or incompatible expectedVersion={}",
                AccuratePenetration::API::kVersion);
            return false;
        }

        if (!InstallPPAHooks(module)) {
            return false;
        }

        _ppaModule = module;
        _ppaAPI = api;
        return true;
    }

    bool PPAIntegration::ConnectTransport()
    {
        const auto module =
            GetModuleHandleW(kSTRPMModuleName);
        if (!module) {
            SKSE::log::warn(
                "OSTNET PPA transport unavailable: STRPluginMessagingAPI.dll is not loaded");
            return false;
        }

        const auto query =
            reinterpret_cast<STRPMApi::QueryInterfaceFn>(
                GetProcAddress(
                    module,
                    STRPMApi::kQueryInterfaceExportName));

        const STRPMApi::Interface* api = nullptr;
        const auto queryResult = query ?
            query(STRPMApi::kInterfaceVersion, &api) :
            STRPMApi::Result::kNotAvailable;

        if (queryResult != STRPMApi::Result::kOk ||
            !api ||
            !api->registerChannel ||
            !api->unregisterChannel ||
            !api->send) {
            SKSE::log::warn(
                "OSTNET PPA transport API unavailable result={}",
                static_cast<std::uint32_t>(queryResult));
            return false;
        }

        STRPMApi::ListenerHandle listener{};
        const auto result =
            api->registerChannel(
                kPPAChannel,
                &PPAIntegration::OnTransportMessage,
                this,
                &listener);

        if (result != STRPMApi::Result::kOk) {
            SKSE::log::warn(
                "OSTNET PPA transport channel registration failed result={}",
                static_cast<std::uint32_t>(result));
            return false;
        }

        _transportAPI = api;
        _transportListener = listener;

        SKSE::log::info(
            "OSTNET PPA TRANSPORT READY channel={} apiVersion={} routing=active-ostim-participants-only",
            kPPAChannel,
            api->version);
        return true;
    }

    bool PPAIntegration::Initialize()
    {
        if (_enabled.load()) {
            return true;
        }

        if (!IsOptionalIntegrationInstalled()) {
            SKSE::log::info(
                "OSTNET PPA integration disabled: optional FOMOD component not installed");
            return false;
        }

        if (!ConnectOStim() ||
            !ConnectPPA() ||
            !ConnectTransport()) {
            Disconnect();
            return false;
        }

        _enabled.store(true);

        SKSE::log::info(
            "OSTNET PPA integration READY authority=real-local-player performerSync=1 targetSiteSync=1 explicitTargetActorSync=1 remoteApply=internal-setter exactBuildFailClosed=1");
        return true;
    }

    void PPAIntegration::Disconnect()
    {
        _enabled.store(false);

        if (_transportAPI &&
            _transportListener.value != 0 &&
            _transportAPI->unregisterChannel) {
            _transportAPI->unregisterChannel(_transportListener);
        }

        _transportListener = {};
        _transportAPI = nullptr;
        _ppaAPI = nullptr;
        _ppaModule = nullptr;
        _threads = nullptr;
        _threadControl = nullptr;
    }

    void PPAIntegration::Reset()
    {
        // Hooks stay installed for the lifetime of the process, exactly like
        // other SKSE detours. Save changes require no persistent network state.
    }

    bool PPAIntegration::IsLocalPlayerPerformer(
        std::uint8_t performerPosition) const
    {
        if (!_enabled.load() ||
            !_threads ||
            !_threadControl ||
            performerPosition == 0) {
            return false;
        }

        const auto tid =
            _threadControl->GetPlayerThreadID();
        if (!_threadControl->IsThreadValid(tid)) {
            return false;
        }

        auto* thread =
            _threads->getThread(
                static_cast<std::int32_t>(tid));
        if (!thread ||
            performerPosition > thread->getActorCount()) {
            return false;
        }

        auto* threadActor =
            thread->getActor(
                static_cast<std::uint32_t>(
                    performerPosition - 1));
        auto* actor = threadActor ?
            static_cast<RE::Actor*>(
                threadActor->getGameActor()) :
            nullptr;

        return actor &&
               actor == RE::PlayerCharacter::GetSingleton();
    }

    std::vector<STRPMApi::ConnectionID>
        PPAIntegration::SnapshotRemoteParticipants() const
    {
        std::vector<STRPMApi::ConnectionID> result;

        if (!_threads || !_threadControl) {
            return result;
        }

        const auto tid =
            _threadControl->GetPlayerThreadID();
        if (!_threadControl->IsThreadValid(tid)) {
            return result;
        }

        auto* thread =
            _threads->getThread(
                static_cast<std::int32_t>(tid));
        if (!thread) {
            return result;
        }

        std::unordered_set<STRPMApi::ConnectionID> unique;
        for (std::uint32_t i = 0;
             i < thread->getActorCount();
             ++i) {
            auto* threadActor = thread->getActor(i);
            auto* actor = threadActor ?
                static_cast<RE::Actor*>(
                    threadActor->getGameActor()) :
                nullptr;

            if (!actor || actor->IsPlayerRef()) {
                continue;
            }

            const auto connection =
                STRPMTransport::GetSingleton()
                    .ResolveConnection(
                        actor->GetFormID());
            if (connection && *connection != 0 &&
                unique.insert(*connection).second) {
                result.push_back(*connection);
            }
        }

        return result;
    }

    bool PPAIntegration::ValidateRemoteSender(
        STRPMApi::ConnectionID senderConnectionID,
        std::uint8_t performerPosition) const
    {
        if (!_threads || !_threadControl ||
            senderConnectionID == 0 ||
            performerPosition == 0) {
            return false;
        }

        const auto proxyFormID =
            STRPMTransport::GetSingleton()
                .ResolveProxy(senderConnectionID);
        if (!proxyFormID) {
            return false;
        }

        const auto tid =
            _threadControl->GetPlayerThreadID();
        if (!_threadControl->IsThreadValid(tid)) {
            return false;
        }

        auto* thread =
            _threads->getThread(
                static_cast<std::int32_t>(tid));
        if (!thread ||
            performerPosition > thread->getActorCount()) {
            return false;
        }

        auto* threadActor =
            thread->getActor(
                static_cast<std::uint32_t>(
                    performerPosition - 1));
        auto* actor = threadActor ?
            static_cast<RE::Actor*>(
                threadActor->getGameActor()) :
            nullptr;

        return actor &&
               actor->GetFormID() == *proxyFormID;
    }

    bool PPAIntegration::ValidateTargetActorPosition(
        std::uint8_t targetActorPosition,
        bool explicitTarget) const
    {
        if (!explicitTarget) {
            return true;
        }

        if (!_threads || !_threadControl ||
            targetActorPosition == 0) {
            return false;
        }

        const auto tid =
            _threadControl->GetPlayerThreadID();
        if (!_threadControl->IsThreadValid(tid)) {
            return false;
        }

        auto* thread =
            _threads->getThread(
                static_cast<std::int32_t>(tid));
        return thread &&
               targetActorPosition <= thread->getActorCount();
    }

    bool PPAIntegration::SendToParticipants(
        std::string_view payload)
    {
        if (!_transportAPI ||
            !_transportAPI->send ||
            payload.empty()) {
            return false;
        }

        const auto participants =
            SnapshotRemoteParticipants();
        bool allOK = !participants.empty();

        for (const auto connectionID : participants) {
            STRPMApi::Target target{};
            target.kind = STRPMApi::TargetKind::kPlayer;
            target.connectionID = connectionID;

            const auto result =
                _transportAPI->send(
                    kPPAChannel,
                    target,
                    payload.data(),
                    payload.size(),
                    STRPMApi::kMessageReliable |
                        STRPMApi::kMessageOrdered);

            if (result != STRPMApi::Result::kOk) {
                allOK = false;
                SKSE::log::warn(
                    "OSTNET PPA TX failed connection={} result={} bytes={}",
                    connectionID,
                    static_cast<std::uint32_t>(result),
                    payload.size());
            }
        }

        return allOK;
    }

    void PPAIntegration::SetTargetHook(
        void* tagger,
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        std::uint8_t target)
    {
        if (_setTargetOriginal) {
            _setTargetOriginal(
                tagger,
                animation,
                stage,
                performerPosition,
                target);
        }

        GetSingleton().PublishSetTarget(
            animation,
            stage,
            performerPosition,
            target);
    }

    void PPAIntegration::SetInteractionHook(
        void* tagger,
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        const StageInteractionRaw* interaction)
    {
        StageInteractionRaw snapshot{};
        const bool hasInteraction = interaction != nullptr;
        if (hasInteraction) {
            snapshot = *interaction;
        }

        if (_setInteractionOriginal) {
            _setInteractionOriginal(
                tagger,
                animation,
                stage,
                performerPosition,
                interaction);
        }

        if (hasInteraction) {
            GetSingleton().PublishSetInteraction(
                animation,
                stage,
                performerPosition,
                snapshot);
        }
    }

    void PPAIntegration::PublishSetTarget(
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        std::uint8_t target)
    {
        if (!IsLocalPlayerPerformer(performerPosition) ||
            animation.empty() ||
            animation.size() > 512 ||
            stage < 1 || stage > 64 ||
            target > static_cast<std::uint8_t>(ActionTarget::RightHand)) {
            return;
        }

        const auto payload = fmt::format(
            "SETTARGET|v=1|animation={}|stage={}|performer={}|target={}",
            HexEncode(animation),
            stage,
            performerPosition,
            target);

        const bool sent = SendToParticipants(payload);

        SKSE::log::info(
            "OSTNET PPA SETTARGET TX animation=\"{}\" stage={} performer={} target={}({}) participantsSent={}",
            animation,
            stage,
            performerPosition,
            target,
            TargetName(target),
            sent ? 1 : 0);
    }

    void PPAIntegration::PublishSetInteraction(
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        const StageInteractionRaw& interaction)
    {
        const bool explicitTarget =
            interaction.hasExplicitTargetActor != 0;

        if (!IsLocalPlayerPerformer(performerPosition) ||
            animation.empty() ||
            animation.size() > 512 ||
            stage < 1 || stage > 64 ||
            interaction.target >
                static_cast<std::uint8_t>(ActionTarget::RightHand) ||
            !ValidateTargetActorPosition(
                interaction.targetActorPosition,
                explicitTarget)) {
            return;
        }

        const auto payload = fmt::format(
            "SETINTERACTION|v=1|animation={}|stage={}|performer={}|target={}|actor={}|explicit={}",
            HexEncode(animation),
            stage,
            performerPosition,
            interaction.target,
            interaction.targetActorPosition,
            explicitTarget ? 1 : 0);

        const bool sent = SendToParticipants(payload);

        SKSE::log::info(
            "OSTNET PPA SETINTERACTION TX animation=\"{}\" stage={} performer={} target={}({}) targetActor={} explicit={} participantsSent={}",
            animation,
            stage,
            performerPosition,
            interaction.target,
            TargetName(interaction.target),
            interaction.targetActorPosition,
            explicitTarget ? 1 : 0,
            sent ? 1 : 0);
    }

    void __cdecl PPAIntegration::OnTransportMessage(
        const STRPMApi::Message* message,
        void* userData)
    {
        if (!message || !userData ||
            !message->data || message->size == 0 ||
            message->sender.connectionID == 0) {
            return;
        }

        std::string payload(
            static_cast<const char*>(message->data),
            message->size);
        const auto senderConnection =
            message->sender.connectionID;

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(
                [userData,
                 senderConnection,
                 payload = std::move(payload)]() mutable {
                    STRPMApi::Message copy{};
                    copy.data = payload.data();
                    copy.size = payload.size();
                    copy.sender.connectionID = senderConnection;

                    static_cast<PPAIntegration*>(userData)
                        ->HandleTransportMessage(copy);
                });
        }
    }

    void PPAIntegration::HandleTransportMessage(
        const STRPMApi::Message& message)
    {
        if (!_enabled.load()) {
            return;
        }

        const std::string_view payload(
            static_cast<const char*>(message.data),
            message.size);

        const auto version = ParseUInt(payload, "v");
        const auto animationHex = Field(payload, "animation");
        const auto stage = ParseInt(payload, "stage");
        const auto performer = ParseUInt(payload, "performer");
        const auto target = ParseUInt(payload, "target");

        if (!version || *version != 1 ||
            !animationHex || !stage || !performer || !target ||
            *performer == 0 || *performer > 5 ||
            *stage < 1 || *stage > 64 ||
            *target > static_cast<std::uint32_t>(
                ActionTarget::RightHand)) {
            SKSE::log::warn(
                "OSTNET PPA RX invalid header senderConnection={} payload={}",
                message.sender.connectionID,
                payload);
            return;
        }

        const auto animation = HexDecode(*animationHex);
        if (!animation || animation->empty() ||
            animation->size() > 512) {
            SKSE::log::warn(
                "OSTNET PPA RX invalid animation senderConnection={}",
                message.sender.connectionID);
            return;
        }

        const auto performerPosition =
            static_cast<std::uint8_t>(*performer);

        if (!ValidateRemoteSender(
                message.sender.connectionID,
                performerPosition)) {
            SKSE::log::warn(
                "OSTNET PPA RX rejected senderConnection={} performer={} reason=sender-not-performer-in-current-thread",
                message.sender.connectionID,
                performerPosition);
            return;
        }

        if (payload.starts_with("SETTARGET|")) {
            const bool applied =
                ApplyRemoteSetTarget(
                    *animation,
                    *stage,
                    performerPosition,
                    static_cast<std::uint8_t>(*target));

            SKSE::log::info(
                "OSTNET PPA SETTARGET RX senderConnection={} animation=\"{}\" stage={} performer={} target={}({}) applied={}",
                message.sender.connectionID,
                *animation,
                *stage,
                performerPosition,
                *target,
                TargetName(static_cast<std::uint8_t>(*target)),
                applied ? 1 : 0);
            return;
        }

        if (payload.starts_with("SETINTERACTION|")) {
            const auto actor = ParseUInt(payload, "actor");
            const auto explicitValue = ParseUInt(payload, "explicit");

            if (!actor || !explicitValue || *actor > 5 ||
                *explicitValue > 1) {
                SKSE::log::warn(
                    "OSTNET PPA SETINTERACTION RX invalid senderConnection={} payload={}",
                    message.sender.connectionID,
                    payload);
                return;
            }

            StageInteractionRaw interaction{};
            interaction.target =
                static_cast<std::uint8_t>(*target);
            interaction.targetActorPosition =
                static_cast<std::uint8_t>(*actor);
            interaction.hasExplicitTargetActor =
                static_cast<std::uint8_t>(*explicitValue);

            if (!ValidateTargetActorPosition(
                    interaction.targetActorPosition,
                    interaction.hasExplicitTargetActor != 0)) {
                SKSE::log::warn(
                    "OSTNET PPA SETINTERACTION RX rejected senderConnection={} targetActor={} explicit={} reason=invalid-target-actor-position",
                    message.sender.connectionID,
                    interaction.targetActorPosition,
                    interaction.hasExplicitTargetActor ? 1 : 0);
                return;
            }

            const bool applied =
                ApplyRemoteSetInteraction(
                    *animation,
                    *stage,
                    performerPosition,
                    interaction);

            SKSE::log::info(
                "OSTNET PPA SETINTERACTION RX senderConnection={} animation=\"{}\" stage={} performer={} target={}({}) targetActor={} explicit={} applied={}",
                message.sender.connectionID,
                *animation,
                *stage,
                performerPosition,
                interaction.target,
                TargetName(interaction.target),
                interaction.targetActorPosition,
                interaction.hasExplicitTargetActor ? 1 : 0,
                applied ? 1 : 0);
            return;
        }
    }

    bool PPAIntegration::ApplyRemoteSetTarget(
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        std::uint8_t target)
    {
        if (!CanApplyRemoteTargets()) {
            return false;
        }

        auto* tagger = _getAnimationTagger();
        if (!tagger) {
            return false;
        }

        // Call the trampoline/original directly. This intentionally bypasses
        // our entry hook, so a remotely applied target cannot echo back onto
        // the network.
        _setTargetOriginal(
            tagger,
            animation,
            stage,
            performerPosition,
            target);
        return true;
    }

    bool PPAIntegration::ApplyRemoteSetInteraction(
        const std::string& animation,
        std::int32_t stage,
        std::uint8_t performerPosition,
        const StageInteractionRaw& interaction)
    {
        if (!CanApplyRemoteTargets()) {
            return false;
        }

        auto* tagger = _getAnimationTagger();
        if (!tagger) {
            return false;
        }

        _setInteractionOriginal(
            tagger,
            animation,
            stage,
            performerPosition,
            std::addressof(interaction));
        return true;
    }
}
