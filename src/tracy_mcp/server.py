"""FastMCP server entry point — defines all Tracy MCP tools."""

import sys

from fastmcp import FastMCP

from tracy_mcp.backend.query_client import query, _find_backend, QueryBackendError
from tracy_mcp.tools.info import get_trace_info
from tracy_mcp.tools.stats import get_zone_stats
from tracy_mcp.tools.timeline import get_zone_timeline
from tracy_mcp.tools.compare import compare_traces, compare_timelines
from tracy_mcp.tools.messages import get_messages
from tracy_mcp.tools.plots import get_plots

# Create FastMCP server
mcp = FastMCP(
    "Tracy Profiler",
    instructions="Access Tracy profiler trace files — zone stats, timelines, and cross-trace comparison",
)


@mcp.tool
def tool_get_trace_info(trace_file: str) -> dict:
    """获取 Tracy trace 文件的基本元数据。

    返回程序名、捕获时间、总时长、zone 总数、帧数、线程数等信息。
    这是了解一个 trace 文件概况的第一步。

    Args:
        trace_file: .tracy 文件的路径（绝对路径或相对路径）
    """
    return get_trace_info(trace_file)


@mcp.tool
def tool_get_zone_stats(
    trace_file: str,
    zone_type: str = "all",
    filter_name: str | None = None,
    sort_by: str = "total_time",
    top_n: int = 50,
) -> dict:
    """获取 Tracy trace 文件中 zone 的聚合统计信息。

    返回每个 zone（按源位置聚合）的总耗时、均值、最小值、最大值、标准差、调用次数等。
    使用聚合模式，数据量小，速度快。

    Args:
        trace_file: .tracy 文件路径
        zone_type: 过滤 zone 类型 — "cpu"(CPU zone), "gpu"(GPU zone), "all"(全部)
        filter_name: 按 zone 名称过滤（支持部分匹配）
        sort_by: 排序方式 — "total_time"(总耗时), "mean_time"(均值), "count"(调用次数), "max_time"(最大单次)
        top_n: 返回前 N 条结果，默认 50，最大 200
    """
    return get_zone_stats(
        trace_file,
        zone_type=zone_type,  # type: ignore
        filter_name=filter_name,
        sort_by=sort_by,  # type: ignore
        top_n=top_n,
    )


@mcp.tool
def tool_get_zone_timeline(
    trace_file: str,
    start_second: float,
    end_second: float,
    filter_name: str | None = None,
    filter_thread: str | None = None,
    aggregation: str = "raw",
    interval_ms: float = 16.67,
    limit: int = 500,
    cursor: int | None = None,
) -> dict:
    """获取指定时间范围内的 zone 事件时间线。

    支持三种模式：
    - raw: 返回单个 zone 事件（默认）
    - by_frame: 按帧聚合（需要 trace 包含帧标记）
    - by_interval: 按时间间隔聚合（默认 16.67ms ≈ 60fps）

    Args:
        trace_file: .tracy 文件路径
        start_second: 开始时间（秒），必填
        end_second: 结束时间（秒），必填
        filter_name: 按 zone 名称过滤
        filter_thread: 按线程名过滤
        aggregation: "raw" | "by_frame" | "by_interval"
        interval_ms: by_interval 模式下的聚合间隔（毫秒），默认 16.67
        limit: 最大返回条数，默认 500，最大 2000
        cursor: 分页游标（仅 raw 模式）
    """
    return get_zone_timeline(
        trace_file,
        start_second=start_second,
        end_second=end_second,
        filter_name=filter_name,
        filter_thread=filter_thread,
        aggregation=aggregation,  # type: ignore
        interval_ms=interval_ms,
        limit=limit,
        cursor=cursor,
    )


@mcp.tool
def tool_compare_traces(
    trace_file_a: str,
    trace_file_b: str,
    zone_type: str = "all",
    filter_name: str | None = None,
    sort_by: str = "delta_percent",
    top_n: int = 50,
    regression_threshold_pct: float = 10.0,
) -> dict:
    """对比两个 trace 文件的 zone 统计数据。

    匹配同名 zone，输出差值、回退检测。
    两个文件同时导出（并行），耗时约等于慢的那个。

    Args:
        trace_file_a: 基准 trace 文件路径
        trace_file_b: 对比 trace 文件路径
        zone_type: "cpu" | "gpu" | "all"
        filter_name: 按 zone 名称过滤
        sort_by: "delta_percent"(变化百分比), "total_time_a", "total_time_b"
        top_n: 返回前 N 条，默认 50
        regression_threshold_pct: 回退标记阈值(%)，超过此值标记为回退
    """
    return compare_traces(
        trace_file_a,
        trace_file_b,
        zone_type=zone_type,  # type: ignore
        filter_name=filter_name,
        sort_by=sort_by,  # type: ignore
        top_n=top_n,
        regression_threshold_pct=regression_threshold_pct,
    )


