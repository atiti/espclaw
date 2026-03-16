cmake_minimum_required(VERSION 3.21)

project(espclaw VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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

add_library(
  espclaw_core
  firmware/components/espclaw_core/src/agent_loop.c
  firmware/components/espclaw_core/src/admin_ops.c
  firmware/components/espclaw_core/src/app_runtime.c
  firmware/components/espclaw_core/src/admin_api.c
  firmware/components/espclaw_core/src/admin_ui.c
  firmware/components/espclaw_core/src/auth_store.c
  firmware/components/espclaw_core/src/board_config.c
  firmware/components/espclaw_core/src/board_profile.c
  firmware/components/espclaw_core/src/channel.c
  firmware/components/espclaw_core/src/control_loop.c
  firmware/components/espclaw_core/src/config_render.c
  firmware/components/espclaw_core/src/hardware.c
  firmware/components/espclaw_core/src/ota_state.c
  firmware/components/espclaw_core/src/provider.c
  firmware/components/espclaw_core/src/provider_request.c
  firmware/components/espclaw_core/src/session_store.c
  firmware/components/espclaw_core/src/storage.c
  firmware/components/espclaw_core/src/task_policy.c
  firmware/components/espclaw_core/src/telegram_protocol.c
  firmware/components/espclaw_core/src/tool_catalog.c
  firmware/components/espclaw_core/src/workspace.c
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

target_link_libraries(espclaw_tests PRIVATE espclaw_core)

add_test(NAME espclaw_tests COMMAND $<TARGET_FILE:espclaw_tests>)

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
