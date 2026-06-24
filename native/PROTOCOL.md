# tracy-mcp-query protocol

A persistent process that loads `.tracy` traces into Tracy's `Worker` once and
answers MCP queries. Transport is **line-delimited JSON** over stdio: one
request object per line on stdin, one response object per line on stdout.
(Logs/diagnostics go to stderr only — stdout stays pure protocol.)

## Request

```json
{"id": 1, "method": "zone_stats", "params": {"trace_file": "C:/x.tracy", "top_n": 20}}
```

- `id`: client-chosen integer, echoed back to correlate responses.
- `method`: one of the methods below.
- `params`: method-specific; always includes `trace_file` (or `trace_file_a`/`_b`).

## Response

Success:
```json
{"id": 1, "ok": true, "result": { ... }}
```

Error:
```json
{"id": 1, "ok": false, "error": {"code": "load_failed", "message": "..."}}
```

Error codes: `bad_request`, `not_found`, `not_a_trace`, `load_failed`,
`unknown_method`, `internal`.

## Trace loading / caching

The daemon keeps a map `path -> Worker`. A trace is loaded lazily on first use
and reused for subsequent queries (this is the whole point — a 136 MB trace
loads once per session, not per query). Invalidation is by (mtime, size); an
optional LRU cap bounds resident memory.

## Methods (mirror the 8 MCP tools)

| method | key params | returns |
|---|---|---|
| `trace_info` | trace_file | name, span_seconds, total_zones, gpu_zones, frames, threads, gpu_contexts |
| `zone_stats` | trace_file, zone_type(cpu/gpu/all), filter_name, sort_by(…/self_time), top_n | zones[] with mean/min/max/std/total/count; CPU also self_total/self_mean/self_percent |
| `zone_timeline` | trace_file, start_second, end_second, filter_name, filter_thread, aggregation, interval_ms, limit, cursor | events[] / intervals[] within range (binary search on sorted zones) |
| `frame_stats` | trace_file, top_slowest, budget_ms | frame_ms{mean,p50,p95,p99,min,max}, fps_mean, slowest[], frames_over_budget |
| `zone_outliers` | trace_file, filter_name, zone_type, top_n, [start_second, end_second] | outliers[] = slowest instances {duration_ms, start_second, frame, thread} |
| `compare_traces` | trace_file_a, trace_file_b, zone_type, filter_name, sort_by, top_n, regression_threshold_pct | comparisons[] matched by (name,file,line) |
| `compare_timelines` | trace_file_a, trace_file_b, start_second, end_second, filter_name, max_depth, limit | native call tree (GetZoneChildren) with per-node a/b stats + delta |
| `messages` | trace_file, start_second, end_second, filter_text, limit | messages[] (native MessageData; has thread + text) |
| `plots` | trace_file, start_second, end_second, plot_name, downsample, max_points | real plot series (GetPlots) with downsampling |
| `frame_range` | trace_file, start_frame, end_frame | exact time range via GetFrameBegin/End (no 60fps guess) |

All numeric times in results are milliseconds unless named `_seconds`.
Limits/aggregation are applied in C++ so payloads stay small.
