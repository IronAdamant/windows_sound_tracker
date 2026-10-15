cmake_minimum_required(VERSION 3.20)

# Static MSVC runtime so SoundTracker.exe runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(WindowsSoundTracker VERSION 1.2.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOUNDTRACKER_BUILD_TESTS "Build unit tests" ON)
option(SOUNDTRACKER_BUILD_TOOLS "Build the SoundBeacon test tool" ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows Sound Tracker only builds on Windows")
endif()

# Windows 10+ APIs, Unicode everywhere.
add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)

if(MSVC)
    add_compile_options(/EHsc /W4 /utf-8 /permissive-)
else()
    # MinGW (gcc or clang)
    add_compile_options(-Wall -Wextra)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Detection logic shared with the unit tests (no Windows audio dependencies).
add_library(soundtracker_detection STATIC src/SoundDetector.cpp include/SoundDetector.h)
target_include_directories(soundtracker_detection PUBLIC include)

set(SOURCES
    src/main_gui.cpp
    src/SoundTrackerGUI.cpp
    src/SoundTracker.cpp
    src/AlertOverlay.cpp
    src/AlertsDialog.cpp
    src/DebugLog.cpp
    src/DeviceWatcher.cpp
    src/Logger.cpp
    src/ProcessInfo.cpp
    src/Settings.cpp
)

set(HEADERS
    include/AlertOverlay.h
    include/AlertsDialog.h
    include/AudioEvent.h
    include/ComPtr.h
    include/CoreAudioCompat.h
    include/DebugLog.h
    include/DeviceWatcher.h
    include/Logger.h
    include/ProcessInfo.h
    include/Settings.h
    include/SoundTracker.h
    include/SoundTrackerGUI.h
    include/Version.h
)

add_executable(SoundTracker WIN32 ${SOURCES} ${HEADERS} SoundTracker.rc resource.h)
target_include_directories(SoundTracker PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SoundTracker PRIVATE
    soundtracker_detection
    ole32 oleaut32 uuid
    user32 gdi32 comctl32 comdlg32 shell32 shlwapi advapi32
    psapi cfgmgr32 dwmapi shcore
)
# The manifest is embedded through SoundTracker.rc on every toolchain.
set_source_files_properties(SoundTracker.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/SoundTracker.manifest;${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")

if(MSVC)
    target_link_options(SoundTracker PRIVATE /MANIFEST:NO)
else()
    target_link_options(SoundTracker PRIVATE -municode -static)
endif()

if(SOUNDTRACKER_BUILD_TOOLS)
    add_executable(SoundBeacon WIN32 tools/sound_beacon/SoundBeacon.cpp)
    target_link_libraries(SoundBeacon PRIVATE winmm user32 gdi32 shell32)
    if(NOT MSVC)
        target_link_options(SoundBeacon PRIVATE -municode -static)
    endif()
endif()

if(SOUNDTRACKER_BUILD_TESTS)
    enable_testing()
    add_executable(test_detection tests/test_detection.cpp)
    target_link_libraries(test_detection PRIVATE soundtracker_detection)
    if(NOT MSVC)
        target_link_options(test_detection PRIVATE -static)
    endif()
    add_test(NAME detection COMMAND test_detection)
endif()
