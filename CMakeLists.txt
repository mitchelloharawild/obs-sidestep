cmake_minimum_required(VERSION 3.16)

project(obs-sidestep VERSION 0.2.0 LANGUAGES C)

option(SIDESTEP_BUILD_PLUGIN "Build the OBS plugin (needs libobs)" ON)
option(SIDESTEP_BUILD_TESTS "Build unit tests for the core library" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Core: image analysis and placement, no libobs.
add_library(sidestep-core STATIC src/core/busy.c src/core/place.c)
target_include_directories(sidestep-core PUBLIC src/core)
set_target_properties(sidestep-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_link_libraries(sidestep-core PUBLIC m)
endif()

if(SIDESTEP_BUILD_PLUGIN)
  find_package(libobs REQUIRED)

  add_library(obs-sidestep MODULE
    src/plugin-main.c
    src/sidestep-filter.c
    src/sidestep-capture.c
    src/sidestep-overlay.c)
  target_link_libraries(obs-sidestep PRIVATE sidestep-core OBS::libobs)
  set_target_properties(obs-sidestep PROPERTIES PREFIX "")
  target_compile_definitions(obs-sidestep PRIVATE
    PLUGIN_VERSION="${PROJECT_VERSION}")

  include(GNUInstallDirs)
  if(UNIX AND NOT APPLE)
    install(TARGETS obs-sidestep
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
    install(DIRECTORY data/
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/obs-sidestep)
  else()
    install(TARGETS obs-sidestep LIBRARY DESTINATION obs-plugins/64bit)
    install(DIRECTORY data/ DESTINATION data/obs-plugins/obs-sidestep)
  endif()
endif()

if(SIDESTEP_BUILD_TESTS)
  enable_testing()
  foreach(t test_busy test_place)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sidestep-core)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  add_executable(bench_busy tests/bench_busy.c)
  target_link_libraries(bench_busy PRIVATE sidestep-core)
endif()
