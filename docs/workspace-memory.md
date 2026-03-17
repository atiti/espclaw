# Workspace Memory Files

ESPClaw bootstraps a small set of markdown control files in the workspace:

```text
/workspace/
├── AGENTS.md
├── IDENTITY.md
├── USER.md
├── HEARTBEAT.md
└── memory/MEMORY.md
```

## Current Behavior

Today these files are enabled in a simple, direct way:

- they are created during workspace bootstrap if missing
- they are concatenated into the system prompt when workspace storage is ready
- they are not semantically indexed or retrieved selectively
- they are not automatically summarized or compacted

This means they act as persistent prompt context, not as a structured memory database.

## What They Are For

- `AGENTS.md`: local operating rules for the device/runtime
- `IDENTITY.md`: board or device identity details
- `USER.md`: operator or owner preferences
- `HEARTBEAT.md`: short current-state notes
- `memory/MEMORY.md`: longer-lived persistent context

## How They Are Updated

There is no dedicated memory-management subsystem yet. These files are currently updated through ordinary workspace write paths:

- model tool: `fs.write`
- manual console command: `/tool fs.write {...}`
- Lua: `espclaw.fs.write(...)`
- any admin or app flow that writes files into the workspace

Example:

```text
/tool fs.write {"path":"USER.md","content":"Preferred language: English\n"}
```

## Limitations

This is intentionally simple, but it has real tradeoffs:

- every enabled file is injected on every run
- larger files consume prompt budget directly
- stale content is not automatically pruned
- there is no selective recall or ranking yet

## Recommended Use

Keep these files concise.

Good uses:

- device identity and deployment notes
- operator preferences
- short working memory and current objectives
- durable setup notes for the board or attached peripherals

Avoid turning them into large logs or full archives. Session transcripts and regular workspace files are better for that.
