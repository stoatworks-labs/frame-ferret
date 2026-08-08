cmake_minimum_required(VERSION 3.21)

project(frame-ferret VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(FERRET_VERSION="${PROJECT_VERSION}")

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# ferret_core — everything with no I/O, no GPU and no SDK dependency.
#
# Kept as its own library for one reason: it is the part a cheap unit test can
# reach. WebLinked learned this the hard way — its OSC decoder sat inside the
# CEF-linked target where no test could get at it, and shipped a bug that
# dropped a quarter of all messages. Anything decision-shaped belongs here.
# ---------------------------------------------------------------------------
add_library(ferret_core STATIC
  src/core/dylib.cpp
  src/core/subprocess.cpp
  src/core/frame.cpp
  src/core/json.cpp
  src/core/pixel_format.cpp
  src/core/rational.cpp
  src/core/convert.cpp
  src/sources/test_pattern.cpp
  src/sinks/preview.cpp
  src/sinks/decklink.cpp
  src/sinks/syphon_stub.cpp
  src/app/main_loop.cpp
  src/app/config.cpp
  src/app/engine.cpp
  src/app/factory.cpp
  src/app/node.cpp
  src/app/router.cpp
  src/net/interfaces.cpp
  src/transports/ndi.cpp
  src/transports/omt.cpp
  src/transports/srt_socket.cpp
  src/transports/ffmpeg.cpp
  src/transports/srt.cpp
  src/transports/st2110_rtp.cpp
  src/transports/st2110.cpp
)
target_include_directories(ferret_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
  target_link_libraries(ferret_core PUBLIC iphlpapi ws2_32)
endif()

# ---------------------------------------------------------------------------
# Syphon (macOS only), plus the vendored Syphon server subset.
#
# BSD-3, same revision oxbow uses; third_party/syphon/README.md records what was
# taken and what was left. Those sources are ARC and expect Syphon's own prefix
# header, neither of which is true of anything else here, so both are set
# per-file rather than on the target.
# ---------------------------------------------------------------------------
if(APPLE)
  target_sources(ferret_core PRIVATE src/sinks/syphon.mm)
  set(FERRET_SYPHON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon/Syphon)
  file(GLOB FERRET_SYPHON_OBJC ${FERRET_SYPHON_DIR}/*.m)
  file(GLOB FERRET_SYPHON_C    ${FERRET_SYPHON_DIR}/*.c)
  target_sources(ferret_core PRIVATE ${FERRET_SYPHON_OBJC} ${FERRET_SYPHON_C})

  # -w because this is upstream code we do not modify, and it uses
  # kIOSurfaceIsGlobal — which is how Syphon shares a surface between processes
  # at all, and is deprecated with no replacement. Left as a warning it would be
  # noise in every build forever.
  #
  # ARC is Syphon's own setting and applies only to the Objective-C files;
  # SyphonDispatch.c is plain C, where the flag is rejected rather than merely
  # unnecessary.
  set_source_files_properties(${FERRET_SYPHON_OBJC} PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-include;${FERRET_SYPHON_DIR}/Syphon_Prefix.pch;-w")
  set_source_files_properties(${FERRET_SYPHON_C} PROPERTIES COMPILE_OPTIONS "-w")

  target_include_directories(ferret_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon)
  target_link_libraries(ferret_core PUBLIC
    "-framework Foundation" "-framework IOSurface" "-framework CoreFoundation"
    "-framework AppKit")
endif()

# ---------------------------------------------------------------------------
# DeckLink, optional.
#
# The Blackmagic SDK is not vendored — its licence is not ours to redistribute
# — so this is off unless -DDECKLINK_SDK_DIR is given. Without it the sink
# still exists and reports itself unavailable, so nothing else changes.
#
# The version check is load-bearing, not defensive. This fleet's machines carry
# SEVERAL SDK copies at different versions (a 10.11 set inside the NDI SDK's
# examples, a 12.2 set inside Unreal's BlackmagicMedia), and first-hit order
# picks whichever comes first. Below 11.0 there is no IDeckLinkProfileAttributes
# and the build fails a long way from the cause.
# ---------------------------------------------------------------------------
set(DECKLINK_SDK_DIR "" CACHE PATH "Blackmagic DeckLink SDK (enables SDI output)")
if(DECKLINK_SDK_DIR)
  if(APPLE)
    set(DECKLINK_PLATFORM Mac)
  elseif(WIN32)
    set(DECKLINK_PLATFORM Win)
  else()
    set(DECKLINK_PLATFORM Linux)
  endif()

  find_path(DECKLINK_INCLUDE_DIR DeckLinkAPI.h
    HINTS "${DECKLINK_SDK_DIR}"
    PATH_SUFFIXES "${DECKLINK_PLATFORM}/include" "include/${DECKLINK_PLATFORM}"
                  "${DECKLINK_PLATFORM}" "Include/${DECKLINK_PLATFORM}" "include"
    NO_DEFAULT_PATH)
  if(NOT DECKLINK_INCLUDE_DIR)
    message(FATAL_ERROR "DECKLINK_SDK_DIR is set but DeckLinkAPI.h was not found under it")
  endif()

  if(EXISTS "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIVersion.h")
    file(STRINGS "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIVersion.h" DECKLINK_VERSION_LINE
         REGEX "BLACKMAGIC_DECKLINK_API_VERSION_STRING")
    string(REGEX MATCH "([0-9]+)\\.([0-9]+)" _dlm "${DECKLINK_VERSION_LINE}")
    if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 LESS 11)
      message(FATAL_ERROR
        "DeckLink SDK at ${DECKLINK_INCLUDE_DIR} is ${CMAKE_MATCH_1}.${CMAKE_MATCH_2}; "
        "11.0+ required (IDeckLinkProfileAttributes arrived in 11.0). "
        "There is very likely a newer SDK elsewhere on this machine.")
    endif()
    message(STATUS "Frame Ferret: DeckLink SDK ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} at ${DECKLINK_INCLUDE_DIR}")
  endif()

  find_file(DECKLINK_DISPATCH DeckLinkAPIDispatch.cpp
    HINTS "${DECKLINK_INCLUDE_DIR}" "${DECKLINK_SDK_DIR}"
    PATH_SUFFIXES "" "${DECKLINK_PLATFORM}/include" "include"
    NO_DEFAULT_PATH)
  if(NOT DECKLINK_DISPATCH)
    message(FATAL_ERROR "DeckLinkAPIDispatch.cpp not found next to DeckLinkAPI.h")
  endif()

  target_sources(ferret_core PRIVATE "${DECKLINK_DISPATCH}")
  target_include_directories(ferret_core PUBLIC "${DECKLINK_INCLUDE_DIR}")
  target_compile_definitions(ferret_core PUBLIC FERRET_HAVE_DECKLINK)
  if(APPLE)
    target_link_libraries(ferret_core PUBLIC "-framework CoreFoundation")
  endif()
endif()

# ---------------------------------------------------------------------------
# The application.
# ---------------------------------------------------------------------------
add_executable(frame-ferret
  src/app/main.cpp
  src/control/control_api.cpp
  src/control/http_server.cpp
  src/diag/diag.cpp
)
target_link_libraries(frame-ferret PRIVATE ferret_core)

if(APPLE)
  target_link_libraries(frame-ferret PRIVATE "-framework CoreFoundation")
elseif(WIN32)
  # dbghelp is for the vendored diag's crash backtrace (SymInitialize /
  # SymFromAddr). WebLinked hit exactly this and it is easy to miss because
  # nothing else in the tree references it — the only symptom is LNK2019 at the
  # final link of the executable, long after every object compiles cleanly.
  target_link_libraries(frame-ferret PRIVATE dbghelp)
elseif(UNIX)
  target_link_libraries(frame-ferret PRIVATE dl pthread)
endif()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
enable_testing()

foreach(t router rational pixel_format interfaces convert config engine web_assets ndi_abi omt_abi srt st2110)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE ferret_core)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
