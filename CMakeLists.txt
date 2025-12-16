cmake_minimum_required(VERSION 3.25)
project(flowcontrol VERSION 0.1.0 LANGUAGES CXX)

# ---------- Options ----------
option(FLOWCONTROL_BUILD_LIB "Build the flowcontrol library" ON)

# Consumer-friendly default:
# - If top-level: follow BUILD_SHARED_LIBS
# - If a subproject (FetchContent/add_subdirectory): default to STATIC unless user overrides
set(_flowcontrol_default_shared OFF)
if(PROJECT_IS_TOP_LEVEL AND BUILD_SHARED_LIBS)
  set(_flowcontrol_default_shared ON)
endif()
option(FLOWCONTROL_BUILD_SHARED "Build flowcontrol as a shared lib" ${_flowcontrol_default_shared})

option(FLOWCONTROL_BUILD_APP "Build CLI from src/app" ${PROJECT_IS_TOP_LEVEL})
option(FLOWCONTROL_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds" OFF)

include(CTest) # defines BUILD_TESTING option

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------- Dependencies ----------
find_package(TBB QUIET)

if(NOT TBB_FOUND)
  include(FetchContent)
  message(STATUS "TBB not found; fetching oneTBB (v2022.3.0)")

  FetchContent_Declare(
    onetbb
    GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
    GIT_TAG v2022.3.0
    GIT_SHALLOW TRUE
  )

  # keep the dependency lightweight
  set(TBB_TEST OFF CACHE BOOL "" FORCE)
  set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(onetbb)
endif()

# Ensure a consistent target name: TBB::tbb
if(NOT TARGET TBB::tbb)
  if(TARGET tbb)
    add_library(TBB::tbb ALIAS tbb)
  elseif(TARGET tbb_static)
    add_library(TBB::tbb ALIAS tbb_static)
  else()
    message(FATAL_ERROR "TBB found/built but no target (TBB::tbb, tbb, tbb_static) is available")
  endif()
endif()

find_package(Threads REQUIRED)

# ---------- Headers ----------
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

# ---------- Library sources (exclude src/app) ----------
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER LIB_SOURCES EXCLUDE REGEX "/src/app/")
list(FILTER LIB_SOURCES EXCLUDE REGEX "/src/test_runner\\.cpp$")

# ---------- Library ----------
if(FLOWCONTROL_BUILD_LIB)
  set(_libtype STATIC)
  if(FLOWCONTROL_BUILD_SHARED)
    set(_libtype SHARED)
  endif()

  add_library(flowcontrol ${_libtype} ${LIB_SOURCES} ${HEADER_FILES})
  add_library(flowcontrol::flowcontrol ALIAS flowcontrol)

  target_compile_features(flowcontrol PUBLIC cxx_std_17)

  target_include_directories(flowcontrol
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  target_link_libraries(flowcontrol
    PUBLIC
      TBB::tbb
      Threads::Threads
  )

  # Warnings & sanitizers (per-target, generator-safe)
  target_compile_options(flowcontrol
    PRIVATE
      $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion -Wformat=2>
      $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
      $<$<BOOL:${FLOWCONTROL_ENABLE_SANITIZERS}>:$<$<AND:$<CONFIG:Debug>,$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>>:-fsanitize=address,undefined -fno-omit-frame-pointer>>
  )

  target_link_options(flowcontrol
    PRIVATE
      $<$<BOOL:${FLOWCONTROL_ENABLE_SANITIZERS}>:$<$<AND:$<CONFIG:Debug>,$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>>:-fsanitize=address,undefined>>
  )

  set_target_properties(flowcontrol PROPERTIES
    OUTPUT_NAME flowcontrol
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )

  # ---------- Install (headers + library + package) ----------
  install(TARGETS flowcontrol
    EXPORT flowcontrolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT flowcontrolTargets
    NAMESPACE flowcontrol::
    FILE flowcontrolTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flowcontrol
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/flowcontrolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/flowcontrolConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/flowcontrolConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flowcontrol
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/flowcontrolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/flowcontrolConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flowcontrol
  )
endif()

# ---------- CLI app ----------
if(FLOWCONTROL_BUILD_APP)
  file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp"
  )

  list(LENGTH APP_SOURCES _app_n)
  if(_app_n GREATER 0)
    add_executable(flowcontrol_cli ${APP_SOURCES})
    target_link_libraries(flowcontrol_cli PRIVATE flowcontrol::flowcontrol)
    target_compile_features(flowcontrol_cli PRIVATE cxx_std_17)
    install(TARGETS flowcontrol_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

# ---------- Tests ----------
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  # Tests require the library to exist
  if(NOT FLOWCONTROL_BUILD_LIB)
    message(FATAL_ERROR "BUILD_TESTING=ON requires FLOWCONTROL_BUILD_LIB=ON")
  endif()

  add_executable(flowcontrol_tests src/test_runner.cpp)
  target_link_libraries(flowcontrol_tests PRIVATE flowcontrol::flowcontrol)
  target_compile_features(flowcontrol_tests PRIVATE cxx_std_17)

  add_test(NAME flowcontrol_tests COMMAND flowcontrol_tests)
endif()


message(STATUS "-- [FLOWCONTROL BUILDER]: Setup complete.")
