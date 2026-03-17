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
   - Status: completed on the AI Thinker `esp32cam`
   - Replace the current host-only/simulator camera path with a real `esp32-camera` backend for `esp32cam`.
   - Persist captured JPEGs to the SD-backed workspace.
   - Add board-aware failure reporting when the camera is unavailable or misconfigured.

4. Vision bench
   - Status: completed on the AI Thinker `esp32cam`
   - Add a real image-to-LLM validation stage:
     - capture a JPEG
     - attach it to a live Codex run
     - validate that the assistant reasons about the captured image

## Current Execution Order

The immediate next work should happen in this order:

1. Enable and validate more hardware/task tools for the LLM.
2. Expand the staged real-device bench to cover those hardware surfaces explicitly.
3. Keep the camera/vision path green while new hardware tools are added.
4. Close the current tool-catalog vs. tool-executor gap the new `tool_matrix_full` bench now exposes on the real AI Thinker board.

## Latest Validation Snapshot

The current real-device bench is green on the AI Thinker `esp32cam` running the PSRAM-enabled OTA image:

- `preflight`
- `inventory`
- `hello`
- `tool_reasoning`
- `generate_echo_app`
- `task_event_runtime`
- `vision`

This proves the current baseline on hardware:

- direct ChatGPT Pro / Codex runs
- SD-backed workspace access
- OTA updates
- model-driven tool reasoning
- model-generated Lua app installation and execution
- event-driven task runtime
- real camera capture plus image-to-LLM reasoning

## Current Bench Findings

- `tool_matrix_full` is now implemented and running on the real AI Thinker board.
- The executor surface has been expanded to cover the missing hardware / filesystem / network / compute tools, and the matrix now probes them mostly one-by-one so real tool compliance is measurable instead of being masked by grouped prompts.
- The remaining matrix work is to finish a full real-device run and tighten any tool-specific prompts that still let the model stop after a partial success.
- `large_lua_app` now reliably reaches a real `app.install` call on hardware. The current blocker is the generated Lua contract on `app.run`, where the model tends to return module-style `M.manual(...)` code instead of the runtime's supported `on_manual(...)`, `on_event(...)`, or `handle(...)` entrypoints.
- The next fix should either teach the bench prompt to require `handle(trigger, payload)` explicitly or extend the Lua runtime to accept module-return entrypoints for model-generated apps.
