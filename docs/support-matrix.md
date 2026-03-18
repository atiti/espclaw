# Support Matrix

This matrix describes the current intended public support level for ESPClaw.

## Boards

| Target | Status | Notes |
| --- | --- | --- |
| `esp32s3` | Stable primary target | Preferred full-agent board class with PSRAM. |
| AI Thinker `esp32cam` | Stable compatibility target | Camera-first profile. Tight RAM and DMA margins still matter. |
| `esp32c3` | Unsupported | Removed as a supported target because the full agent runtime is too constrained. |

## Operator Surfaces

| Surface | Status | Notes |
| --- | --- | --- |
| Admin web UI | Stable | Primary local operator UI. |
| UART console | Stable | Shared console executor with slash commands and LLM turns. |
| Simulator web UI | Stable | Good for host-side development and app/runtime iteration. |
| Telegram | Experimental | Works for bring-up and remote use, but still has tighter runtime margins than local surfaces. |

## Runtime Features

| Feature | Status | Notes |
| --- | --- | --- |
| Lua apps | Stable | Installable inline, from file/blob, or from URL. |
| Components | Stable | Shareable reusable Lua modules under `/workspace/components`. |
| Tasks | Stable | Periodic and named-event schedules. |
| Behaviors | Stable | Persisted task definitions with boot autostart. |
| Context chunking/search/select | Stable | Preferred path for large workspace docs. |
| OTA | Stable | Dual-slot OTA with boot confirmation. |
| Camera on `esp32cam` | Stable with caveats | Sensitive to DMA/internal memory pressure. |
| Web search/fetch | Experimental | Depends on configured external adapter. |

## Provider And Channel Dependencies

| Capability | Requirement |
| --- | --- |
| LLM chat/tool use | Configured provider auth |
| Telegram bot | Runtime token in NVS |
| Web search/fetch | Configured proxy adapter |
| OTA | Working local network path to admin API |

## Community Contribution Guidance

Public contributions are most useful in these areas:

- reusable components
- board profiles and board config docs
- simulator and host-test coverage
- admin UI polish
- docs and examples

Higher-risk areas that need extra care:

- OTA
- auth storage
- Telegram runtime paths
- provider transport and streaming
- `esp32cam` camera and SDSPI concurrency
