cmake_minimum_required(VERSION 3.24)

project(
    OStimTogether
    VERSION 0.20.1
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
        src/Config.cpp
        src/Config.h
        src/EquipmentLock.cpp
        src/EquipmentLock.h
        src/DefaultOutfitGuard.cpp
        src/DefaultOutfitGuard.h
        src/NetworkProbe.cpp
        src/NetworkProbe.h
        src/StrServerDiscovery.cpp
        src/StrServerDiscovery.h
        src/STRPMApi.h
        src/STRPMTransport.cpp
        src/STRPMTransport.h
        src/UdpTransport.cpp
        src/UdpTransport.h
        src/ActorResolver.cpp
        src/ActorResolver.h
        src/VisualKeepAlive.cpp
        src/VisualKeepAlive.h
        src/PapyrusAnimationBridge.cpp
        src/PapyrusAnimationBridge.h
        src/Input.cpp
        src/Input.h
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
)


target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 version bcrypt)