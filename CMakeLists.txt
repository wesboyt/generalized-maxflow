cmake_minimum_required(VERSION 3.18)

project(generalized-maxflow
  VERSION 0.1.0
  DESCRIPTION "Generalized maximum flow: maximum flow with per-edge gain multipliers"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Link-time optimisation for the optimised configurations, where supported. The
# solver is one translation unit calling into another, so cross-module inlining
# of the inner loops is worth having.
include(CheckIPOSupported)
check_ipo_supported(RESULT GMF_IPO_SUPPORTED OUTPUT GMF_IPO_MESSAGE)
if(GMF_IPO_SUPPORTED)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

option(GMF_BUILD_CLI "Build the gmaxflow command line tool" ON)
option(GMF_BUILD_TESTS "Build the test suite" ON)
option(GMF_BUILD_BENCH "Build the benchmark harness" ON)
if(DEFINED SKBUILD)
  set(GMF_PYTHON_DEFAULT ON)
else()
  set(GMF_PYTHON_DEFAULT OFF)
endif()
option(GMF_BUILD_PYTHON "Build the CPython extension module" ${GMF_PYTHON_DEFAULT})
option(GMF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Emscripten compiles with exceptions disabled by default, which turns a throw
# into a module-level abort. The JSON front door reports a malformed graph by
# catching the exception validation raises, so without this the solver aborts
# instead of returning {"status": "invalid_input"} -- which is what the Node
# smoke test caught. These must be set before the targets they apply to.
if(EMSCRIPTEN)
  add_compile_options(-fexceptions)
  add_link_options(-fexceptions)
endif()

add_library(gmf_core STATIC
  src/json.cpp
  src/solver.cpp
  src/api.cpp)
target_include_directories(gmf_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

if(MSVC)
  target_compile_options(gmf_core PRIVATE /W4 /permissive-)
  if(GMF_WARNINGS_AS_ERRORS)
    target_compile_options(gmf_core PRIVATE /WX)
  endif()
else()
  target_compile_options(gmf_core PRIVATE -Wall -Wextra -Wpedantic)
  if(GMF_WARNINGS_AS_ERRORS)
    target_compile_options(gmf_core PRIVATE -Werror)
  endif()
endif()

if(GMF_BUILD_CLI AND NOT EMSCRIPTEN)
  add_executable(gmaxflow src/cli_main.cpp)
  target_link_libraries(gmaxflow PRIVATE gmf_core)
endif()

if(GMF_BUILD_TESTS)
  enable_testing()
  add_executable(gmf_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_solver.cpp
    tests/test_api.cpp
    tests/test_regression.cpp)
  target_link_libraries(gmf_tests PRIVATE gmf_core)
  target_include_directories(gmf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME gmf_tests COMMAND gmf_tests)
endif()

if(GMF_BUILD_BENCH AND NOT EMSCRIPTEN)
  add_executable(gmf_bench tools/bench.cpp)
  target_link_libraries(gmf_bench PRIVATE gmf_core)
endif()

if(GMF_BUILD_PYTHON)
  find_package(Python 3.8 REQUIRED COMPONENTS Interpreter Development.Module)
  python_add_library(_gmaxflow MODULE bindings/python/module.cpp WITH_SOABI)
  target_link_libraries(_gmaxflow PRIVATE gmf_core)
  install(TARGETS _gmaxflow DESTINATION gmaxflow)
endif()

if(EMSCRIPTEN)
  add_executable(gmaxflow_wasm bindings/wasm/wasm_bindings.cpp)
  target_link_libraries(gmaxflow_wasm PRIVATE gmf_core)
  set_target_properties(gmaxflow_wasm PROPERTIES OUTPUT_NAME "gmaxflow")
  target_link_options(gmaxflow_wasm PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createGmaxflow"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXPORTED_FUNCTIONS=['_gmf_solve_json','_gmf_free','_gmf_version','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    "-sENVIRONMENT=web,worker,node"
    "--no-entry")
endif()

# A Python wheel holds only the extension module; the library and its headers
# are for an ordinary install.
if(NOT SKBUILD)
  include(GNUInstallDirs)
  install(TARGETS gmf_core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(GMF_BUILD_CLI AND NOT EMSCRIPTEN)
    install(TARGETS gmaxflow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
