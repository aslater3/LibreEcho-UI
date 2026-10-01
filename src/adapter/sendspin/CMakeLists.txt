# SPDX-License-Identifier: MIT
#
# LibreEcho Sendspin adapter — opt-in companion build entry point.
#
# This companion (the RC1 SDK fixture plus the C++20 native engine-sink bridge,
# src/adapter/sendspin/engine_sink.{h,cpp}) is an explicit *opt-in* C++20 target.
# AGENTS.md documents the C++20 exception for the separately built Sendspin
# companion in `src/adapter/sendspin/` and its entry point `src/adapter/sendspind.cpp`,
# together with their dedicated tests and build targets; SDK background threads
# are confined to this companion, the C99 HTTP daemon must not link these
# dependencies, and the companion feeds the bounded shared audio sink and must
# not open a second ALSA/PCM device. The core LibreEcho web daemon and its tests
# stay C99 and do not link any of this; nothing here is added to the default
# `make` target or `tests/run_tests.sh`. The engine-sink bridge is exercised
# offline by the opt-in `make test-sendspin-sink` lane against the real compiled
# C99 engine.
#
# Configure (all dependency sources must be supplied, offline):
#
#   cmake -S ui/src/adapter/sendspin -B "$SENDSPIN_HOST_BUILD" \
#     -DSENDSPIN_SDK_DIR="$SCRATCH/src/sendspin-cpp-<rc1-sha>" \
#     -DSENDSPIN_ARDUINOJSON_DIR="$SCRATCH/deps/ArduinoJson-<sha>" \
#     -DSENDSPIN_MICRO_FLAC_DIR="$SCRATCH/deps/micro-flac-<sha>" \
#     -DSENDSPIN_IXWEBSOCKET_DIR="$SCRATCH/deps/IXWebSocket-<sha>" \
#     -DSENDSPIN_NOISE_C_DIR="$SCRATCH/deps/noise-c-<sha>"
#
#   cmake --build "$SENDSPIN_HOST_BUILD" --target sendspin_adapter_tests
#   ctest --test-dir "$SENDSPIN_HOST_BUILD" --output-on-failure
#
# The pinned SDK is consumed exactly as-is (add_subdirectory); FreeContent source
# overrides replace every network fetch, so a configure can never reach the network.
# Roles other than `player`, the Opus decoder, the SDK's examples and the SDK's own
# GoogleTest suite are all switched off.

