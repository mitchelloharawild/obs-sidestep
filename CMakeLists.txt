cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(SIDESTEP_BUILD_PLUGIN "Build the OBS plugin (needs libobs)" ON)
option(SIDESTEP_BUILD_TESTS "Build unit tests for the core library" ON)

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OBS Studio filter that moves your camera out of the way of on-screen content")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mitchelloharawild/obs-sidestep")

include(compilerconfig)
include(defaults)
include(helpers)

# Core: image analysis and placement, no libobs.
add_library(
  sidestep-core
  STATIC
  src/core/busy.c
  src/core/place.c
  src/core/strategy.c
  src/core/strat_any.c
  src/core/strat_bgimage.c
  src/core/strat_solid.c
  src/core/strat_activity.c
  src/core/mod_template.c
)
target_include_directories(sidestep-core PUBLIC src/core)
set_target_properties(sidestep-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_link_libraries(sidestep-core PUBLIC m)
endif()

if(SIDESTEP_BUILD_PLUGIN)
  add_library(${CMAKE_PROJECT_NAME} MODULE)

  find_package(libobs REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE sidestep-core OBS::libobs)

  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/plugin-main.c src/sidestep-filter.c src/sidestep-capture.c src/sidestep-overlay.c
  )

  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
endif()

if(SIDESTEP_BUILD_TESTS)
  enable_testing()
  foreach(t test_busy test_place test_mod_template test_strat_activity test_strat_solid)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sidestep-core)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  # Not tests: a benchmark and the fixture evaluation used for tuning
  # (run from the source dir: eval_fixtures tests/fixtures/user2026).
  foreach(t bench_busy eval_fixtures)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sidestep-core)
  endforeach()
endif()
