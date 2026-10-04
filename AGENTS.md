# AGENTS.md — GodotAI engine development

GodotAI is a hard fork of [Godot Engine](https://github.com/godotengine/godot) aimed at
human + AI co-editing and vibe coding. Upstream conventions apply unless stated here.

## Build

```sh
pip install "scons>=4.4"            # distro scons is often too old
scons platform=linuxbsd target=editor linker=mold -j$(nproc)
./bin/godot.linuxbsd.editor.x86_64 --path /path/to/project
```

Ubuntu deps: `build-essential pkg-config libx11-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
libglu1-mesa-dev libasound2-dev libpulse-dev libudev-dev libxi-dev libxrandr-dev libwayland-dev mold`.

## Where GodotAI-specific code lives

- `modules/ai_bridge/` — editor MCP server (`ai_bridge_plugin.*`), AI dock, tool implementations
  (`ai_bridge_tools.cpp`). New AI tools: add a function + schema + entry in the `TOOLS` table.
- Small hooks in upstream files are allowed (e.g. `EditorLog::get_recent_messages`,
  `ScriptEditorDebugger::get_runtime_errors`). Keep them minimal and documented so upstream merges stay easy.

## Rules

- Run `pre-commit` (clang-format, file headers) before committing. New C++ files need the standard Godot license header.
- Every editor mutation exposed to AI must go through `EditorUndoRedoManager` so humans can undo it.
- The MCP server must only bind to localhost by default.
