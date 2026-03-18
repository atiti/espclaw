# ESPClaw Component Registry Manifest

## Purpose

`component.install_from_manifest` is the shareable community path for reusable drivers and helpers.

Instead of pasting large Lua inline or only linking raw source, a manifest URL gives the runtime enough metadata to:

- identify the component
- download the real Lua source from `source_url`
- keep a docs link with the installed artifact
- install dependency manifests first

## Manifest Shape

Required fields:

- `id`
- `module`
- `source_url`

Recommended optional fields:

- `title`
- `summary`
- `version`
- `docs_url`
- `checksum`
- `dependencies`

Example:

```json
{
  "id": "ms5611_driver",
  "title": "MS5611 Driver",
  "module": "sensors.ms5611",
  "summary": "Shared pressure sensor driver and compensation math",
  "version": "0.2.0",
  "source_url": "https://example.com/components/ms5611.lua",
  "docs_url": "https://example.com/components/ms5611.md",
  "checksum": "",
  "dependencies": [
    "https://example.com/components/i2c_helpers.json"
  ]
}
```

## Install Flow

HTTP:

```bash
curl -X POST \
  'http://127.0.0.1:8080/api/components/install/from-manifest?manifest_url=https%3A%2F%2Fexample.com%2Fcomponents%2Fms5611.json'
```

Model tool:

```json
{
  "manifest_url": "https://example.com/components/ms5611.json"
}
```

## Current Behavior

- dependency entries are treated as manifest URLs and installed first
- `source_url` is fetched into workspace staging and then installed through the normal component install path
- installed local metadata preserves:
  - `manifest_url`
  - `source_url`
  - `docs_url`
  - `checksum`
  - `dependencies`

## Recommended Community Structure

For reusable sensor/driver packages:

1. publish one manifest per component
2. keep raw Lua source at a stable URL
3. keep human docs at a stable URL
4. keep dependencies small and explicit
5. compose end-user features as apps on top of components
