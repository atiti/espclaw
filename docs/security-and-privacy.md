# Security And Privacy

ESPClaw is a network-connected embedded runtime with local hardware control. Treat it as an operational device, not a toy demo.

## What ESPClaw Stores

Depending on configuration, ESPClaw may store:

- provider credentials
- Telegram bot token
- Wi-Fi credentials
- workspace files
- session history
- installed apps and components
- captured media

On real hardware, sensitive runtime configuration is stored in `NVS`. Workspace content typically lives on SD-backed storage.

## External Services

ESPClaw can talk to:

- OpenAI / ChatGPT / Codex provider endpoints
- Telegram Bot API
- configured web search/fetch proxy services

These services may receive:

- prompts
- selected workspace excerpts
- tool outputs
- captured images if a workflow explicitly sends them

## Recommended Deployment Posture

- Keep the admin UI on a trusted network.
- Rotate any token that was ever pasted into logs, chat, or screenshots.
- Avoid storing secrets in workspace markdown files.
- Disable channels you do not use.
- Treat `YOLO mode` as an operator convenience on trusted devices, not a general public deployment default.

## Logging

ESPClaw aims to keep normal logs operational, but logs can still reveal:

- device identity
- local IPs
- selected tool names
- provider failures
- file paths

Do not publish raw logs without review.

## Workspace Privacy

The workspace may contain:

- notes
- prompts
- retrieved docs
- generated code
- captured media

If you mount a shared SD card or publish workspace snapshots, review:

- `sessions/`
- `memory/`
- `media/`
- `config/`

## Community Components

Components installed from URLs or manifests are code execution inputs. Only install components you trust. For public registries, prefer:

- immutable versioned URLs
- checksums
- clear ownership
- human-readable docs

## Vulnerability Reporting

See [SECURITY.md](../SECURITY.md).
