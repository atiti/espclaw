# ESPClaw Architecture

## Overview

ESPClaw is split into two layers:

- firmware integration, which depends on ESP-IDF services such as Wi-Fi, camera, SD, NVS, HTTP, and OTA
- runtime contracts, which are plain C modules that define the product surface and can be tested on the host

This repo implements the second layer first so the project can stabilize public interfaces before the device-specific code grows around them.

## Core Modules

### Board Profiles

Board profiles capture platform-level constraints and defaults:

- camera support
- BLE provisioning support
- preferred provisioning transport
- OTA slot count
- soft concurrency budget
- default capture sizing

The current profiles are:

- `esp32s3`: primary feature profile
- `esp32cam`: constrained compatibility profile

### Workspace Store

The workspace store defines the SD card file layout and the bootstrap contents for the control files:

- `AGENTS.md`
- `IDENTITY.md`
- `USER.md`
- `HEARTBEAT.md`
- `memory/MEMORY.md`

Session transcripts, media captures, and non-secret config also live under the workspace root.

The workspace module now includes filesystem bootstrap logic so firmware can create the expected SD structure without re-implementing path handling in the HTTP or agent layers.

### Providers

The provider layer is capability-driven. Each provider type declares:

- whether it supports image inputs
- whether it supports tool calls
- whether it supports streaming

The initial provider types are:

- `openai_compat`
- `anthropic_messages`
- `openai_codex`

`openai_codex` is the ChatGPT/Codex-account path modeled after PicoClaw/OpenClaw. It uses ChatGPT backend auth material instead of a normal API key and expects an account id alongside the access token.

The core runtime now includes provider-specific request renderers plus auth-profile storage so the eventual HTTP layer can focus on transport, retries, refresh, and streaming rather than rebuilding prompt JSON or secret handling on each platform binding.

### Channels

The channel layer is transport-agnostic. Each channel descriptor declares:

- whether it requires polling or inbound webhooks
- whether it supports media uploads
- whether it should be exposed in v1

Only Telegram is v1-enabled today.

### Tools

Tools are described with a safety level so the agent loop can enforce confirmation before executing mutating operations. The catalog distinguishes informational tools from mutating tools without binding the implementation to a specific prompt format.

### OTA

OTA state is modeled separately from the transport. This allows the firmware to reuse the same state machine whether updates are loaded from a local upload or a remote manifest later.

### Admin UI

The admin UI is embedded into firmware as a static asset. It is intentionally lightweight and centered on:

- onboarding status
- provider/channel configuration
- workspace file editing
- logs and storage health
- OTA operations

The corresponding admin API should emit small JSON payloads that can be served directly by `esp_http_server` handlers without requiring a client-heavy frontend architecture.

### Sessions

Session history is stored as append-only JSONL under `/workspace/sessions`. The runtime contract currently covers:

- session identifier validation
- append of individual role/content events
- reading the raw session transcript for diagnostics or future summarization

### Agent Loop

The agent loop now implements an iterative Responses-style orchestration path:

- load workspace control files into the system prompt
- load recent session history from JSONL
- advertise the enabled tool schemas
- call the configured provider
- execute returned tool calls locally
- continue the same run with `previous_response_id`
- stop when the provider returns normal assistant output or the iteration cap is reached

This is the current shared path used by the admin chat panel, simulator chat API, and Telegram free-text handling.

### Apps

ESPClaw apps are small Lua bundles stored under `/workspace/apps/<app_id>/`. The runtime contract covers:

- manifest parsing and validation
- app scaffolding on the SD workspace
- trigger matching
- installed app discovery
- Lua host bindings for a permissioned API surface
- persistent Lua VM instances for scheduled control loops

The app runtime now includes a hardware-native bridge instead of routing tight loops back through generic tool calls. The current bindings cover:

- GPIO read/write
- LEDC-backed PWM setup, write, stop, and state inspection
- servo and ESC pulse helpers on top of LEDC
- raw and calibrated ADC reads through `adc_oneshot`
- I2C bus setup, scan, register reads, and register writes
- typed `TMP102` and `MPU6050` helpers for common control sensors
- RMT-backed PPM output framing
- buzzer tone generation built on PWM
- native PID step computation
- native differential-drive and quad-X mixer helpers
- complementary attitude estimation for IMU fusion
- monotonic tick and sleep helpers

That keeps the data path short enough for on-device control loops while preserving manifest-based capability gating.

### Control Loops

Control loops sit between the app runtime and the hardware bridge. Each running loop owns:

- a stable loop id
- a persistent Lua VM instance
- an app id and trigger name
- a fixed period in milliseconds
- an optional iteration cap
- last-result and status metadata for inspection through the admin API

On the host simulator they run in detached pthreads. On firmware they run in FreeRTOS tasks. The loop worker reuses the same Lua globals across iterations, which is important for PID integrators, filter state, and device-local controller calibration.

## Current Firmware Bring-Up

The firmware now has an initial runtime path that performs:

1. NVS initialization
2. network stack initialization
3. Wi-Fi provisioning manager startup
4. SD-backed workspace mount/bootstrap when available
5. Telegram polling in a background FreeRTOS task
6. Lua app boot-trigger execution from the SD workspace

The Telegram runtime now has two layers:

- command handling for status and app-management commands
- iterative LLM execution for free-text chat messages, using the same run loop as the admin API

That gives the project a real end-to-end embedded message loop with multi-tool runs, while still keeping camera upload, richer provider refresh flows, and additional tool implementations as follow-on work.

## Planned Firmware Integration

The next implementation steps should bind these contracts to ESP-IDF services:

1. mount SD and bootstrap the workspace on first boot
2. persist secrets and network configuration in NVS
3. expose HTTP routes and WebSocket streams with `esp_http_server`
4. implement Telegram long polling over the ESP HTTP client
5. integrate `esp32-camera`
6. wire dual-slot OTA with rollback validation

## Extension Strategy

The repo should stay modular by keeping new functionality behind the same contracts:

- add board profiles rather than board-specific conditionals in the agent loop
- add provider adapters instead of hardcoding model vendors
- add channels by registering descriptors and runtime handlers
- add tools through the tool catalog and a consistent confirmation policy
