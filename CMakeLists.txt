cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OST_VERSION)
string(STRIP "${OST_VERSION}" OST_VERSION)

project(
    OStimTogether
    VERSION ${OST_VERSION}
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CommonLibSSE REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/main.cpp
        src/AddonBridge.cpp
        src/AddonBridge.h
        src/AddonStateRepair.cpp
        src/AddonStateRepair.h
        src/AddActorConsentGate.cpp
        src/CoopSessionManager.cpp
        src/CoopSessionManager.h
        src/OCumStateSync.cpp
        src/OCumStateSync.h
        src/OCumOverlayRelink.cpp
        src/OCumOverlayRelink.h
        src/OCumOverlaySkinFix.cpp
        src/OCumOverlaySkinFix.h
        src/FurnitureOverlayReplay.cpp
        src/FurnitureOverlayReplay.h
        src/FreeSceneOverlayReplay.cpp
        src/FreeSceneOverlayReplay.h
        src/AccuratePenetrationAPI.h
        src/PPAIntegration.cpp
        src/PPAAbsoluteHooks.cpp
        src/PPAIntegration.h
        src/PapyrusConsentBridge.cpp
        src/PapyrusConsentBridge.h
        src/PreflightGuard.cpp
        src/PreflightGuard.h
        src/ProxyOnlyThreadCleanup.cpp
        src/ProxyOnlyThreadCleanup.h
        src/SafeMessageBox.cpp
        src/SafeMessageBox.h
        src/Config.cpp
        src/Config.h
        src/EquipmentLock.cpp
        src/EquipmentLock.h
        src/DefaultOutfitGuard.cpp
        src/DefaultOutfitGuard.h
        src/NetworkProbe.cpp
        src/NetworkProbe.h
        src/STRPMApi.h
        src/STRPMTransport.cpp
        src/STRPMTransport.h
        src/ActorResolverSTRPM.cpp
        src/ActorResolver.h
        src/VisualKeepAlive.cpp
        src/VisualKeepAlive.h
        src/PapyrusAnimationBridge.cpp
        src/PapyrusAnimationBridge.h
        src/SKEEOverlayRefresh.cpp
        src/SKEEOverlayRefresh.h
        src/MirrorUndressRepair.cpp
        src/MirrorUndressRepair.h
        src/Input.cpp
        src/Input.h
        src/FreeSceneAlignmentFix.cpp
        src/FreeSceneAlignmentFix.h
        src/FreeScenePhaseSync.cpp
        src/FreeScenePhaseSync.h
        src/ParticipantAlignmentSync.cpp
        src/ParticipantAlignmentSync.h
        src/FreeSceneSelfOriginLock.cpp
        src/FreeSceneSelfOriginLock.h
        src/FreeSceneRootSync.cpp
        src/FreeSceneRootSync.h
        src/SharedSceneControl.cpp
        src/SharedSceneControl.h
        src/OStimBridge.cpp
        src/OStimBridge.h
        src/RaceMenuOverlayBridge.cpp
        src/RaceMenuOverlayBridge.h
        src/SceneCenter.h
        src/OStimInternalGraphProbe.h
        src/OStimAPI/PluginInterface.h
        src/OStimAPI/ThreadActor.h
        src/OStimAPI/Thread.h
        src/OStimAPI/ThreadEventListener.h
        src/OStimAPI/ThreadInterface.h
        src/OStimAPI/InterfaceMap.h
        src/OStimAPI/InterfaceExchangeMessage.h
)

target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

target_compile_options(${PROJECT_NAME}
    PRIVATE
        /utf-8
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        OSTIM_TOGETHER_VERSION="${OST_VERSION}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE version)