@mcp.tool
def tool_compare_timelines(
    trace_file_a: str,
    trace_file_b: str,
    start_second: float,
    end_second: float,
    filter_name: str | None = None,
    max_depth: int = 5,
    limit: int = 300,
) -> dict:
    """对比两个 trace 文件在指定时间范围内的 zone 时间线，以树状结构展示。

    按 zone 名称匹配，每个节点包含 a/b 两侧的 count/total/mean/min/max/median 和变化百分比。

    Args:
        trace_file_a: 基准 trace 文件路径
        trace_file_b: 对比 trace 文件路径
        start_second: 开始时间（秒），必填
        end_second: 结束时间（秒），必填
        filter_name: 按 zone 名称过滤
        max_depth: 树的最大深度，默认 5
        limit: 最大节点数，默认 300
    """
    return compare_timelines(
        trace_file_a,
        trace_file_b,
        start_second=start_second,
        end_second=end_second,
        filter_name=filter_name,
        max_depth=max_depth,
        limit=limit,
    )


@mcp.tool
def tool_get_messages(
    trace_file: str,
    start_second: float,
    end_second: float,
    filter_text: str | None = None,
    limit: int = 200,
) -> dict:
    """获取 trace 文件中的应用程序消息/日志。

    提取指定时间范围内的消息（trace 中极短 duration 的 zone 通常为消息）。

    Args:
        trace_file: .tracy 文件路径
        start_second: 开始时间（秒），必填
        end_second: 结束时间（秒），必填
        filter_text: 按消息文本过滤（部分匹配）
        limit: 最大返回条数，默认 200，最大 500
    """
    return get_messages(
        trace_file,
        start_second=start_second,
        end_second=end_second,
        filter_text=filter_text,
        limit=limit,
    )


@mcp.tool
def tool_get_plots(
    trace_file: str,
    start_second: float,
    end_second: float,
    plot_name: str | None = None,
    downsample: int = 1,
    max_points: int = 500,
) -> dict:
    """获取 trace 文件中的图表/时序数据。

    提取指定时间范围内的时序数据，支持降采样以减少数据量。

    Args:
        trace_file: .tracy 文件路径
        start_second: 开始时间（秒），必填
        end_second: 结束时间（秒），必填
        plot_name: 图表名称（不传则返回聚合后的 zone 耗时曲线）
        downsample: 降采样因子 (1=原始, 10=每10个点取1个)
        max_points: 最大数据点数，默认 500，最大 2000
    """
    return get_plots(
        trace_file,
        start_second=start_second,
        end_second=end_second,
        plot_name=plot_name,
        downsample=downsample,
        max_points=max_points,
    )


@mcp.tool
def tool_get_frame_range(
    trace_file: str,
    start_frame: int,
    end_frame: int,
) -> dict:
    """将帧号范围转换为时间范围，方便后续使用时间范围工具。

    Tracy 用户通常按帧思考性能问题（如"第 100-200 帧"），
    此工具将帧号转换为秒数，输出可直接用于 get_zone_timeline 等工具。

    Args:
        trace_file: .tracy 文件路径
        start_frame: 起始帧号
        end_frame: 结束帧号
    """
    # Exact frame->time mapping from Tracy's frame data (not a 60fps estimate).
    result = query("frame_range", {
        "trace_file": trace_file,
        "start_frame": start_frame,
        "end_frame": end_frame,
    })
    tr = result.get("time_range")
    if tr:
        result["hint"] = (
            f"已转换为时间范围 [{tr['start_second']:.3f}s, {tr['end_second']:.3f}s]（精确帧时间）。"
            f"可将 start_second={tr['start_second']}, end_second={tr['end_second']} 用于 "
            f"get_zone_timeline / compare_timelines / get_messages / get_plots 等工具。"
        )
    return result


def main():
    """Entry point for `tracy-mcp` command."""
    # Locate the native query backend before starting (don't hard-fail; the
    # error is also returned per-call so the user sees it in the client).
    try:
        path = _find_backend()
        print(f"[tracy-mcp] Found query backend: {path}", file=sys.stderr)
    except QueryBackendError as e:
        print(f"[tracy-mcp] WARNING: {e.message}", file=sys.stderr)
        print("[tracy-mcp] Build native/ (see native/README.md) or set TRACY_MCP_QUERY_PATH.",
              file=sys.stderr)

    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
