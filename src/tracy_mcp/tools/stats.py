"""Tool: aggregated zone statistics (delegates to the native query backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def get_zone_stats(
    trace_file: str,
    zone_type: Literal["cpu", "gpu", "all"] = "all",
    filter_name: str | None = None,
    sort_by: Literal["total_time", "mean_time", "count", "max_time", "self_time"] = "total_time",
    top_n: int = 50,
) -> dict:
    """Per-source-location aggregates (mean/min/max/std/total/count).

    Stats are precomputed in Tracy's Worker, so this is fast and exact. CPU
    zones also include self (exclusive) time — self_total_ms/self_mean_ms/
    self_percent — which distinguishes "slow itself" from "slow children".
    Sort by "self_time" to find true leaf hotspots.
    """
    return query("zone_stats", {
        "trace_file": trace_file,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
    })


def get_zone_outliers(
    trace_file: str,
    filter_name: str,
    zone_type: Literal["cpu", "gpu"] = "cpu",
    top_n: int = 10,
    start_second: float | None = None,
    end_second: float | None = None,
) -> dict:
    """The slowest individual invocations of a zone.

    Returns the worst `top_n` instances (by duration), each with its duration,
    trace-relative start, containing frame index, and thread — so a spike can
    be pinned to a specific frame. Optionally restrict to a time window.
    """
    params: dict = {
        "trace_file": trace_file,
        "filter_name": filter_name,
        "zone_type": zone_type,
        "top_n": top_n,
    }
    if start_second is not None and end_second is not None:
        params["start_second"] = start_second
        params["end_second"] = end_second
    return query("zone_outliers", params)
