# ESPClaw Components

## Overview

Components are reusable Lua modules with metadata. They are the intended way to share drivers, filters, and helper logic across multiple apps.

Each component lives under:

```text
/workspace/components/<component_id>/
├── component.json
└── module.lua
```

The runtime also publishes the module source into `/workspace/lib/...` so apps can load it with `require(...)`.

## When To Use Components

Use a component when:

- the Lua code should be reused by multiple apps
- the logic is low-level or infrastructural
- the code is a driver, filter, mixer, parser, or sensor adapter

Do not use a component for scheduling or autostart. That belongs to:

- apps for product logic
- tasks for live schedules
- behaviors for persisted schedules
- events for decoupled signaling

## Example: MS5611

Recommended shape:

- component: `ms5611_driver` exporting `require("sensors.ms5611")`
- app: `weather_station`
- app: `paragliding_vario`
- task or behavior: whichever schedule each app needs

Only introduce events if you want a separate sampler app that owns the sensor and fans out readings to multiple consumers.

## APIs

HTTP:

- `GET /api/components`
- `GET /api/components/detail?component_id=<id>`
- `POST /api/components/install?component_id=<id>&module=<module_name>[&title=<title>&summary=<summary>&version=<version>]`
- `POST /api/components/install/from-file?component_id=<id>&module=<module_name>&source_path=<workspace_path>`
- `POST /api/components/install/from-blob?component_id=<id>&module=<module_name>&blob_id=<blob_id>`
- `POST /api/components/install/from-url?component_id=<id>&module=<module_name>&source_url=<raw_lua_url>`
- `POST /api/components/install/from-manifest?manifest_url=<component_manifest_url>`
- `DELETE /api/components?component_id=<id>`

Model tools:

- `component.list`
- `component.install`
- `component.install_from_file`
- `component.install_from_blob`
- `component.install_from_url`
- `component.install_from_manifest`
- `component.remove`

## Registry Manifests

Community-shareable components should prefer a manifest URL over raw-source-only installs.

Recommended manifest shape:

```json
{
  "id": "ms5611_driver",
  "title": "MS5611 Driver",
  "module": "sensors.ms5611",
  "summary": "Shared pressure sensor driver and compensation math",
  "version": "0.2.0",
  "source_url": "https://example.com/components/ms5611.lua",
  "docs_url": "https://example.com/components/ms5611.md",
  "dependencies": [
    "https://example.com/components/i2c_helpers.json"
  ]
}
```

Current semantics:

- `source_url` is required
- `dependencies` are optional manifest URLs and are installed first
- the installed local `component.json` preserves `manifest_url`, `source_url`, `docs_url`, and `dependencies`
