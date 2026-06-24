"""Tool: application messages/logs (delegates to the native query backend)."""

from tracy_mcp.backend.query_client import query


def get_messages(
    trace_file: str,
    start_second: float,
    end_second: float,
    filter_text: str | None = None,
    limit: int = 200,
) -> dict:
    """Messages in [start_second, end_second] (relative to trace start).

    Uses Tracy's real message stream (timestamp, thread, text).
    """
    return query("messages", {
        "trace_file": trace_file,
        "start_second": start_second,
        "end_second": end_second,
        "filter_text": filter_text or "",
        "limit": limit,
    })
