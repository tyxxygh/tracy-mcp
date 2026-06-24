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
) -> dict:
    """Match same-named zones across two traces and report deltas/regressions."""
    return query("compare_traces", {
        "trace_file_a": trace_file_a,
        "trace_file_b": trace_file_b,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
        "regression_threshold_pct": regression_threshold_pct,
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
