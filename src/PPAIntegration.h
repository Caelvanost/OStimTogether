#pragma once

#include "PCH.h"

#include "AccuratePenetrationAPI.h"
#include "STRPMApi.h"

namespace OStimTogether
{
    class PPAIntegration final
    {
    public:
        static PPAIntegration& GetSingleton();

        bool Initialize();
        void Reset();

        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return _enabled.load();
        }

        [[nodiscard]] bool IsPPAConnected() const noexcept
        {
            return _ppaAPI != nullptr && _ppaListener != 0;
        }

        [[nodiscard]] bool CanApplyRemoteTargets() const noexcept
        {
            return false;
        }

    private:
        struct TargetState
        {
            std::uint8_t performerPosition{ 0 };
            std::uint8_t receiverPosition{ 0 };
            AccuratePenetration::API::PenetrationSite site{
                AccuratePenetration::API::PenetrationSite::None };
            std::uint32_t context{ 0 };
        };

        PPAIntegration() = default;
        ~PPAIntegration();

        PPAIntegration(const PPAIntegration&) = delete;
        PPAIntegration& operator=(const PPAIntegration&) = delete;

        static void __cdecl OnPPAUpdate(
            const AccuratePenetration::API::AnimationUpdateEvent* event,
            void* userData);

        static void __cdecl OnTransportMessage(
            const STRPMApi::Message* message,
            void* userData);

        bool IsOptionalIntegrationInstalled() const;
        bool ConnectPPA();
        bool ConnectTransport();
        void Disconnect();

        void HandlePPAUpdate(
            const AccuratePenetration::API::AnimationUpdateEvent& event);
        void HandleTransportMessage(const STRPMApi::Message& message);

        bool SendTargetState(const TargetState& state);
        bool ApplyRemoteTarget(const TargetState& state);

        static std::uint16_t TargetKey(
            std::uint8_t performerPosition,
            std::uint8_t receiverPosition) noexcept;
        static const char* SiteName(
            AccuratePenetration::API::PenetrationSite site) noexcept;

        std::atomic_bool _enabled{ false };

        const AccuratePenetration::API::InterfaceV1* _ppaAPI{ nullptr };
        AccuratePenetration::API::ListenerHandle _ppaListener{ 0 };

        const STRPMApi::Interface* _transportAPI{ nullptr };
        STRPMApi::ListenerHandle _transportListener{};

        mutable std::mutex _stateMutex;
        std::unordered_map<std::uint16_t, TargetState> _lastSent;
        std::unordered_map<std::uint16_t, TargetState> _remoteDesired;
    };
}
