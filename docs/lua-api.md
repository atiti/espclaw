# Lua API

ESPClaw keeps the model-facing Lua app contract in a single runtime registry.

Authoritative sources:

- model tool: `lua_api.list`
- admin API JSON: `GET /api/lua-api`
- generated Markdown: `GET /api/lua-api.md`

These surfaces are generated from the same registry that the agent loop uses when it injects the compact Lua app contract into app-generation runs.

## Why This Exists

The model previously hallucinated generic helpers such as `pwm.write`, `i2c_write`, or external modules like `cjson`.

The registry fixes that by making the contract explicit:

- valid handler shapes
- exact `espclaw.*` function signatures
- the rule that external modules are not assumed to exist
- the recommendation to call `lua_api.list` when exact signatures matter

## Prompt Strategy

The agent loop now uses a layered strategy:

- every run gets the normal tool inventory snapshot
- Lua/app-generation requests additionally get a compact `Lua App Contract` snapshot generated from the registry
- the model can call `lua_api.list` for the full authoritative listing
- board-specific pins and buses still come from `hardware.list`

This keeps normal chat runs small while making Lua generation much more reliable.
