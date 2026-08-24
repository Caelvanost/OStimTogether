#pragma once

#include "PCH.h"

#include "AccuratePenetrationAPI.h"
#include "STRPMApi.h"
#include "OStimAPI/ThreadInterface.h"
#include "OStimAPI/ModThreadControl.h"

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
            return _ppaModule != nullptr && _hooksInstalled;
        }

        [[nodiscard]] bool CanApplyRemoteTargets() const noexcept
        {
            return _hooksInstalled &&
                   _getAnimationTagger &&
                   _setTargetOriginal &&
                   _setInteractionOriginal;
        }

    private:
        enum class ActionTarget : std::uint8_t
        {
            Auto = 0,
            None = 1,
            Vagina = 2,
            Anus = 3,
            Mouth = 4,
            Hand = 5,
            LeftHand = 6,
            RightHand = 7
        };

#pragma pack(push, 1)
        struct StageInteractionRaw
        {
            std::uint8_t target{ 0 };
            std::uint8_t targetActorPosition{ 0 };
            std::uint8_t hasExplicitTargetActor{ 0 };
        };
#pragma pack(pop)
        static_assert(sizeof(StageInteractionRaw) == 3);

        using GetAnimationTaggerFn = void*(__cdecl*)();
        using SetTargetFn = void (*)(
            void* tagger,
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            std::uint8_t target);
        using SetInteractionFn = void (*)(
            void* tagger,
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            const StageInteractionRaw* interaction);

        PPAIntegration() = default;
        ~PPAIntegration();

        PPAIntegration(const PPAIntegration&) = delete;
        PPAIntegration& operator=(const PPAIntegration&) = delete;

        static void SetTargetHook(
            void* tagger,
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            std::uint8_t target);

        static void SetInteractionHook(
            void* tagger,
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            const StageInteractionRaw* interaction);

        static void __cdecl OnTransportMessage(
            const STRPMApi::Message* message,
            void* userData);

        bool IsOptionalIntegrationInstalled() const;
        bool ConnectOStim();
        bool ConnectPPA();
        bool ConnectTransport();
        bool ValidateExactPPABuild(HMODULE module) const;
        bool InstallPPAHooks(HMODULE module);
        void Disconnect();

        bool IsLocalPlayerPerformer(std::uint8_t performerPosition) const;
        bool ValidateRemoteSender(
            STRPMApi::ConnectionID senderConnectionID,
            std::uint8_t performerPosition) const;
        bool ValidateTargetActorPosition(
            std::uint8_t targetActorPosition,
            bool explicitTarget) const;

        std::vector<STRPMApi::ConnectionID>
            SnapshotRemoteParticipants() const;

        void PublishSetTarget(
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            std::uint8_t target);

        void PublishSetInteraction(
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            const StageInteractionRaw& interaction);

        bool SendToParticipants(std::string_view payload);
        void HandleTransportMessage(const STRPMApi::Message& message);

        bool ApplyRemoteSetTarget(
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            std::uint8_t target);

        bool ApplyRemoteSetInteraction(
            const std::string& animation,
            std::int32_t stage,
            std::uint8_t performerPosition,
            const StageInteractionRaw& interaction);

        static const char* TargetName(std::uint8_t target) noexcept;
        static std::string HexEncode(std::string_view value);
        static std::optional<std::string> HexDecode(std::string_view value);

        std::atomic_bool _enabled{ false };
        bool _hooksInstalled{ false };

        HMODULE _ppaModule{ nullptr };
        const AccuratePenetration::API::InterfaceV1* _ppaAPI{ nullptr };
        GetAnimationTaggerFn _getAnimationTagger{ nullptr };

        inline static SetTargetFn _setTargetOriginal{ nullptr };
        inline static SetInteractionFn _setInteractionOriginal{ nullptr };

        OStim::ThreadInterface* _threads{ nullptr };
        OStimModAPI::Thread::IThreadInterface* _threadControl{ nullptr };

        const STRPMApi::Interface* _transportAPI{ nullptr };
        STRPMApi::ListenerHandle _transportListener{};
    };
}
