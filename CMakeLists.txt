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
  src/core/frame.cpp
  src/core/json.cpp
  src/core/pixel_format.cpp
  src/core/rational.cpp
  src/core/convert.cpp
  src/sources/test_pattern.cpp
  src/sinks/preview.cpp
  src/app/config.cpp
  src/app/engine.cpp
  src/app/factory.cpp
  src/app/node.cpp
  src/app/router.cpp
  src/net/interfaces.cpp
)
target_include_directories(ferret_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
  target_link_libraries(ferret_core PUBLIC iphlpapi ws2_32)
endif()

# ---------------------------------------------------------------------------
# The application.
# ---------------------------------------------------------------------------
add_executable(frame-ferret
  src/app/main.cpp
  src/control/control_api.cpp
  src/control/http_server.cpp
  src/core/dylib.cpp
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

foreach(t router rational pixel_format interfaces convert config engine web_assets)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE ferret_core)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
