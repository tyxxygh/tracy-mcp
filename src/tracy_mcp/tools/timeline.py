"""Tool: zone timeline within a time range (delegates to the native backend)."""

from typing import Literal

from tracy_mcp.backend.query_client import query


def get_zone_timeline(
    trace_file: str,
    start_second: float,
    end_second: float,
    filter_name: str | None = None,
    filter_thread: str | None = None,
    aggregation: Literal["raw", "by_frame", "by_interval"] = "raw",
    interval_ms: float = 16.67,
    limit: int = 500,
    cursor: int | None = None,
) -> dict:
    """Zone events in [start_second, end_second] (relative to trace start).

    The backend binary-searches Tracy's time-sorted zones, so only the matching
    window is materialized — no full-trace dump.
    """
    result = query("zone_timeline", {
        "trace_file": trace_file,
        "start_second": start_second,
        "end_second": end_second,
        "filter_name": filter_name or "",
        "filter_thread": filter_thread or "",
        "aggregation": aggregation,
        "interval_ms": interval_ms,
        "limit": limit,
        "cursor": cursor or 0,
    })
    if result.get("has_more") and "hint" not in result:
        total = result.get("total_events_in_range", 0)
        result["hint"] = (
            f"区间内共 {total} 条事件，已分页返回。可用 cursor 翻页，"
            f"或缩小时间范围 / 改用 by_interval 聚合获取概览。"
        )
    return result
