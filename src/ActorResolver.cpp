#include "PCH.h"
#include "ActorResolver.h"
#include "AddonBridge.h"
#include "OStimBridge.h"

#include <cctype>
#include <limits>

namespace OStimTogether
{
    ActorResolver& ActorResolver::GetSingleton()
    {
        static ActorResolver instance;
        return instance;
    }

    std::vector<std::string> ActorResolver::Split(
        std::string_view text,
        char delimiter)
    {
        std::vector<std::string> result;
        std::size_t start = 0;

        while (start <= text.size()) {
            const auto pos = text.find(delimiter, start);

            if (pos == std::string_view::npos) {
                result.emplace_back(text.substr(start));
                break;
            }

            result.emplace_back(text.substr(start, pos - start));
            start = pos + 1;
        }

        return result;
    }

    std::string ActorResolver::Trim(std::string value)
    {
        while (!value.empty() &&
               std::isspace(
                   static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }

        while (!value.empty() &&
               std::isspace(
                   static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }

        return value;
    }

    bool ActorResolver::EqualsInsensitive(
        std::string_view lhs,
        std::string_view rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (std::size_t i = 0; i < lhs.size(); ++i) {
            const auto a = static_cast<unsigned char>(lhs[i]);
            const auto b = static_cast<unsigned char>(rhs[i]);

            if (std::tolower(a) != std::tolower(b)) {
                return false;
            }
        }

        return true;
    }

    std::optional<std::string> ActorResolver::Field(
        std::string_view payload,
        std::string_view key)
    {
        const auto needle = fmt::format("{}=", key);
        const auto pos = payload.find(needle);

        if (pos == std::string_view::npos) {
            return std::nullopt;
        }

        const auto valueStart = pos + needle.size();
        const auto valueEnd = payload.find('|', valueStart);

        if (valueEnd == std::string_view::npos) {
            return std::string(payload.substr(valueStart));
        }

        return std::string(
            payload.substr(
                valueStart,
                valueEnd - valueStart));
    }

    std::optional<std::int32_t> ActorResolver::ParseThreadID(
        std::string_view payload)
    {
        const auto value = Field(payload, "thread");
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

    SceneCenter ActorResolver::ParseSceneCenter(
        std::string_view payload)
    {
        SceneCenter center{};

        const auto value =
            Field(payload, "center");

        if (!value ||
            value->empty()) {
            return center;
        }

        const auto parts =
            Split(*value, ',');

        if (parts.size() != 4) {
            return center;
        }

        try {
            center.x = std::stof(parts[0]);
            center.y = std::stof(parts[1]);
            center.z = std::stof(parts[2]);
            center.r = std::stof(parts[3]);
            center.valid = true;
        } catch (...) {
            center.valid = false;
        }

        if (!center.IsFinite() ||
            std::abs(center.x) > 1000000.0F ||
            std::abs(center.y) > 1000000.0F ||
            std::abs(center.z) > 1000000.0F ||
            std::abs(center.r) > 100.0F) {
            center.valid = false;
        }

        return center;
    }

    FurnitureAnchor ActorResolver::ParseFurniture(
        std::string_view payload)
    {
        FurnitureAnchor result{};

        const auto value =
            Field(
                payload,
                "furniture");

        if (!value ||
            value->empty() ||
            *value == "none") {
            return result;
        }

        const auto fields =
            Split(
                *value,
                ',');

        if (fields.size() != 6) {
            return result;
        }

        try {
            result.referenceFormID =
                static_cast<std::uint32_t>(
                    std::stoul(
                        fields[0],
                        nullptr,
                        16));

            result.baseFormID =
                static_cast<std::uint32_t>(
                    std::stoul(
                        fields[1],
                        nullptr,
                        16));

            result.x =
                std::stof(fields[2]);

            result.y =
                std::stof(fields[3]);

            result.z =
                std::stof(fields[4]);

            result.r =
                std::stof(fields[5]);

            result.valid = true;
        } catch (...) {
            result.valid = false;
        }

        if (!result.IsFinite() ||
            std::abs(result.x) >
                1000000.0F ||
            std::abs(result.y) >
                1000000.0F ||
            std::abs(result.z) >
                1000000.0F ||
            std::abs(result.r) >
                100.0F) {
            result.valid = false;
        }

        return result;
    }

    RE::TESObjectREFR* ActorResolver::ResolveFurniture(
        const FurnitureAnchor& furniture,
        const std::vector<RE::Actor*>& actors)
    {
        if (!furniture.IsFinite()) {
            return nullptr;
        }

        const RE::NiPoint3 target{
            furniture.x,
            furniture.y,
            furniture.z
        };

        auto isFurniture =
            [](RE::TESObjectREFR* ref) {
                if (!ref) {
                    return false;
                }

                auto* base =
                    ref->GetBaseObject();

                return base &&
                    base->As<RE::TESFurniture>();
            };

        // Same static placed reference is the safest path.
        if (furniture.referenceFormID != 0) {
            auto* form =
                RE::TESForm::LookupByID(
                    furniture.referenceFormID);

            auto* ref =
                form ?
                    form->As<
                        RE::TESObjectREFR>() :
                    nullptr;

            if (isFurniture(ref)) {
                const auto pos =
                    ref->GetPosition();

                const float dx =
                    pos.x - target.x;
                const float dy =
                    pos.y - target.y;
                const float dz =
                    pos.z - target.z;

                const float distanceSq =
                    dx * dx +
                    dy * dy +
                    dz * dz;

                constexpr float
                    kExactMaxDistance =
                        24.0F;

                if (distanceSq <=
                    kExactMaxDistance *
                        kExactMaxDistance) {
                    SKSE::log::info(
                        "OSTNET FURNITURE receiver EXACT ref={:08X} base={:08X} name=\"{}\" pos=({:.3f},{:.3f},{:.3f},{:.5f}) distance={:.3f}",
                        ref->GetFormID(),
                        ref->GetBaseObject() ?
                            ref->
                                GetBaseObject()->
                                GetFormID() :
                            0,
                        ref->GetName(),
                        pos.x,
                        pos.y,
                        pos.z,
                        ref->GetAngleZ(),
                        std::sqrt(
                            distanceSq));

                    return ref;
                }
            }
        }

        RE::TESObjectCELL* cell = nullptr;

        for (auto* actor : actors) {
            if (!actor ||
                !actor->GetParentCell()) {
                continue;
            }

            cell =
                actor->GetParentCell();

            if (actor->IsPlayerRef()) {
                break;
            }
        }

        if (!cell) {
            SKSE::log::warn(
                "OSTNET FURNITURE receiver no-parent-cell");
            return nullptr;
        }

        RE::TESObjectREFR* best = nullptr;
        float bestDistanceSq =
            std::numeric_limits<float>::max();

        // Coordinate fallback is intentionally strict. The old v0.18.3
        // bug came from choosing an arbitrary nearby stool.
        constexpr float kSearchRadius =
            48.0F;

        cell->ForEachReferenceInRange(
            target,
            kSearchRadius,
            [&](RE::TESObjectREFR& ref) {
                auto* refPtr =
                    std::addressof(ref);

                if (!isFurniture(refPtr)) {
                    return RE::BSContainer::
                        ForEachResult::kContinue;
                }

                const auto pos =
                    ref.GetPosition();

                const float dx =
                    pos.x - target.x;
                const float dy =
                    pos.y - target.y;
                const float dz =
                    pos.z - target.z;

                const float distanceSq =
                    dx * dx +
                    dy * dy +
                    dz * dz;

                if (distanceSq <
                    bestDistanceSq) {
                    bestDistanceSq =
                        distanceSq;
                    best = refPtr;
                }

                return RE::BSContainer::
                    ForEachResult::kContinue;
            });

        if (!best ||
            bestDistanceSq >
                kSearchRadius *
                    kSearchRadius) {
            SKSE::log::warn(
                "OSTNET FURNITURE receiver MISS target=({:.3f},{:.3f},{:.3f}) radius={:.0f}; fallback=noFurniture",
                furniture.x,
                furniture.y,
                furniture.z,
                kSearchRadius);

            return nullptr;
        }

        const auto pos =
            best->GetPosition();

        SKSE::log::info(
            "OSTNET FURNITURE receiver COORD ref={:08X} base={:08X} name=\"{}\" pos=({:.3f},{:.3f},{:.3f},{:.5f}) target=({:.3f},{:.3f},{:.3f}) distance={:.3f}",
            best->GetFormID(),
            best->GetBaseObject() ?
                best->
                    GetBaseObject()->
                    GetFormID() :
                0,
            best->GetName(),
            pos.x,
            pos.y,
            pos.z,
            best->GetAngleZ(),
            furniture.x,
            furniture.y,
            furniture.z,
            std::sqrt(
                bestDistanceSq));

        return best;
    }

    std::vector<ActorPose> ActorResolver::ParseActorPoses(
        std::string_view payload)
    {
        std::vector<ActorPose> result;

        const auto value =
            Field(payload, "poses");

        if (!value ||
            value->empty()) {
            return result;
        }

        for (const auto& rawPose :
             Split(*value, ';')) {
            const auto fields =
                Split(rawPose, ':');

            if (fields.size() != 5) {
                continue;
            }

            std::size_t index = 0;

            try {
                index =
                    static_cast<std::size_t>(
                        std::stoul(fields[0]));
            } catch (...) {
                continue;
            }

            if (index > 15) {
                continue;
            }

            ActorPose pose{};

            try {
                pose.x = std::stof(fields[1]);
                pose.y = std::stof(fields[2]);
                pose.z = std::stof(fields[3]);
                pose.r = std::stof(fields[4]);
                pose.valid = true;
            } catch (...) {
                continue;
            }

            if (!pose.IsFinite() ||
                std::abs(pose.x) > 1000000.0F ||
                std::abs(pose.y) > 1000000.0F ||
                std::abs(pose.z) > 1000000.0F ||
                std::abs(pose.r) > 100.0F) {
                continue;
            }

            if (result.size() <= index) {
                result.resize(index + 1);
            }

            result[index] = pose;
        }

        return result;
    }

    std::vector<ActorResolver::Participant>
        ActorResolver::ParseParticipants(
            std::string_view payload)
    {
        std::vector<Participant> result;

        const auto actorsValue = Field(payload, "actors");
        if (!actorsValue || actorsValue->empty()) {
            return result;
        }

        for (const auto& rawActor : Split(*actorsValue, ',')) {
            const auto first = rawActor.find(':');
            if (first == std::string::npos) {
                continue;
            }

            const auto second =
                rawActor.find(':', first + 1);

            if (second == std::string::npos) {
                continue;
            }

            Participant p{};

            try {
                p.remoteFormID =
                    static_cast<std::uint32_t>(
                        std::stoul(
                            rawActor.substr(0, first),
                            nullptr,
                            16));
            } catch (...) {
                p.remoteFormID = 0;
            }

            p.role =
                Trim(rawActor.substr(
                    first + 1,
                    second - first - 1));

            p.name =
                Trim(rawActor.substr(second + 1));

            result.push_back(std::move(p));
        }

        return result;
    }

    ActorResolver::ResolveResult
        ActorResolver::ResolveParticipant(
            const Participant& participant)
    {
        ResolveResult result{};

        if (participant.name.empty()) {
            return result;
        }

        auto* localPlayer =
            RE::PlayerCharacter::GetSingleton();

        // Important multiplayer identity rule:
        //
        // On the sender, only THAT client's real PlayerCharacter is
        // actor->IsPlayerRef().  A remote STR player proxy is serialized
        // as role=actor.  When the received participant name exactly
        // matches our own local PlayerCharacter, resolve that participant
        // directly to FormID 0x14.
        //
        // Never do this for role=player: role=player is explicitly the
        // sender's own local player and must remain a remote STR proxy here.
        if (participant.role != "player" &&
            localPlayer) {
            const auto* localName =
                localPlayer->GetName();

            if (localName &&
                EqualsInsensitive(
                    localName,
                    participant.name)) {
                result.chosen =
                    localPlayer->GetFormID();

                result.matches.push_back(
                    result.chosen);

                result.localSelf = true;

                return result;
            }
        }

        auto* processLists =
            RE::ProcessLists::GetSingleton();

        if (!processLists) {
            return result;
        }

        struct Candidate
        {
            RE::FormID formID{ 0 };
            float distance{ 0.0F };
        };

        std::vector<Candidate> candidates;
        std::unordered_set<RE::FormID> seen;

        auto considerActor =
            [&](RE::Actor* actor) {
                if (!actor) {
                    return;
                }

                const auto formID =
                    actor->GetFormID();

                if (!seen.insert(formID).second) {
                    return;
                }

                // Incoming role=player means the OTHER client's
                // player. Never resolve it to our local 0x14.
                if (participant.role == "player" &&
                    actor == localPlayer) {
                    return;
                }

                const auto* actorName =
                    actor->GetName();

                if (!actorName ||
                    !EqualsInsensitive(
                        actorName,
                        participant.name)) {
                    return;
                }

                float distance = 0.0F;

                if (localPlayer) {
                    distance =
                        actor->GetPosition().
                        GetDistance(
                            localPlayer->GetPosition());
                }

                candidates.push_back(
                    Candidate{
                        formID,
                        distance });

                result.matches.push_back(
                    formID);
            };

        auto scanHandles =
            [&](const auto& handles) {
                for (const auto& handle : handles) {
                    const auto actorPtr =
                        handle.get();

                    considerActor(
                        actorPtr.get());
                }
            };

        // CommonLibSSE-NG 3.5.3 does not expose
        // ProcessLists::ForAllActors(). Iterate the four
        // public process arrays directly instead.
        scanHandles(processLists->highActorHandles);
        scanHandles(processLists->middleHighActorHandles);
        scanHandles(processLists->middleLowActorHandles);
        scanHandles(processLists->lowActorHandles);

        if (candidates.empty()) {
            return result;
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& lhs,
               const Candidate& rhs) {
                return lhs.distance < rhs.distance;
            });

        result.chosen =
            candidates.front().formID;

        return result;
    }

    RE::Actor* ActorResolver::ResolveRemotePlayerByName(
        std::string_view name)
    {
        Participant participant{};
        participant.role = "player";
        participant.name = Trim(std::string(name));

        const auto resolved =
            ResolveParticipant(participant);

        if (!resolved.chosen) {
            return nullptr;
        }

        auto* form =
            RE::TESForm::LookupByID(resolved.chosen);

        return form ? form->As<RE::Actor>() : nullptr;
    }

    void ActorResolver::HandleStart(
        const std::string& sender,
        std::string_view payload)
    {
        const auto threadID =
            ParseThreadID(payload);

        if (!threadID) {
            SKSE::log::error(
                "OSTNET RESOLVE START: invalid thread field");
            return;
        }

        const auto nodeValue =
            Field(payload, "node");

        if (!nodeValue ||
            nodeValue->empty()) {
            SKSE::log::error(
                "OSTNET RESOLVE START sender={} thread={}: missing node",
                sender,
                *threadID);
            return;
        }

        const auto participants =
            ParseParticipants(payload);

        const auto authoritativeCenter =
            ParseSceneCenter(payload);

        const auto furnitureDescriptor =
            ParseFurniture(payload);

        const auto authoritativePoses =
            ParseActorPoses(payload);

        SKSE::log::info(
            "OSTNET RESOLVE START sender={} thread={} participants={} node={} centerValid={} center=({:.3f},{:.3f},{:.3f},{:.5f}) furnitureValid={} furnitureRef={:08X} poses={}",
            sender,
            *threadID,
            participants.size(),
            *nodeValue,
            authoritativeCenter.IsFinite() ? 1 : 0,
            authoritativeCenter.x,
            authoritativeCenter.y,
            authoritativeCenter.z,
            authoritativeCenter.r,
            furnitureDescriptor.IsFinite() ? 1 : 0,
            furnitureDescriptor.referenceFormID,
            authoritativePoses.size());

        std::vector<RE::Actor*> resolvedActors;
        resolvedActors.reserve(
            participants.size());

        std::vector<bool> localAlignmentMask;
        localAlignmentMask.reserve(
            participants.size());

        std::int32_t localSelfIndex = -1;

        bool allResolved = true;

        for (std::size_t i = 0;
             i < participants.size();
             ++i) {
            const auto& participant =
                participants[i];

            const auto resolved =
                ResolveParticipant(participant);

            std::string matchList;

            for (std::size_t m = 0;
                 m < resolved.matches.size();
                 ++m) {
                if (m != 0) {
                    matchList += ",";
                }

                matchList += fmt::format(
                    "{:08X}",
                    resolved.matches[m]);
            }

            if (resolved.chosen == 0) {
                allResolved = false;

                SKSE::log::warn(
                    "OSTNET RESOLVE MISS sender={} thread={} idx={} role={} remoteForm={:08X} name=\"{}\" matches=[{}]",
                    sender,
                    *threadID,
                    i,
                    participant.role,
                    participant.remoteFormID,
                    participant.name,
                    matchList);

                resolvedActors.push_back(
                    nullptr);

                localAlignmentMask.push_back(
                    false);

                continue;
            }

            auto* form =
                RE::TESForm::LookupByID(
                    resolved.chosen);

            auto* actor =
                form ?
                form->As<RE::Actor>() :
                nullptr;

            if (!actor) {
                allResolved = false;

                SKSE::log::warn(
                    "OSTNET RESOLVE vanished localForm={:08X}",
                    resolved.chosen);

                resolvedActors.push_back(
                    nullptr);

                localAlignmentMask.push_back(
                    false);

                continue;
            }

            auto* base =
                actor->GetActorBase();

            SKSE::log::info(
                "OSTNET RESOLVE {} sender={} thread={} idx={} role={} remoteForm={:08X} name=\"{}\" -> localForm={:08X} base={:08X} matches={} [{}]",
                resolved.localSelf ? "SELF" : "OK",
                sender,
                *threadID,
                i,
                participant.role,
                participant.remoteFormID,
                participant.name,
                resolved.chosen,
                base ? base->GetFormID() : 0,
                resolved.matches.size(),
                matchList);

            resolvedActors.push_back(
                actor);

            // Position ownership:
            //
            // role=player is the sender's real PlayerCharacter. On this
            // client it is an STR remote proxy. Do not use OStim's generic
            // alignment keepalive for it; OStimBridge instead applies the
            // authoritative network pose through the dedicated per-frame
            // STR proxy guard for the lifetime of the scene.
            //
            // Every other participant remains locally aligned. This
            // includes NPCs and RESOLVE SELF -> 00000014.
            // Position authority in v0.18.1c:
            //
            // sender role=player proxy -> dedicated pose guard
            // normal NPC              -> OStim Together keepalive
            // RESOLVE SELF 0x14       -> pre-anchor before builder->start(),
            //                            then OStim owns normal alignment
            //
            // Once SELF has been moved to the authoritative center BEFORE
            // thread construction, OStim's own Thread::center is correct.
            // We can therefore let OStim use its native GraphActor offsets
            // and lockAtPosition() instead of reconstructing them ourselves.
            const bool isLocalSelf =
                resolved.localSelf &&
                actor->IsPlayerRef();

            if (isLocalSelf) {
                localSelfIndex =
                    static_cast<std::int32_t>(i);
            }

            const bool alignLocally =
                participant.role != "player";

            localAlignmentMask.push_back(
                alignLocally);

            const char* owner =
                isLocalSelf ?
                    "OStimPreAnchor" :
                    (alignLocally ?
                        "OStimTogether" :
                        "OStimTogetherPoseGuard");

            SKSE::log::info(
                "OSTNET POSITION OWNER idx={} name=\"{}\" role={} owner={}",
                i,
                participant.name,
                participant.role,
                owner);

            const auto key =
                fmt::format(
                    "{}|{}|{}",
                    sender,
                    *threadID,
                    i);

            {
                std::scoped_lock lock(_mutex);

                _resolved[key] =
                    resolved.chosen;
            }
        }

        if (!allResolved ||
            resolvedActors.empty()) {
            SKSE::log::warn(
                "OSTNET MIRROR not started sender={} thread={} resolved={}/{}",
                sender,
                *threadID,
                std::count_if(
                    resolvedActors.begin(),
                    resolvedActors.end(),
                    [](RE::Actor* actor) {
                        return actor != nullptr;
                    }),
                resolvedActors.size());

            return;
        }

        auto* localFurniture =
            ResolveFurniture(
                furnitureDescriptor,
                resolvedActors);

        OStimBridge::GetSingleton()
            .StartRemoteMirror(
                sender,
                *threadID,
                resolvedActors,
                localAlignmentMask,
                localSelfIndex,
                authoritativeCenter,
                authoritativePoses,
                localFurniture,
                *nodeValue);
    }

    void ActorResolver::HandleUdpPacket(
        std::string packet)
    {
        constexpr std::string_view prefix =
            "OSTUDP|v1|from=";

        if (!packet.starts_with(prefix)) {
            return;
        }

        const auto senderStart =
            prefix.size();

        const auto senderEnd =
            packet.find('|', senderStart);

        if (senderEnd == std::string::npos) {
            return;
        }

        const std::string sender =
            packet.substr(
                senderStart,
                senderEnd - senderStart);

        const std::string_view payload{
            packet.data() + senderEnd + 1,
            packet.size() - senderEnd - 1
        };

        if (payload.starts_with("ADDONOVR|") ||
            payload.starts_with("ADDONOBJ|")) {
            AddonBridge::GetSingleton()
                .HandleRemotePacket(sender, payload);
            return;
        }

        if (payload.starts_with("START|")) {
            HandleStart(sender, payload);
            return;
        }

        if (payload.starts_with("NODE|")) {
            const auto threadID =
                ParseThreadID(payload);

            const auto nodeValue =
                Field(payload, "node");

            if (!threadID ||
                !nodeValue ||
                nodeValue->empty()) {
                SKSE::log::warn(
                    "OSTNET MIRROR NODE invalid packet sender={}",
                    sender);
                return;
            }

            const auto authoritativePoses =
                ParseActorPoses(payload);

            OStimBridge::GetSingleton()
                .NavigateRemoteMirror(
                    sender,
                    *threadID,
                    *nodeValue,
                    authoritativePoses);

            return;
        }

        if (payload.starts_with("SPEED|")) {
            const auto threadID =
                ParseThreadID(payload);
            const auto speedValue =
                Field(payload, "speed");

            if (!threadID || !speedValue) {
                SKSE::log::warn(
                    "OSTNET MIRROR SPEED invalid packet sender={}",
                    sender);
                return;
            }

            try {
                const auto speed =
                    static_cast<std::int32_t>(
                        std::stol(*speedValue));

                OStimBridge::GetSingleton()
                    .SetRemoteMirrorSpeed(
                        sender,
                        *threadID,
                        speed);
            } catch (...) {
                SKSE::log::warn(
                    "OSTNET MIRROR SPEED invalid value sender={} value={}",
                    sender,
                    *speedValue);
            }

            return;
        }

        if (payload.starts_with("STOP|")) {
            const auto threadID =
                ParseThreadID(payload);

            if (!threadID) {
                SKSE::log::warn(
                    "OSTNET MIRROR STOP invalid packet sender={}",
                    sender);
                return;
            }

            OStimBridge::GetSingleton()
                .StopRemoteMirror(
                    sender,
                    *threadID);

            return;
        }
    }
}
