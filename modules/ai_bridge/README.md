# AI bridge (MCP server in the editor)

GodotAI ships an [MCP](https://modelcontextprotocol.io) server inside the editor so AI coding agents
(Claude Code, Codex, Cursor, Devin, ...) can operate the same editor a human is using.

- Transport: Streamable HTTP, JSON responses, `POST http://127.0.0.1:6010/mcp`
- Starts automatically when a project is opened. Toggle it in the **AI** dock or with the
  `ai_bridge/enabled` editor setting. Port: `ai_bridge/port` (or env `GODOTAI_MCP_PORT`).
- Binds to `127.0.0.1` and rejects non-local `Origin` headers (DNS rebinding protection).
- Every scene edit goes through the editor's undo history: the human can press Ctrl+Z to revert
  what the AI did, and sees each call in the **AI** dock activity log.

## Connect an agent

```sh
# Claude Code
claude mcp add --transport http godot http://127.0.0.1:6010/mcp
```

Generic MCP client config:

```json
{ "mcpServers": { "godot": { "type": "http", "url": "http://127.0.0.1:6010/mcp" } } }
```

Headless (CI / no display): `godot --editor --headless --path my_project` also starts the server
(screenshots are unavailable in this mode).

## Tools

| Tool | Purpose |
| --- | --- |
| `editor_get_state` | Project info, edited/open scenes, selection, play state |
| `scene_get_tree` | Node tree of the edited scene (optionally with properties) |
| `scene_open` / `scene_new` / `scene_save` | Scene file management |
| `node_add` / `node_remove` / `node_reparent` | Edit the tree (undoable) |
| `node_set_properties` / `node_get_properties` | Read/write properties (undoable) |
| `node_attach_script` / `node_select` | Scripts and selection |
| `file_read` / `file_write` / `file_list` | Project files; written scripts hot-reload and return errors |
| `script_validate` | Compile-check a script without running |
| `project_run` / `project_stop` | Run the game |
| `debugger_get_errors` / `log_get` | Runtime errors with file:line + call stack, Output panel |
| `project_set_setting` | ProjectSettings |
| `class_get_info` | Engine API reference (properties, methods, signals, docs) |
| `editor_undo` | Undo / redo |
| `editor_run_script` | Run arbitrary GDScript inside the editor |
| `editor_screenshot` | PNG of the editor window or 2D/3D viewport |

### Property values

`node_add.properties` and `node_set_properties.properties` accept:

- JSON numbers / bools / strings
- Godot literals: `"Vector2(100, 50)"`, `"Color(1, 0, 0)"`, `"Rect2(0, 0, 10, 10)"`
- Arrays for vector-like types: `[100, 50]`
- Enum names: `"motion_mode": "Floating"`
- Resources: `"res://icon.svg"`, `"new:RectangleShape2D"`, or `{"type": "CircleShape2D", "radius": 16}`

## Quick test with curl

```sh
curl -s localhost:6010/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scene_get_tree","arguments":{}}}'
```
