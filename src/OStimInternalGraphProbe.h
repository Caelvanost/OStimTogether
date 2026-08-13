#pragma once

#include "PCH.h"
#include "OStimAPI/Node.h"

#include <cmath>
#include <cstddef>

namespace OStimTogether::OStimInternalProbe
{
    // READ-ONLY ABI probe.
    //
    // OStim's public Node interface deliberately does not expose:
    //   - Speed::animation
    //   - Speed::playbackSpeed
    //   - GraphActor::animationIndex
    //
    // The concrete object behind OStim::Node is Graph::Node.
    //
    // IMPORTANT: this layout is pinned specifically to OStim 7.4c /
    // OStim.dll 7.4.0.3. GraphActor changed in later development versions.
    // Do not copy the current-main GraphActor layout over this probe.
    //
    // We mirror only the initial data layout required for read-only probes:
    //
    //   speed.animation + "_" + graphActor.animationIndex
    //
    // Nothing through these layouts is ever written.

    struct Speed
    {
        std::string animation;
        float playbackSpeed;
        float displaySpeed;
    };

    struct NodePrefix
    {
        void* vtbl;
        std::string sceneID;
        std::string lowercaseID;
        std::uint32_t numActors;
        std::uint32_t pad4C;
        std::string sceneName;
        std::string lowercaseName;
        std::vector<Speed> speeds;
        std::uint32_t defaultSpeed;
    };

    struct GamePosition
    {
        float x;
        float y;
        float z;
        float r;
    };

    struct NodeActorPrefix
    {
        void* vtbl;
        std::int32_t sosBend;
        float scale;
        float scaleHeight;
        bool feetOnGround;
        std::uint8_t pad15[3];
        std::int32_t expressionAction;
        std::int32_t animationIndex;
        std::string underlyingExpression;
        std::string expressionOverride;
        bool noStrip;
        bool moan;
        bool talk;
        bool muffled;
        GamePosition offset;
    };

#if defined(_MSC_VER) && defined(_WIN64)
    static_assert(sizeof(std::string) == 0x20);
    static_assert(sizeof(std::vector<Speed>) == 0x18);
    static_assert(offsetof(NodePrefix, speeds) == 0x90);
    static_assert(offsetof(NodePrefix, defaultSpeed) == 0xA8);
    static_assert(offsetof(NodeActorPrefix, animationIndex) == 0x1C);
    static_assert(offsetof(NodeActorPrefix, offset) == 0x64);
#endif

    inline bool GetActorOffset(
        OStim::Node* node,
        std::uint32_t actorIndex,
        GamePosition& out)
    {
        if (!node ||
            actorIndex >= node->getActorCount()) {
            return false;
        }

        auto* nodeActor =
            node->getActor(actorIndex);

        if (!nodeActor) {
            return false;
        }

        const auto* concrete =
            reinterpret_cast<
                const NodeActorPrefix*>(
                    nodeActor);

        const auto value =
            concrete->offset;

        if (!std::isfinite(value.x) ||
            !std::isfinite(value.y) ||
            !std::isfinite(value.z) ||
            !std::isfinite(value.r) ||
            std::abs(value.x) > 10000.0F ||
            std::abs(value.y) > 10000.0F ||
            std::abs(value.z) > 10000.0F ||
            std::abs(value.r) > 3600.0F) {
            return false;
        }

        out = value;
        return true;
    }

    struct EventInfo
    {
        bool valid{ false };
        std::string eventName;
        float playbackSpeed{ 1.0F };
        std::int32_t animationIndex{ -1 };
        std::int32_t speedIndex{ -1 };
        std::string reason;
    };

    inline bool IsReasonableAnimationName(std::string_view value)
    {
        if (value.empty() || value.size() > 240) {
            return false;
        }

        // OStim/OAR animation event identifiers are printable ASCII in the
        // currently supported scene packs.  Reject obviously invalid memory
        // before constructing/logging an event.
        for (const unsigned char c : value) {
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }

        return true;
    }

    inline EventInfo BuildEvent(
        OStim::Node* node,
        std::uint32_t actorIndex,
        std::int32_t speedIndex)
    {
        EventInfo out{};
        out.speedIndex = speedIndex;

        if (!node) {
            out.reason = "null-node";
            return out;
        }

        if (speedIndex < 0 || speedIndex > 31) {
            out.reason = "speed-index-range";
            return out;
        }

        const auto* concrete =
            reinterpret_cast<const NodePrefix*>(node);

        const auto speedCount =
            concrete->speeds.size();

        if (speedCount == 0 || speedCount > 32) {
            out.reason = "speed-vector-range";
            return out;
        }

        if (static_cast<std::size_t>(speedIndex) >= speedCount) {
            out.reason = "speed-index-oob";
            return out;
        }

        if (actorIndex >= node->getActorCount()) {
            out.reason = "actor-index-oob";
            return out;
        }

        auto* nodeActor =
            node->getActor(actorIndex);

        if (!nodeActor) {
            out.reason = "null-node-actor";
            return out;
        }

        const auto* concreteActor =
            reinterpret_cast<const NodeActorPrefix*>(nodeActor);

        const auto animationIndex =
            concreteActor->animationIndex;

        if (animationIndex < 0 || animationIndex > 15) {
            out.reason = "animation-index-range";
            return out;
        }

        const auto& speed =
            concrete->speeds[
                static_cast<std::size_t>(speedIndex)];

        if (!IsReasonableAnimationName(speed.animation)) {
            out.reason = "animation-name-invalid";
            return out;
        }

        if (!std::isfinite(speed.playbackSpeed) ||
            speed.playbackSpeed <= 0.0F ||
            speed.playbackSpeed > 10.0F) {
            out.reason = "playback-speed-invalid";
            return out;
        }

        out.valid = true;
        out.animationIndex = animationIndex;
        out.playbackSpeed = speed.playbackSpeed;
        out.eventName = fmt::format(
            "{}_{}",
            speed.animation,
            animationIndex);

        return out;
    }
}
