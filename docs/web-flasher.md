# Web Flasher

ESPClaw ships a browser-based landing page and flasher at `https://espclaw.dev/`.

It is designed to do three things well:

- explain what ESPClaw is and what it is for
- let users flash the latest firmware from a browser
- let users try the real ESPClaw runtime in the browser before touching hardware

## How It Works

The site is a static site deployed to Cloudflare Pages.

At runtime it:

1. queries the latest GitHub release from `atiti/espclaw`
2. finds the published `esp-web-tools` manifest for each target
3. renders browser flash buttons for:
   - `esp32`
   - `esp32s3`
4. links the corresponding release zip bundle for manual flashing
5. exposes an experimental browser lab that runs the real ESPClaw C runtime compiled to WebAssembly and can use WebLLM or an OpenAI-compatible endpoint for the agent surface

The repo also contains a GitHub Actions site workflow that builds and validates this bundle on every push. That workflow is a build check, not the live host serving `espclaw.dev`.

Manual live deploys use:

```bash
./scripts/deploy_site_cloudflare.sh
```

The release workflow publishes:

- `esp-web-tools-manifest-esp32.json`
- `esp-web-tools-manifest-esp32s3.json`
- `espclaw-esp32-<tag>.zip`
- `espclaw-esp32s3-<tag>.zip`

## Browser Requirements

Use a Chromium-class browser with Web Serial support:

- Chrome
- Edge
- Brave

Safari and Firefox are not sufficient for browser flashing here.

For the browser lab:

- local WebLLM use additionally depends on browser WebGPU support
- if WebGPU is unavailable, the page can still target an OpenAI-compatible endpoint

## First Flash

For most boards, the first wired flash gets ESPClaw onto the device.
After that, use the admin UI OTA path for most upgrades.

See [OTA](ota.md) for the current migration and partition-layout caveats.

## Manual Fallback

Every tagged release also publishes raw firmware assets and a zipped bundle per target, so users can still flash with `esptool.py` if browser flashing is unavailable or if their board needs a more manual bring-up path.

## Browser Lab Scope

The browser lab is intentionally an experimental operator-facing surface, but it is no longer a fake JavaScript kernel. It executes the real `espclaw_core` runtime in WebAssembly and is useful for making the ESPClaw mental model legible before touching a board:

- components
- apps
- tasks
- behaviors
- events
- system logs

It is useful for demos, prompt iteration, tool-path debugging, and explaining how the runtime is expected to structure work before users flash a board.
