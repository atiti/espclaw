# Web Tools

ESPClaw now exposes proxy-backed network retrieval tools for the model and for manual operator use.

## Tools

- `web.search`
- `web.fetch`

## Backend

The current generic adapter is configured against:

- search: `https://llmproxy.markster.io/v1/search?q=...`
- fetch: `https://llmproxy.markster.io/v1/scrape?url=...`

The adapter lives in firmware code, so the model does not need to know the backend-specific endpoint shape.

## `web.search`

Runs a remote search and returns a compact structured summary of top results.

Example:

```json
{"query":"ms5611 datasheet"}
```

## `web.fetch`

Fetches and scrapes a page or document, returns a compact response, and persists larger markdown into the workspace when useful.

Example:

```json
{"url":"https://sensorsandpower.angst-pfister.com/fileadmin/products/datasheets/183/MS5611-01BA03_1610-21559-0020-E-0816.pdf"}
```

Large fetched content is stored under:

```text
memory/web_fetch_<hash>.md
```

The tool response includes the stored path so later tool calls or Lua logic can inspect the fetched document through normal workspace APIs.

## Manual Use

These tools are also available through the shared console:

```text
/tool web.search {"query":"ms5611 datasheet"}
/tool web.fetch {"url":"https://example.com/file.pdf"}
```
