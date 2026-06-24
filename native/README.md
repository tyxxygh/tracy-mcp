# tracy-mcp-query (native backend)

A persistent C++ process that loads `.tracy` traces into Tracy's `Worker`
(the same analysis engine the GUI and csvexport use) and answers MCP queries
over line-delimited JSON on stdio. See [PROTOCOL.md](PROTOCOL.md).

## Why native

`tracy-csvexport` can only dump the full trace as CSV with no time-range
filter, no limits, and no call hierarchy — so the MCP server had to export
gigabytes and reconstruct trees/frames approximately in Python. The Worker
already holds, in memory and precomputed:

- per-source-location aggregates (`SourceLocationZones`: total/min/max/sumSq → mean/std)
- time-sorted zone vectors (binary-search time-range queries)
- the native call tree (`ZoneEvent::Child()` / `GetZoneChildren`)
- exact frame↔time mapping, GPU zones, messages, and plots

This backend exposes those directly, returning compact JSON.

## Build

Requires CMake ≥ 3.16, a C++20 compiler, and network access on the first
configure (CPM fetches capstone, zstd, PPQSort, nlohmann/json into
`native/.cpm_cache`). It does **not** build the GUI deps (ImGui/glfw/curl/…).

```bash
# from native/
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# → build/Release/tracy-mcp-query.exe
```

`TRACY_DIR` points at the Tracy source checkout (default
`D:/ajin/opensourceProjs/tracy_0.13.1`); override with
`-DTRACY_DIR=/path/to/tracy`.

The Python MCP server finds the executable automatically (build/Release,
build/Debug, build/, or PATH), or via `TRACY_MCP_QUERY_PATH`.

## Smoke test

```bash
build/Release/tracy-mcp-query.exe some.tracy   # milestone build: prints a summary
```
