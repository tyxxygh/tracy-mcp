"""Tool: frame-time statistics + slowest-frame finder (native backend)."""

from tracy_mcp.backend.query_client import query


def get_frame_stats(
    trace_file: str,
    top_slowest: int = 10,
    budget_ms: float | None = None,
) -> dict:
    """Per-frame time distribution and the slowest frames.

    Returns mean/p50/p95/p99/min/max frame time, mean FPS, and the N slowest
    frames (with frame index + trace-relative start). If `budget_ms` is given,
    also counts frames exceeding it.
    """
    params: dict = {"trace_file": trace_file, "top_slowest": top_slowest}
    if budget_ms is not None:
        params["budget_ms"] = budget_ms
    return query("frame_stats", params)
