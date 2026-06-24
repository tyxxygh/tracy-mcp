"""Tool: basic trace metadata (delegates to the native query backend)."""

from tracy_mcp.backend.query_client import query


def get_trace_info(trace_file: str) -> dict:
    """Return program name, capture time, span, zone/frame/thread counts.

    All values come from Tracy's Worker (loaded once and cached), so this is
    exact — including program name and capture time, which csvexport can't give.
    """
    return query("trace_info", {"trace_file": trace_file})
