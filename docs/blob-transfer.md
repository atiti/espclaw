# ESPClaw Chunked Blob Transfer

## Why It Exists

ESPClaw runs on memory-constrained boards. Large markdown context files, Lua source files, or other documents should not be forced through one large admin request body or one giant in-memory prompt buffer.

The chunked blob store provides a generic streamed upload path that writes directly to the workspace filesystem.

## Blob Lifecycle

HTTP endpoints:

- `POST /api/blobs/begin?blob_id=<id>[&target_path=<workspace_path>&content_type=<mime>]`
- `POST /api/blobs/append?blob_id=<id>`
- `POST /api/blobs/commit?blob_id=<id>`
- `GET /api/blobs/status?blob_id=<id>`

Stages:

1. `begin`
   Creates a staging file under `/workspace/blobs/.staging/` and records metadata.
2. `append`
   Streams one chunk at a time directly into the staging file.
3. `commit`
   Atomically moves the completed blob into its final workspace path.
4. `status`
   Reports whether the blob is `none`, `open`, or `committed`, plus byte count and target path.

## Workspace Layout

```text
/workspace/
├── blobs/
│   ├── .staging/
│   │   ├── <blob_id>.part
│   │   └── <blob_id>.meta
│   ├── <blob_id>.meta
│   └── ...
```

If no `target_path` is provided, a committed blob defaults to:

```text
blobs/<blob_id>.bin
```

## Intended Uses

### Large Markdown Context

Upload a large document into `memory/` or another workspace path without requiring one huge admin request.

Example:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/blobs/begin?blob_id=context_doc&target_path=memory/context.md&content_type=text%2Fmarkdown'
curl -s -X POST 'http://127.0.0.1:8080/api/blobs/append?blob_id=context_doc' --data-binary @part1.md
curl -s -X POST 'http://127.0.0.1:8080/api/blobs/append?blob_id=context_doc' --data-binary @part2.md
curl -s -X POST 'http://127.0.0.1:8080/api/blobs/commit?blob_id=context_doc'
```

### Large Lua Source

This first slice only solves storage. The next intended layer is:

- `app.install_from_file`
- `app.install_from_url`
- `component.install_from_file`
- `component.install_from_url`

That now lets the model or operator upload large Lua code in pieces, commit it, and then install it from a workspace path rather than forcing all source through one JSON string.

Install endpoints:

- `POST /api/apps/install/from-file?app_id=<id>&source_path=<workspace_path>`
- `POST /api/apps/install/from-url?app_id=<id>&source_url=<raw_lua_url>`
- `POST /api/components/install/from-file?component_id=<id>&module=<module_name>&source_path=<workspace_path>`
- `POST /api/components/install/from-url?component_id=<id>&module=<module_name>&source_url=<raw_lua_url>`

Recommended operator/model sequence for large source:

1. `blobs/begin`
2. `blobs/append` as many times as needed
3. `blobs/commit`
4. `app.install_from_file` or `component.install_from_file`

Use `*_from_url` when the source already lives at a stable raw URL and should be fetched directly into the workspace staging area.

## Prompt-Side Guidance

This blob store is the right primitive for large context inputs, but large documents should still not be injected wholesale into every model run.

Recommended next step:

- chunk and store large docs in the workspace
- retrieve only the relevant excerpts or bounded segments for a given run
- pass selected chunks into the prompt instead of the entire file

That keeps the system memory-safe while still allowing large external context to be used deliberately.
