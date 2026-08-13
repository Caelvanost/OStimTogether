#pragma once

#include "PCH.h"
#include "SceneCenter.h"

namespace OStimTogether
{
    class ActorResolver
    {
    public:
        static ActorResolver& GetSingleton();

        // Must be called on Skyrim's game thread.
        void HandleUdpPacket(std::string packet);

        // Resolve the other client's real player by character name. Used by
        // the generic addon bus; role=player deliberately excludes local 0x14.
        RE::Actor* ResolveRemotePlayerByName(std::string_view name);

    private:
        ActorResolver() = default;

        struct Participant
        {
            std::uint32_t remoteFormID{ 0 };
            std::string role;
            std::string name;
        };

        struct ResolveResult
        {
            RE::FormID chosen{ 0 };
            std::vector<RE::FormID> matches;
            bool localSelf{ false };
        };

        static std::vector<std::string> Split(
            std::string_view text,
            char delimiter);

        static std::string Trim(std::string value);

        static bool EqualsInsensitive(
            std::string_view lhs,
            std::string_view rhs);

        static std::optional<std::string> Field(
            std::string_view payload,
            std::string_view key);

        static std::optional<std::int32_t> ParseThreadID(
            std::string_view payload);

        static std::vector<Participant> ParseParticipants(
            std::string_view payload);

        static SceneCenter ParseSceneCenter(
            std::string_view payload);

        static FurnitureAnchor ParseFurniture(
            std::string_view payload);

        static RE::TESObjectREFR* ResolveFurniture(
            const FurnitureAnchor& furniture,
            const std::vector<RE::Actor*>& actors);

        static std::vector<ActorPose> ParseActorPoses(
            std::string_view payload);

        ResolveResult ResolveParticipant(
            const Participant& participant);

        void HandleStart(
            const std::string& sender,
            std::string_view payload);

        std::mutex _mutex;

        // key = sender|thread|participantIndex (diagnostic/cache)
        std::unordered_map<std::string, RE::FormID> _resolved;
    };
}
