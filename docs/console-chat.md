# Console Chat

ESPClaw now exposes one shared operator console across:

- `UART0` on real hardware
- the simulator process console
- the admin web chat surface

The web UI chat uses the same console executor as the serial console, so slash commands and plain LLM turns behave the same everywhere.

## Behavior

- Lines starting with `/` are handled as static operator commands.
- Any other input is treated as a normal LLM turn.
- Console exchanges are persisted to the same session transcript store as normal chat runs.

## Slash Commands

- `/help`
- `/status`
- `/tools`
- `/tool <name> [json]`
- `/wifi status`
- `/wifi scan`
- `/wifi join <ssid> [password]`
- `/memory`
- `/reboot`
- `/factory-reset`

## Tool Access

`/tool` executes the named ESPClaw tool directly with an optional JSON argument object.

Examples:

```text
/tool system.info {}
/tool fs.read {"path":"USER.md"}
/tool web.search {"query":"ms5611 datasheet"}
/tool web.fetch {"url":"https://example.com/doc.pdf"}
```

Because `/tool` is an explicit operator action, it runs with mutation enabled on the local console surface.

## UART Console

On firmware builds, ESPClaw starts a line-oriented console task on `UART0`.

- Input is echoed locally.
- Press Enter to submit a line.
- Responses are printed back to the same serial console.
- Device logs may interleave with console output on the same UART.

## YOLO Mode

The shared console supports a local `YOLO mode` flag.

- When enabled on the local operator surface, the model is instructed to execute allowed tools directly instead of adding another approval hop.
- This is intended for trusted local debugging, bench work, and hardware bring-up.
