"""Tool: aggregated zone statistics (delegates to the native query backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def get_zone_stats(
    trace_file: str,
    zone_type: Literal["cpu", "gpu", "all"] = "all",
    filter_name: str | None = None,
    sort_by: Literal["total_time", "mean_time", "count", "max_time"] = "total_time",
    top_n: int = 50,
) -> dict:
    """Per-source-location aggregates (mean/min/max/std/total/count).

    Stats are precomputed in Tracy's Worker, so this is fast and exact.
    """
    return query("zone_stats", {
        "trace_file": trace_file,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
    })
