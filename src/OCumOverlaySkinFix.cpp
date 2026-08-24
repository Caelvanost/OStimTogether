#include "PCH.h"
#include "OCumOverlaySkinFix.h"

#include "RaceMenuOverlayBridge.h"
#include "STRPMTransport.h"
#include "OStimAPI/InterfaceExchangeMessage.h"
#include "OStimAPI/Thread.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kMarker = "CumOverlays";
        constexpr std::string_view kChannel = "OCum";
        constexpr std::string_view kBodyOverlayPrefix = "Body [Ovl";
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr std::uint32_t kMinBodyVertices = 5000;
        constexpr std::uint32_t kMinBodyMatrices = 30;

        std::string LowerCopy(std::string_view value)
        {
            std::string out(value);
            std::transform(
                out.begin(),
                out.end(),
                out.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            return out;
        }

        bool IsRejectedBodyCandidateName(std::string_view lowerName)
        {
            if (lowerName.find("[ovl") != std::string_view::npos ||
                lowerName.find("[sovl") != std::string_view::npos) {
                return true;
            }

            static constexpr std::array<std::string_view, 10> kReject = {
                "hair",
                "head",
                "face",
                "eye",
                "brow",
                "lash",
                "hand",
                "feet",
                "foot",
                "tongue"
            };

            return std::ranges::any_of(
                kReject,
                [&](std::string_view token) {
                    return lowerName.find(token) != std::string_view::npos;
                });
        }
    }

    OCumOverlaySkinFix& OCumOverlaySkinFix::GetSingleton()
    {
        static OCumOverlaySkinFix singleton;
        return singleton;
    }

    void OCumOverlaySkinFix::StartListener::listen(OStim::Thread* thread)
    {
        OCumOverlaySkinFix::GetSingleton().HandleStart(thread);
    }

    void OCumOverlaySkinFix::StopListener::listen(OStim::Thread* thread)
    {
        OCumOverlaySkinFix::GetSingleton().HandleStop(thread);
    }

    bool OCumOverlaySkinFix::Initialize()
    {
        if (_initialized.exchange(true)) {
            return _threads != nullptr;
        }

        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || !data->LookupModByName("OCum.esp")) {
            SKSE::log::info(
                "OSTNET OCUM DIRECT SKIN disabled reason=OCum-not-installed");
            return false;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
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
                "OSTNET OCUM DIRECT SKIN unavailable reason=OStim-interface-exchange");
            return false;
        }

        _threads = static_cast<OStim::ThreadInterface*>(
            exchange.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!_threads) {
            SKSE::log::warn(
                "OSTNET OCUM DIRECT SKIN unavailable reason=Threads-interface");
            return false;
        }

        _threads->registerThreadStartListener(&_startListener);
        _threads->registerThreadStopListener(&_stopListener);

        SKSE::log::info(
            "OSTNET OCUM DIRECT SKIN READY pollMs={} source=minVertices{}+minMatrices{} method=racemenu-style-vertexDesc+clonedSkin scope=free+furniture",
            kPollInterval.count(),
            kMinBodyVertices,
            kMinBodyMatrices);
        return true;
    }

    void OCumOverlaySkinFix::HandleStart(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();
        _activeThreads.insert(threadID);

        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                const auto actorID = actor->GetFormID();
                _overlaySignatures[actorID] = "0|";
                _lastSourceSkins.erase(actorID);
            }
        }

        SKSE::log::info(
            "OSTNET OCUM DIRECT SKIN START thread={} actors={} baseline=empty",
            threadID,
            thread->getActorCount());
    }

    void OCumOverlaySkinFix::HandleStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        _activeThreads.erase(thread->getThreadID());
        for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
            auto* ta = thread->getActor(i);
            auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (actor) {
                const auto actorID = actor->GetFormID();
                _overlaySignatures.erase(actorID);
                _lastSourceSkins.erase(actorID);
            }
        }
    }

    std::string OCumOverlaySkinFix::BuildSignature(
        const std::vector<std::string>& chunks)
    {
        std::string signature = fmt::format("{}|", chunks.size());
        for (const auto& chunk : chunks) {
            signature += fmt::format("{}:{}|", chunk.size(), chunk);
        }
        return signature;
    }

    OCumOverlaySkinFix::BodySource
    OCumOverlaySkinFix::FindBestBodySource(RE::Actor* actor)
    {
        BodySource best{};
        if (!actor) {
            return best;
        }

        auto* root = actor->Get3D();
        if (!root) {
            return best;
        }

        std::function<void(RE::NiAVObject*)> visit;
        visit = [&](RE::NiAVObject* object) {
            if (!object) {
                return;
            }

            if (auto* geometry = object->AsGeometry()) {
                const char* rawName = geometry->name.c_str();
                const std::string_view name = rawName ? rawName : "";
                const auto lowerName = LowerCopy(name);

                if (!IsRejectedBodyCandidateName(lowerName)) {
                    auto& runtime = geometry->GetGeometryRuntimeData();
                    auto* skin = runtime.skinInstance.get();
                    auto* partition = skin ? skin->skinPartition.get() : nullptr;
                    const auto vertices = partition ? partition->vertexCount : 0;
                    const auto matrices = skin ? skin->numMatrices : 0;

                    if (skin && partition &&
                        vertices >= kMinBodyVertices &&
                        matrices >= kMinBodyMatrices) {
                        std::uint64_t score =
                            static_cast<std::uint64_t>(vertices) +
                            static_cast<std::uint64_t>(matrices) * 1000ULL;

                        if (lowerName.find("body") != std::string::npos ||
                            lowerName.find("3ba") != std::string::npos ||
                            lowerName.find("3bbb") != std::string::npos ||
                            lowerName.find("cbbe") != std::string::npos) {
                            score += 100000ULL;
                        }

                        if (!best.geometry || score > best.score) {
                            best.geometry = geometry;
                            best.vertices = vertices;
                            best.matrices = matrices;
                            best.score = score;
                            best.name = std::string(name);
                        }
                    }
                }
            }

            if (auto* node = object->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (child) {
                        visit(child.get());
                    }
                }
            }
        };

        visit(root);
        return best;
    }

    OCumOverlaySkinFix::RebindResult
    OCumOverlaySkinFix::RebindBodyOverlays(
        RE::Actor* actor,
        const BodySource& source)
    {
        RebindResult result{};
        if (!actor || !source.geometry) {
            return result;
        }

        auto* root = actor->Get3D();
        if (!root) {
            return result;
        }

        auto& sourceRuntime = source.geometry->GetGeometryRuntimeData();
        auto* sourceSkin = sourceRuntime.skinInstance.get();
        if (!sourceSkin) {
            return result;
        }

        std::function<void(RE::NiAVObject*)> visit;
        visit = [&](RE::NiAVObject* object) {
            if (!object) {
                return;
            }

            if (auto* geometry = object->AsGeometry()) {
                const char* rawName = geometry->name.c_str();
                const std::string_view name = rawName ? rawName : "";

                if (name.starts_with(kBodyOverlayPrefix)) {
                    ++result.found;

                    auto& runtime = geometry->GetGeometryRuntimeData();
                    auto* oldSkin = runtime.skinInstance.get();
                    auto* oldPartition = oldSkin ? oldSkin->skinPartition.get() : nullptr;
                    if (result.oldVertices == 0 && oldPartition) {
                        result.oldVertices = oldPartition->vertexCount;
                        result.oldMatrices = oldSkin ? oldSkin->numMatrices : 0;
                    }

                    // Match RaceMenu OverlayInterface::RelinkOverlay: always
                    // copy the current source vertex descriptor and CLONE the
                    // selected body's NiSkinInstance. Sharing the body's exact
                    // skin pointer (the 0.35.1 experiment) is not equivalent to
                    // RaceMenu's relink path and can leave the overlay geometry
                    // non-rendering even though texture/alpha/material are valid.
                    runtime.vertexDesc = sourceRuntime.vertexDesc;

                    RE::NiPointer<RE::NiObject> clonedObject;
                    sourceSkin->CreateDeepCopy(clonedObject);
                    auto* clonedSkin = clonedObject ?
                        static_cast<RE::NiSkinInstance*>(clonedObject.get()) :
                        nullptr;

                    if (clonedSkin) {
                        runtime.skinInstance.reset(clonedSkin);
                        geometry->UpdateWorldBound();
                        ++result.rebound;
                    } else {
                        ++result.cloneFailed;
                    }
                }
            }

            if (auto* node = object->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (child) {
                        visit(child.get());
                    }
                }
            }
        };

        visit(root);
        return result;
    }

    void OCumOverlaySkinFix::Tick()
    {
        if (!_threads || _activeThreads.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_nextPoll.time_since_epoch().count() != 0 && now < _nextPoll) {
            return;
        }
        _nextPoll = now + kPollInterval;

        std::vector<std::int32_t> staleThreads;
        auto& transport = STRPMTransport::GetSingleton();

        for (const auto threadID : _activeThreads) {
            auto* thread = _threads->getThread(threadID);
            if (!thread) {
                staleThreads.push_back(threadID);
                continue;
            }

            for (std::uint32_t i = 0; i < thread->getActorCount(); ++i) {
                auto* ta = thread->getActor(i);
                auto* actor = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
                if (!actor) {
                    continue;
                }

                const auto actorID = actor->GetFormID();
                const bool isPlayer = actor->IsPlayerRef();
                const bool isProxy =
                    !isPlayer && transport.ResolveConnection(actorID).has_value();
                if (!isPlayer && !isProxy) {
                    continue;
                }

                const auto chunks =
                    RaceMenuOverlayBridge::GetSingleton()
                        .CaptureMarkedOverlayChunks(
                            actor,
                            kMarker,
                            2200);
                const auto signature = BuildSignature(chunks);
                auto [signatureIt, inserted] =
                    _overlaySignatures.try_emplace(actorID, "0|");
                const bool signatureChanged = signatureIt->second != signature;

                if (chunks.empty()) {
                    if (signatureChanged) {
                        signatureIt->second = signature;
                    }
                    _lastSourceSkins.erase(actorID);
                    continue;
                }

                const auto source = FindBestBodySource(actor);
                if (!source.geometry) {
                    // IMPORTANT: do not consume the new overlay signature here.
                    // OStim can expose no suitable body for a few frames while
                    // changing scene/furniture. Keeping the old signature makes
                    // the same snapshot retry until a valid body exists.
                    if (signatureChanged) {
                        SKSE::log::warn(
                            "OSTNET OCUM DIRECT SKIN no-source thread={} actor={:08X} player={} proxy={} chunks={} minVertices={} minMatrices={} retry=1",
                            threadID,
                            actorID,
                            isPlayer ? 1 : 0,
                            isProxy ? 1 : 0,
                            chunks.size(),
                            kMinBodyVertices,
                            kMinBodyMatrices);
                    }
                    continue;
                }

                auto& sourceRuntime = source.geometry->GetGeometryRuntimeData();
                auto* sourceSkin = sourceRuntime.skinInstance.get();
                if (!sourceSkin) {
                    continue;
                }

                auto [sourceIt, sourceInserted] =
                    _lastSourceSkins.try_emplace(actorID, nullptr);
                const bool sourceChanged = sourceIt->second != sourceSkin;

                // A new cum snapshot must always be relinked, even when the
                // source skin pointer did not change. A furniture/body settle
                // can also change the source skin without changing the overlay
                // snapshot, so sourceChanged independently triggers a relink.
                if (!signatureChanged && !sourceChanged) {
                    continue;
                }

                const auto rebound = RebindBodyOverlays(actor, source);
                if (rebound.rebound == 0) {
                    SKSE::log::warn(
                        "OSTNET OCUM DIRECT SKIN clone-failed thread={} actor={:08X} player={} proxy={} source=\"{}\" overlaysFound={} cloneFailed={} signatureChanged={} sourceChanged={}",
                        threadID,
                        actorID,
                        isPlayer ? 1 : 0,
                        isProxy ? 1 : 0,
                        source.name,
                        rebound.found,
                        rebound.cloneFailed,
                        signatureChanged ? 1 : 0,
                        sourceChanged ? 1 : 0);
                    continue;
                }

                // Reapply the existing OCum override state only after the
                // RaceMenu-style relink. These calls do not rediscover the body
                // source or replace the newly cloned overlay skin instances.
                if (isPlayer) {
                    RaceMenuOverlayBridge::GetSingleton()
                        .RefreshLocalOverlayGeometry(
                            actor,
                            kChannel,
                            chunks);
                } else {
                    for (const auto& chunk : chunks) {
                        RaceMenuOverlayBridge::GetSingleton()
                            .ApplyRemoteOverlayChunk(
                                actor,
                                kChannel,
                                chunk);
                    }
                }

                signatureIt->second = signature;
                sourceIt->second = sourceSkin;

                SKSE::log::info(
                    "OSTNET OCUM DIRECT SKIN REBIND thread={} actor={:08X} player={} proxy={} source=\"{}\" sourceVertices={} sourceMatrices={} overlaysFound={} rebound={} cloneFailed={} oldVertices={} oldMatrices={} chunks={} signatureChanged={} sourceChanged={} method=racemenu-style-cloned-skin",
                    threadID,
                    actorID,
                    isPlayer ? 1 : 0,
                    isProxy ? 1 : 0,
                    source.name,
                    source.vertices,
                    source.matrices,
                    rebound.found,
                    rebound.rebound,
                    rebound.cloneFailed,
                    rebound.oldVertices,
                    rebound.oldMatrices,
                    chunks.size(),
                    signatureChanged ? 1 : 0,
                    sourceChanged ? 1 : 0);
            }
        }

        for (const auto threadID : staleThreads) {
            _activeThreads.erase(threadID);
        }
    }
}
