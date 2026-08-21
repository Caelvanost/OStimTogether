#include "PCH.h"
#include "CoopSessionManager.h"

#include "ActorResolver.h"
#include "STRPMTransport.h"

#include <limits>

namespace OStimTogether
{
    std::int32_t CoopSessionManager::BeginAddActorConsent(
        std::string_view selectedLabel)
    {
        if (selectedLabel.empty()) {
            return 0;
        }

        auto* actor = ActorResolver::GetSingleton()
            .ResolveRemotePlayerByName(selectedLabel);
        if (!actor || actor->IsPlayerRef()) {
            return 0;
        }

        const auto connection = STRPMTransport::GetSingleton()
            .ResolveConnection(actor->GetFormID());
        if (!connection || *connection == 0) {
            return 0;
        }

        std::uint64_t parentSessionID = 0;
        {
            std::scoped_lock lock(_mutex);
            if (_approvedReplayArmed) {
                const auto parentIt = _ownerSessions.find(*_approvedReplayArmed);
                if (parentIt != _ownerSessions.end() &&
                    !parentIt->second.canceled) {
                    if (parentIt->second.accepted.contains(*connection)) {
                        SKSE::log::info(
                            "OSTNET ADD-ACTOR GATE already-approved connection={} label=\"{}\" parentSession={}",
                            *connection,
                            selectedLabel,
                            *_approvedReplayArmed);
                        return 0;
                    }
                    parentSessionID = *_approvedReplayArmed;
                }
            }
        }

        OwnerSession gate{};
        gate.sessionID = _nextSessionID.fetch_add(1);
        gate.participants.insert(*connection);
        gate.nodeID = "add-actor";

        // This is a consent-only pseudo-session, not an OStim scene request.
        // HandleInviteResponse uses the normal accepted-session path and may
        // call StartApprovedOwnerSession(). A deliberately invalid actor FormID
        // makes that function exit through actor-missing before it can ever
        // invoke OStim StartScene(). PollAddActorConsent then re-arms the gate
        // for the REAL thread OStim creates after UIExtMessageBox returns.
        gate.actorFormIDs.push_back(0);

        const auto gateID = gate.sessionID;
        if (gateID > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int32_t>::max())) {
            SKSE::log::error(
                "OSTNET ADD-ACTOR GATE exhausted Papyrus-compatible IDs gate={}",
                gateID);
            return 0;
        }

        {
            std::scoped_lock lock(_mutex);
            _ownerSessions[gateID] = std::move(gate);
            if (parentSessionID != 0) {
                _addActorGateParents[gateID] = parentSessionID;
            } else {
                _addActorGateParents[gateID] = std::nullopt;
            }
        }

        SKSE::log::info(
            "OSTNET ADD-ACTOR GATE BEGIN gate={} connection={} actor={:08X} label=\"{}\" parentSession={} uiPaused=1",
            gateID,
            *connection,
            actor->GetFormID(),
            selectedLabel,
            parentSessionID);

        RE::DebugNotification("Waiting for consent");

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([this, gateID]() {
                StopPreflightAndInvite(gateID);
            });
        } else {
            StopPreflightAndInvite(gateID);
        }

        return static_cast<std::int32_t>(gateID);
    }

    std::int32_t CoopSessionManager::PollAddActorConsent(
        std::int32_t gateID)
    {
        if (gateID <= 0) {
            return -1;
        }

        const auto id = static_cast<std::uint64_t>(gateID);
        std::scoped_lock lock(_mutex);

        const auto gateIt = _ownerSessions.find(id);
        if (gateIt == _ownerSessions.end()) {
            return -1;
        }

        auto& gate = gateIt->second;
        const bool accepted =
            !gate.participants.empty() &&
            gate.accepted.size() == gate.participants.size();

        if (accepted) {
            std::optional<std::uint64_t> parent;
            if (const auto parentIt = _addActorGateParents.find(id);
                parentIt != _addActorGateParents.end()) {
                parent = parentIt->second;
                _addActorGateParents.erase(parentIt);
            }

            if (parent) {
                const auto ownerIt = _ownerSessions.find(*parent);
                if (ownerIt == _ownerSessions.end() || ownerIt->second.canceled) {
                    return -1;
                }

                ownerIt->second.participants.insert(
                    gate.participants.begin(), gate.participants.end());
                ownerIt->second.accepted.insert(
                    gate.accepted.begin(), gate.accepted.end());
                ownerIt->second.restarting = true;
                _approvedReplayArmed = *parent;

                SKSE::log::info(
                    "OSTNET ADD-ACTOR GATE ACCEPT gate={} parentSession={} participants={} action=resume-ui",
                    id,
                    *parent,
                    ownerIt->second.participants.size());
            } else {
                gate.canceled = false;
                gate.restarting = true;
                gate.active = false;
                gate.actorFormIDs.clear();
                _approvedReplayArmed = id;

                SKSE::log::info(
                    "OSTNET ADD-ACTOR GATE ACCEPT gate={} parentSession=none action=arm-next-thread-resume-ui",
                    id);
            }
            return 1;
        }

        if (gate.canceled) {
            _addActorGateParents.erase(id);
            SKSE::log::info(
                "OSTNET ADD-ACTOR GATE DECLINE gate={} action=reopen-add-actor",
                id);
            return -1;
        }

        return 0;
    }
}
