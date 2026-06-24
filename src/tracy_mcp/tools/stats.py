"""Tool: aggregated zone statistics (delegates to the native query backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def get_zone_stats(
    trace_file: str,
    zone_type: Literal["cpu", "gpu", "all"] = "all",
    filter_name: str | None = None,
    sort_by: Literal["total_time", "mean_time", "count", "max_time", "self_time"] = "total_time",
    top_n: int = 50,
    skip_first_frames: int = 10,
    skip_last_frames: int = 4,
) -> dict:
    """Per-source-location aggregates (mean/min/max/std/total/count).

    By default the first 10 and last 4 frames are excluded (Tracy
    connect/disconnect overhead perturbs them); the response's `trim` field
    reports what was excluded. Set skip_first_frames=skip_last_frames=0 to use
    the whole trace (and the faster precomputed path).

    CPU zones also include self (exclusive) time — self_total_ms/self_mean_ms/
    self_percent — which distinguishes "slow itself" from "slow children".
    Sort by "self_time" to find true leaf hotspots.

    If a zone name encodes a trailing number (drawcalls/id, e.g.
    "mesh_commands_total#526"), it is surfaced as a numeric `name_value` field,
    so you can correlate it with time (e.g. us per drawcall) directly.
    """
    return query("zone_stats", {
        "trace_file": trace_file,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
        "skip_first_frames": skip_first_frames,
        "skip_last_frames": skip_last_frames,
    })


def get_zone_jitter(
    trace_file: str,
    zone_type: Literal["cpu", "gpu", "all"] = "all",
    filter_name: str | None = None,
    sort_by: Literal["std", "spike", "cv", "max", "range"] = "std",
    top_n: int = 30,
    skip_first_frames: int = 10,
    skip_last_frames: int = 4,
) -> dict:
    """Find the jittery / spiky zones — the stutter sources an average hides.

    Per source location, computes the spread of its per-instance time:
    mean/std/cv, min/max, p50/p95/p99, and spike_ms (p99 - p50, the headroom
    the worst frames have over the typical frame). A zone with high std/spike
    and a meaningful mean is what makes frame time jump.

    By default the first 10 / last 4 frames are excluded — warmup/cooldown
    spikes would otherwise dominate the jitter. GPU timestamps are CPU-aligned,
    so GPU zones use the same window.

    sort_by: "std" (default, absolute jitter in ms), "spike" (p99-p50),
    "cv" (std/mean, relative), "max", or "range" (max-min).
    """
    return query("zone_jitter", {
        "trace_file": trace_file,
        "zone_type": zone_type,
        "filter_name": filter_name or "",
        "sort_by": sort_by,
        "top_n": top_n,
        "skip_first_frames": skip_first_frames,
        "skip_last_frames": skip_last_frames,
    })


def get_zone_outliers(
    trace_file: str,
    filter_name: str,
    zone_type: Literal["cpu", "gpu"] = "cpu",
    top_n: int = 10,
    start_second: float | None = None,
    end_second: float | None = None,
    skip_first_frames: int = 10,
    skip_last_frames: int = 4,
) -> dict:
    """The slowest individual invocations of a zone.

    Returns the worst `top_n` instances (by duration), each with its duration,
    trace-relative start, containing frame index, and thread — so a spike can
    be pinned to a specific frame.

    By default the first 10 / last 4 frames are excluded (warmup/cooldown).
    Passing an explicit start_second+end_second window overrides the trim.
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
    else:
        params["skip_first_frames"] = skip_first_frames
        params["skip_last_frames"] = skip_last_frames
    return query("zone_outliers", params)
