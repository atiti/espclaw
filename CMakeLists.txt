cmake_minimum_required(VERSION 3.21)

project(espclaw VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT ESP_PLATFORM)
  add_compile_definitions(
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
  )
endif()

set(ESPCLAW_HOST_LUA_ONELUA "${CMAKE_CURRENT_SOURCE_DIR}/firmware/managed_components/georgik__lua/lua/onelua.c")
if(EXISTS "${ESPCLAW_HOST_LUA_ONELUA}")
  add_library(espclaw_host_lua STATIC "${ESPCLAW_HOST_LUA_ONELUA}")
  target_compile_definitions(espclaw_host_lua PRIVATE MAKE_LIB)
  target_include_directories(
    espclaw_host_lua
    PUBLIC
      firmware/managed_components/georgik__lua/lua
      firmware/managed_components/georgik__lua/include
  )
endif()

enable_testing()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(
  espclaw_core
  firmware/components/espclaw_core/src/agent_loop.c
  firmware/components/espclaw_core/src/admin_ops.c
  firmware/components/espclaw_core/src/app_patterns.c
  firmware/components/espclaw_core/src/app_runtime.c
  firmware/components/espclaw_core/src/admin_api.c
  firmware/components/espclaw_core/src/admin_ui.c
  firmware/components/espclaw_core/src/auth_store.c
  firmware/components/espclaw_core/src/behavior_runtime.c
  firmware/components/espclaw_core/src/board_config.c
  firmware/components/espclaw_core/src/board_profile.c
  firmware/components/espclaw_core/src/channel.c
  firmware/components/espclaw_core/src/component_runtime.c
  firmware/components/espclaw_core/src/control_loop.c
  firmware/components/espclaw_core/src/context_runtime.c
  firmware/components/espclaw_core/src/config_render.c
  firmware/components/espclaw_core/src/console_chat.c
  firmware/components/espclaw_core/src/event_watch.c
  firmware/components/espclaw_core/src/hardware.c
  firmware/components/espclaw_core/src/lua_api_registry.c
  firmware/components/espclaw_core/src/ota_manager.c
  firmware/components/espclaw_core/src/ota_state.c
  firmware/components/espclaw_core/src/provisioning.c
  firmware/components/espclaw_core/src/provider.c
  firmware/components/espclaw_core/src/provider_request.c
  firmware/components/espclaw_core/src/session_store.c
  firmware/components/espclaw_core/src/storage.c
  firmware/components/espclaw_core/src/system_monitor.c
  firmware/components/espclaw_core/src/task_policy.c
  firmware/components/espclaw_core/src/telegram_protocol.c
  firmware/components/espclaw_core/src/task_runtime.c
  firmware/components/espclaw_core/src/tool_catalog.c
  firmware/components/espclaw_core/src/web_tools.c
  firmware/components/espclaw_core/src/workspace.c
  simulator/runtime_host_shim.c
)

target_include_directories(
  espclaw_core
  PUBLIC
    firmware/components/espclaw_core/include
)

if(TARGET espclaw_host_lua)
  target_compile_definitions(espclaw_core PUBLIC ESPCLAW_HOST_LUA=1)
  target_link_libraries(espclaw_core PUBLIC espclaw_host_lua m)
endif()

target_link_libraries(espclaw_core PUBLIC Threads::Threads)

add_executable(
  espclaw_tests
  tests/test_main.c
)

target_compile_definitions(
  espclaw_tests
  PRIVATE
    ESPCLAW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(espclaw_tests PRIVATE espclaw_core)

add_test(NAME espclaw_tests COMMAND $<TARGET_FILE:espclaw_tests>)
add_test(
  NAME espclaw_release_manifest_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_manifests.py
)

add_executable(
  espclaw_simulator
  simulator/espclaw_simulator.c
)

target_link_libraries(espclaw_simulator PRIVATE espclaw_core)

add_test(NAME espclaw_simulator_self_test COMMAND $<TARGET_FILE:espclaw_simulator> --self-test)
add_test(
  NAME espclaw_simulator_api
  COMMAND /bin/zsh ${CMAKE_CURRENT_SOURCE_DIR}/tests/simulator_api_test.sh $<TARGET_FILE:espclaw_simulator> 0
)
