"""Tool: cross-trace comparison (delegates to the native query backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def compare_traces(
    trace_file_a: str,
    trace_file_b: str,
    zone_type: Literal["cpu", "gpu", "all"] = "all",
    filter_name: str | None = None,
    sort_by: Literal["delta_percent", "total_time_a", "total_time_b"] = "delta_percent",
    top_n: int = 50,
    regression_threshold_pct: float = 10.0,
    normalize_names: bool = True,
    skip_first_frames: int = 10,
    skip_last_frames: int = 4,
) -> dict:
    """Match same-named zones across two traces and report deltas/regressions.

    Each trace excludes its own first 10 / last 4 frames by default
    (warmup/cooldown), so the compared means are cleaner; the `trim` field
    reports it. Set both to 0 to compare whole traces.

    `normalize_names` (default True) folds per-capture numeric ids in zone
    names before matching — "mesh_commands_total#306" -> "...#N",
    "pps_from:511" -> "pps_from:N" — so the same logical pass matches across
    captures even when its drawcall count / pass id differs. Folded rows carry
    `merged_names` (how many variants collapsed). Set False for exact-name
    matching. Resolutions/params (3840x2103, ShaderQuality=2) are not folded.
    """
    return query("compare_traces", {
        "trace_file_a": trace_file_a,
        "trace_file_b": trace_file_b,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
        "regression_threshold_pct": regression_threshold_pct,
        "normalize_names": normalize_names,
        "skip_first_frames": skip_first_frames,
        "skip_last_frames": skip_last_frames,
    })


def compare_frames(
    trace_file: str,
    frame_a: int,
    frame_b: int,
    zone_type: Literal["cpu", "gpu", "all"] = "gpu",
    filter_name: str | None = None,
    sort_by: Literal["abs_delta", "time_a", "time_b"] = "abs_delta",
    top_n: int = 50,
) -> dict:
    """Diff two frames *within one trace*, zone by zone (like the profiler's Frame compare).

    For each frame, sums per-source-location zone time inside that frame's
    [begin, end) window. Each zone instance is assigned to exactly one frame by
    its midpoint, so a GPU "Frame" zone longer than the CPU window (GPU-bound) or
    one straddling a boundary is counted once — no double counting. GPU timestamps
    are already CPU-aligned, so CPU and GPU share the same frame windows.

    Rows carry frame_a/frame_b {time_ms,count}, the delta, and
    percent_of_frame_delta (how much of the frame-time difference this zone
    explains). Defaults to GPU zones. Sorted by |delta| descending.
    """
    return query("compare_frames", {
        "trace_file": trace_file,
        "frame_a": frame_a,
        "frame_b": frame_b,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
    })


def compare_timelines(
    trace_file_a: str,
    trace_file_b: str,
    start_second: float,
    end_second: float,
    filter_name: str | None = None,
    max_depth: int = 5,
    limit: int = 300,
) -> dict:
    """Compare the two traces' call trees in a time window.

    The tree is Tracy's *native* call hierarchy (ZoneEvent children), not a
    reconstruction — exact parent/child with per-node a/b stats and deltas.
    """
    result = query("compare_timelines", {
        "trace_file_a": trace_file_a,
        "trace_file_b": trace_file_b,
        "start_second": start_second,
        "end_second": end_second,
        "filter_name": filter_name or "",
        "max_depth": max_depth,
        "limit": limit,
    })
    if result.get("truncated") and "hint" not in result:
        result["hint"] = (
            f"树超过 {limit} 个节点已截断。请缩小时间范围、增大 limit 或减小 max_depth。"
        )
    return result
