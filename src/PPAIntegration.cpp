#include "PCH.h"
#include "PPAIntegration.h"

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

    std::uint16_t PPAIntegration::TargetKey(
        std::uint8_t performerPosition,
        std::uint8_t receiverPosition) noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(performerPosition) << 8) |
            static_cast<std::uint16_t>(receiverPosition));
    }

    const char* PPAIntegration::SiteName(
        AccuratePenetration::API::PenetrationSite site) noexcept
    {
        using Site = AccuratePenetration::API::PenetrationSite;

        switch (site) {
        case Site::None:
            return "None";
        case Site::Mouth:
            return "Mouth";
        case Site::Anus:
            return "Anus";
        case Site::Vagina:
            return "Vagina";
        case Site::Both:
            return "Both";
        case Site::HandL:
            return "HandL";
        case Site::HandR:
            return "HandR";
        case Site::Hands:
            return "Hands";
        default:
            return "Unknown";
        }
    }

    bool PPAIntegration::IsOptionalIntegrationInstalled() const
    {
        std::error_code ec;
        return std::filesystem::exists(kMarkerPath, ec) && !ec;
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

        if (!api ||
            api->version != AccuratePenetration::API::kVersion ||
            api->size < sizeof(AccuratePenetration::API::InterfaceV1) ||
            !api->RegisterAnimationUpdateListener ||
            !api->UnregisterAnimationUpdateListener) {
            SKSE::log::warn(
                "OSTNET PPA API unavailable or incompatible expectedVersion={}",
                AccuratePenetration::API::kVersion);
            return false;
        }

        const auto listener =
            api->RegisterAnimationUpdateListener(
                &PPAIntegration::OnPPAUpdate,
                this);

        if (listener == 0) {
            SKSE::log::warn(
                "OSTNET PPA API listener registration failed");
            return false;
        }

        _ppaAPI = api;
        _ppaListener = listener;

        SKSE::log::info(
            "OSTNET PPA API READY version={} listener={} targetRead=1 targetWrite=0",
            api->version,
            listener);
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
        const auto queryResult =
            query ?
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
            "OSTNET PPA TRANSPORT READY channel={} apiVersion={}",
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

        if (!ConnectPPA()) {
            Disconnect();
            return false;
        }

        if (!ConnectTransport()) {
            Disconnect();
            return false;
        }

        _enabled.store(true);

        SKSE::log::info(
            "OSTNET PPA integration READY localTargetAuthority=local-player-performer remoteApply=deferred reason=ppa-api-v1-read-only");
        return true;
    }

    void PPAIntegration::Disconnect()
    {
        _enabled.store(false);

        if (_ppaAPI &&
            _ppaListener != 0 &&
            _ppaAPI->UnregisterAnimationUpdateListener) {
            _ppaAPI->UnregisterAnimationUpdateListener(_ppaListener);
        }

        if (_transportAPI &&
            _transportListener.value != 0 &&
            _transportAPI->unregisterChannel) {
            _transportAPI->unregisterChannel(_transportListener);
        }

        _ppaListener = 0;
        _ppaAPI = nullptr;
        _transportListener = {};
        _transportAPI = nullptr;

        Reset();
    }

    void PPAIntegration::Reset()
    {
        std::scoped_lock lock(_stateMutex);
        _lastSent.clear();
        _remoteDesired.clear();
    }

    void __cdecl PPAIntegration::OnPPAUpdate(
        const AccuratePenetration::API::AnimationUpdateEvent* event,
        void* userData)
    {
        if (!event || !userData ||
            event->apiVersion != AccuratePenetration::API::kVersion ||
            event->size < sizeof(AccuratePenetration::API::AnimationUpdateEvent)) {
            return;
        }

        static_cast<PPAIntegration*>(userData)->HandlePPAUpdate(*event);
    }

    void PPAIntegration::HandlePPAUpdate(
        const AccuratePenetration::API::AnimationUpdateEvent& event)
    {
        if (!_enabled.load()) {
            return;
        }

        auto* localPlayer =
            RE::PlayerCharacter::GetSingleton();
        if (!localPlayer) {
            return;
        }

        if (event.ending) {
            std::scoped_lock lock(_stateMutex);
            for (auto it = _lastSent.begin();
                 it != _lastSent.end();) {
                if (it->second.receiverPosition == event.position) {
                    it = _lastSent.erase(it);
                } else {
                    ++it;
                }
            }
            return;
        }

        const auto context =
            static_cast<std::uint32_t>(event.context);

        const auto publishPartner =
            [this,
             localPlayer,
             receiverPosition = event.position,
             context](
                const AccuratePenetration::API::InteractionPartner& partner) {
                auto performerPtr = partner.actor.get();
                auto* performer = performerPtr.get();

                // Only the real local player is authoritative. This prevents
                // the locally simulated STR proxy from echoing remote state
                // back into the network and gives each player ownership of
                // their own PPA target selection.
                if (!performer || performer != localPlayer) {
                    return;
                }

                TargetState state{};
                state.performerPosition = partner.position;
                state.receiverPosition = receiverPosition;
                state.site = partner.site;
                state.context = context;

                if (state.performerPosition == 0 ||
                    state.receiverPosition == 0) {
                    return;
                }

                const auto key =
                    TargetKey(
                        state.performerPosition,
                        state.receiverPosition);

                {
                    std::scoped_lock lock(_stateMutex);
                    const auto it = _lastSent.find(key);
                    if (it != _lastSent.end() &&
                        it->second.site == state.site &&
                        it->second.context == state.context) {
                        return;
                    }
                    _lastSent[key] = state;
                }

                SendTargetState(state);
            };

        for (std::uint32_t i = 0; i < event.actorCount; ++i) {
            publishPartner(event.actors[i]);
        }

        if (event.selfInteraction) {
            publishPartner(*event.selfInteraction);
        }
    }

    bool PPAIntegration::SendTargetState(const TargetState& state)
    {
        if (!_transportAPI || !_transportAPI->send) {
            return false;
        }

        const auto payload = fmt::format(
            "TARGET|performer={}|receiver={}|site={}|context={}",
            state.performerPosition,
            state.receiverPosition,
            static_cast<std::uint32_t>(state.site),
            state.context);

        STRPMApi::Target target{};
        target.kind = STRPMApi::TargetKind::kAllPlayers;

        const auto result =
            _transportAPI->send(
                kPPAChannel,
                target,
                payload.data(),
                payload.size(),
                STRPMApi::kMessageReliable |
                    STRPMApi::kMessageOrdered);

        SKSE::log::info(
            "OSTNET PPA TARGET TX performerPos={} receiverPos={} site={}({}) context=0x{:X} result={}",
            state.performerPosition,
            state.receiverPosition,
            static_cast<std::uint32_t>(state.site),
            SiteName(state.site),
            state.context,
            static_cast<std::uint32_t>(result));

        return result == STRPMApi::Result::kOk;
    }

    void __cdecl PPAIntegration::OnTransportMessage(
        const STRPMApi::Message* message,
        void* userData)
    {
        if (!message || !userData ||
            !message->data || message->size == 0) {
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
        const std::string_view payload(
            static_cast<const char*>(message.data),
            message.size);

        if (!payload.starts_with("TARGET|")) {
            return;
        }

        const auto performer = ParseUInt(payload, "performer");
        const auto receiver = ParseUInt(payload, "receiver");
        const auto site = ParseUInt(payload, "site");
        const auto context = ParseUInt(payload, "context");

        if (!performer || !receiver || !site || !context ||
            *performer == 0 || *performer > 5 ||
            *receiver == 0 || *receiver > 5 ||
            *site > static_cast<std::uint32_t>(
                AccuratePenetration::API::PenetrationSite::Hands)) {
            SKSE::log::warn(
                "OSTNET PPA TARGET RX invalid senderConnection={} payload={}",
                message.sender.connectionID,
                payload);
            return;
        }

        TargetState state{};
        state.performerPosition =
            static_cast<std::uint8_t>(*performer);
        state.receiverPosition =
            static_cast<std::uint8_t>(*receiver);
        state.site =
            static_cast<AccuratePenetration::API::PenetrationSite>(*site);
        state.context = *context;

        {
            std::scoped_lock lock(_stateMutex);
            _remoteDesired[
                TargetKey(
                    state.performerPosition,
                    state.receiverPosition)] = state;
        }

        const bool applied = ApplyRemoteTarget(state);

        SKSE::log::info(
            "OSTNET PPA TARGET RX senderConnection={} performerPos={} receiverPos={} site={}({}) context=0x{:X} applied={}",
            message.sender.connectionID,
            state.performerPosition,
            state.receiverPosition,
            static_cast<std::uint32_t>(state.site),
            SiteName(state.site),
            state.context,
            applied ? 1 : 0);
    }

    bool PPAIntegration::ApplyRemoteTarget(const TargetState& state)
    {
        // Accurate Penetration API V1 intentionally exposes observation only:
        // listener registration plus AnimationUpdateEvent snapshots. It has no
        // supported function that changes the active per-stage target selected
        // by PPA's in-game menu. Keep the received state cached behind this
        // single application point so a future official setter (or a verified
        // internal bridge) can be added without changing the network protocol.
        SKSE::log::warn(
            "OSTNET PPA TARGET APPLY deferred performerPos={} receiverPos={} site={}({}) reason=ppa-api-v1-read-only",
            state.performerPosition,
            state.receiverPosition,
            static_cast<std::uint32_t>(state.site),
            SiteName(state.site));
        return false;
    }
}
