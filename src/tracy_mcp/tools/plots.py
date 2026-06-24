"""Tool: plot/time-series data (delegates to the native query backend)."""

from tracy_mcp.backend.query_client import query


def get_plots(
    trace_file: str,
    start_second: float,
    end_second: float,
    plot_name: str | None = None,
    downsample: int = 1,
    max_points: int = 500,
) -> dict:
    """Tracy plot series in a time window, with optional downsampling.

    With no plot_name, lists available plot names in `available_plots`.
    """
    return query("plots", {
        "trace_file": trace_file,
        "start_second": start_second,
        "end_second": end_second,
        "plot_name": plot_name or "",
        "downsample": downsample,
        "max_points": max_points,
    })