cmake_minimum_required(VERSION 3.20)
project(libreecho_sendspin_adapter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Required pinned SDK and offline dependency sources -----------------------
foreach(required_var SENDSPIN_SDK_DIR SENDSPIN_ARDUINOJSON_DIR SENDSPIN_MICRO_FLAC_DIR
                      SENDSPIN_IXWEBSOCKET_DIR SENDSPIN_NOISE_C_DIR)
    if(NOT DEFINED ${required_var} OR NOT IS_DIRECTORY "${${required_var}}")
        message(FATAL_ERROR
            "${required_var} must point at a populated pinned source directory. "
            "This target is offline-only: it will not fetch dependencies at configure time.")
    endif()
endforeach()

# Map the friendly names onto CMake's FetchContent override hooks so the pinned
# SDK's own FetchContent_* calls resolve to the supplied immutable sources and
# never touch the network.
set(FETCHCONTENT_SOURCE_DIR_ARDUINOJSON "${SENDSPIN_ARDUINOJSON_DIR}" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_MICRO_FLAC  "${SENDSPIN_MICRO_FLAC_DIR}"  CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_IXWEBSOCKET "${SENDSPIN_IXWEBSOCKET_DIR}" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_NOISE_C     "${SENDSPIN_NOISE_C_DIR}"     CACHE PATH "" FORCE)
# Fail closed rather than silently fetching anything the overrides miss.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)

# ---- Pin the SDK's build shape ------------------------------------------------
set(SENDSPIN_ENABLE_PLAYER     ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_CONTROLLER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_METADATA   OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_COLOR      OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_ARTWORK    OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_OPUS       OFF CACHE BOOL "" FORCE)  # PCM-only spike
set(BUILD_EXAMPLES             OFF CACHE BOOL "" FORCE)
set(SENDSPIN_BUILD_TESTS       OFF CACHE BOOL "" FORCE)  # we ship our own fixture

add_subdirectory("${SENDSPIN_SDK_DIR}" "${CMAKE_BINARY_DIR}/_sendspin_sdk")

# ---- The fixture --------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(sendspin_adapter_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/test_sendspin_sdk.cpp")

# Reach the SDK's private headers (crypto/, platform/) and its own shared test
# fixtures (lifecycle_test_fixtures.h and the helpers it includes).
target_include_directories(sendspin_adapter_tests PRIVATE
    "${SENDSPIN_SDK_DIR}/tests"
    "${SENDSPIN_SDK_DIR}/src"
)

target_link_libraries(sendspin_adapter_tests PRIVATE sendspin Threads::Threads)
target_compile_options(sendspin_adapter_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME sendspin_sdk_adapter COMMAND sendspin_adapter_tests)

# ---- The runnable companion (real SDK) ----------------------------------------
# The C++20 companion entry point named by the AGENTS.md exception. It links the
# pinned SDK player role and the EngineSinkSession bridge; SENDPIN_COMPANION_SDK
# enables the --run path (the dependency-free Makefile lane builds the --check
# shape without the SDK).
add_executable(sendspind
    "${CMAKE_CURRENT_SOURCE_DIR}/../sendspind.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_sink.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_sink_session.cpp")
target_include_directories(sendspind PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sendspind PRIVATE sendspin Threads::Threads)
target_compile_definitions(sendspind PRIVATE SENDPIN_COMPANION_SDK=1)
target_compile_options(sendspind PRIVATE -Wall -Wextra)

# Real patched SDK + real C99 sink. The archive-backed Python entry point
# verifies the supplied tree before configuring this opt-in lane.
set(SENDSPIN_PLATFORM_DIR "" CACHE PATH "Platform checkout for real engine fixtures")
if(SENDSPIN_PLATFORM_DIR)
    set(AIRPLAY "${SENDSPIN_PLATFORM_DIR}/tools/mt8163-arm32/airplay")
    if(NOT EXISTS "${AIRPLAY}/audio_sink.c")
        message(FATAL_ERROR "Platform audio sink source is required")
    endif()
    add_library(sendspin_engine_bridge STATIC engine_sink.cpp engine_sink_session.cpp
                "${AIRPLAY}/audio_sink.c")
    target_include_directories(sendspin_engine_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${AIRPLAY}")
    target_compile_definitions(sendspin_engine_bridge PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(sendspin_engine_bridge PUBLIC Threads::Threads)
    foreach(pair "bridge;test_sendspin_bridge_sdk.cpp" "lifecycle;test_sendspin_sdk_lifecycle.cpp"
                 "late_credit;test_sendspin_late_credit.cpp")
        list(GET pair 0 name)
        list(GET pair 1 source)
        add_executable(sendspin_${name}_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/${source}")
        target_include_directories(sendspin_${name}_tests PRIVATE
            "${SENDSPIN_SDK_DIR}/tests" "${SENDSPIN_SDK_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests")
        target_link_libraries(sendspin_${name}_tests PRIVATE sendspin_engine_bridge sendspin Threads::Threads)
        target_compile_options(sendspin_${name}_tests PRIVATE -Wall -Wextra)
        add_test(NAME sendspin_${name} COMMAND sendspin_${name}_tests)
        set_tests_properties(sendspin_${name} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(sendspin_engine_sdk_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/test_sendspin_engine_sdk.cpp")
    target_include_directories(sendspin_engine_sdk_tests PRIVATE
        "${SENDSPIN_SDK_DIR}/tests" "${SENDSPIN_SDK_DIR}/src")
    target_link_libraries(sendspin_engine_sdk_tests PRIVATE sendspin_engine_bridge sendspin Threads::Threads)
    target_compile_options(sendspin_engine_sdk_tests PRIVATE -Wall -Wextra)
    # Executed by test_sendspin_sdk_host.py with the production-loop fixture.
endif()
