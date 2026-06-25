"""Tool: single-trace native call tree (delegates to the native query backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def get_zone_tree(
    trace_file: str,
    zone_type: Literal["cpu", "gpu"] = "gpu",
    frame: int | None = None,
    start_second: float | None = None,
    end_second: float | None = None,
    max_depth: int = 6,
    limit: int = 200,
) -> dict:
    """Tracy's native call tree (ZoneEvent/GpuEvent children) for ONE trace.

    Each node carries inclusive_ms, self_ms (inclusive minus the sum of its
    direct children) and self_percent — so you can tell "slow itself" from "slow
    children" and avoid double-counting a parent with its child (e.g. OpaquePass
    vs its mesh_commands child). This is the only place GPU self-time is
    available, since GpuSourceLocationZones has none.

    Window: pass `frame` for one frame's tree, or `start_second`+`end_second`
    for a time window; otherwise the default warmup/cooldown-trimmed window.
    GPU timestamps are already CPU-aligned. Children are sorted by inclusive
    time; `limit` caps the total node count (`truncated` flags when hit).
    Trailing #N/:N in a node name is surfaced as `name_value`.
    """
    params: dict = {
        "trace_file": trace_file,
        "zone_type": zone_type,
        "max_depth": max_depth,
        "limit": limit,
    }
    if frame is not None:
        params["frame"] = frame
    elif start_second is not None and end_second is not None:
        params["start_second"] = start_second
        params["end_second"] = end_second
    result = query("zone_tree", params)
    if result.get("truncated") and "hint" not in result:
        result["hint"] = (
            f"树超过 {limit} 个节点已截断。请缩小窗口、减小 max_depth 或增大 limit。"
        )
    return result
