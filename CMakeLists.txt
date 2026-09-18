cmake_minimum_required(VERSION 3.16)
project(pulseprobe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PULSEPROBE_BUILD_TESTS "Build the test suite" ON)
option(PULSEPROBE_WERROR "Treat warnings as errors" OFF)
set(PULSEPROBE_SANITIZER "" CACHE STRING "Sanitizer to build with: thread, address, or empty")

find_package(Threads REQUIRED)

add_library(pulseprobe_core STATIC
  src/util.cpp
  src/procfs.cpp
  src/cgroup.cpp
  src/jobmap.cpp
  src/registry.cpp
  src/collectors.cpp
  src/nvml.cpp
  src/exposition.cpp
  src/config.cpp
)

target_include_directories(pulseprobe_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_options(pulseprobe_core PRIVATE -Wall -Wextra -Wpedantic)
if(PULSEPROBE_WERROR)
  target_compile_options(pulseprobe_core PRIVATE -Werror)
endif()

# NVML is dlopen'd at runtime, so the only link-time requirement is libdl.
# There is deliberately no CUDA toolkit dependency: the same binary has to run
# on nodes that have no NVIDIA software installed.
target_link_libraries(pulseprobe_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(PULSEPROBE_SANITIZER)
  target_compile_options(pulseprobe_core PUBLIC -fsanitize=${PULSEPROBE_SANITIZER} -g -fno-omit-frame-pointer)
  target_link_options(pulseprobe_core PUBLIC -fsanitize=${PULSEPROBE_SANITIZER})
endif()

add_executable(pulseprobe src/main.cpp)
target_link_libraries(pulseprobe PRIVATE pulseprobe_core)
# cpp-httplib is vendored as a single header: the agent needs an HTTP server,
# not a hand-written one, and vendoring keeps the build hermetic.
target_include_directories(pulseprobe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_options(pulseprobe PRIVATE -Wall -Wextra)

install(TARGETS pulseprobe RUNTIME DESTINATION bin)

if(PULSEPROBE_BUILD_TESTS)
  enable_testing()

  set(PULSEPROBE_UNIT_TESTS
    test_procfs
    test_cgroup
    test_jobmap
    test_exposition
    test_registry
  )

  foreach(test_name IN LISTS PULSEPROBE_UNIT_TESTS)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE pulseprobe_core)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${test_name} PRIVATE
      PULSEPROBE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # End-to-end: start the real binary, wait for readiness, scrape it.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME integration_http
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
              $<TARGET_FILE:pulseprobe>)
    set_tests_properties(integration_http PROPERTIES TIMEOUT 60)
  else()
    message(STATUS "Python3 not found; skipping the HTTP integration test")
  endif()
endif()
