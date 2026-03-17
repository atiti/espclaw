# Next Phase Plan

ESPClaw is now at the point where the core runtime is credible on real `esp32cam` hardware:

- direct ChatGPT Pro / Codex runs work on-device
- OTA works over HTTP
- the SD-backed workspace now mounts successfully on the AI Thinker board

The next implementation batch should focus on proving the whole runtime surface on real hardware before adding more feature area.

## Priorities

1. Real hardware/tool bench
   - Status: completed on the AI Thinker `esp32cam`
   - Expand the host-side real-device bench to validate the live device in stages:
     - tool inventory
     - hardware inventory
     - workspace file presence
     - LLM exact-text reply
     - model-driven tool round
     - model-generated Lua app install/run
     - direct task/event runtime validation
   - Keep the bench board-agnostic where possible so the same suite can be reused for future S3 boards.

2. Full LLM hardware/task surface
   - Expose the broader hardware and task runtime surfaces to the model by default.
   - Keep `wifi` mutation and `ota` mutation out of autonomous tool access for now.
   - Validate every newly enabled tool through the real-device bench before treating it as supported.

3. Real camera capture on hardware
   - Replace the current host-only/simulator camera path with a real `esp32-camera` backend for `esp32cam`.
   - Persist captured JPEGs to the SD-backed workspace.
   - Add board-aware failure reporting when the camera is unavailable or misconfigured.

4. Vision bench
   - Add a real image-to-LLM validation stage:
     - capture a JPEG
     - attach it to a live Codex run
     - validate that the assistant reasons about the captured image

## Current Execution Order

The immediate next work should happen in this order:

1. Enable and validate more hardware/task tools for the LLM.
2. Implement the real camera backend.
3. Add image-to-LLM verification on real hardware.

## Latest Validation Snapshot

The current real-device bench is green on the AI Thinker `esp32cam` running the PSRAM-enabled OTA image:

- `preflight`
- `inventory`
- `hello`
- `tool_reasoning`
- `generate_echo_app`
- `task_event_runtime`

This proves the current baseline on hardware:

- direct ChatGPT Pro / Codex runs
- SD-backed workspace access
- OTA updates
- model-driven tool reasoning
- model-generated Lua app installation and execution
- event-driven task runtime
