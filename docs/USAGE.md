# Tracy MCP 使用文档

让 AI 通过 MCP 访问 Tracy `.tracy` 性能 trace。本文覆盖：环境准备 → 构建后端 → 接入 MCP 客户端 → 每个工具的用法与示例 → 常见调试工作流 → 排错。

---

## 1. 环境准备

- **C++ 后端**：CMake ≥ 3.16、C++20 编译器（Windows 用 Visual Studio 2022）、首次构建需联网（CPM 拉取 capstone/zstd/PPQSort）。
- **Python**：3.11+。
- **Tracy 源码**：需要与你的 `.tracy` 文件**版本匹配**的 Tracy 源码（后端链接它的 `Worker`）。默认 `D:/ajin/opensourceProjs/tracy_0.13.1`。

> 为什么要源码版本匹配：`.tracy` 是带版本的二进制格式，旧版工具读新版 trace 会崩溃（例如 csvexport 0.12.2 读 0.13.x 直接 segfault）。后端用对应版本源码的 `Worker` 加载，从根上避免这个问题。

---

## 2. 构建 C++ 查询后端

```bash
cd native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64    # 首次配置会联网拉依赖
cmake --build build --config Release
# 产物：native/build/Release/tracy-mcp-query.exe
```

- 用 `-DTRACY_DIR=/path/to/tracy` 指定 Tracy 源码目录。
- 依赖只拉 capstone+zstd+PPQSort，**不会**构建 GUI 依赖（ImGui/glfw/curl 等）。
- 首次配置较慢（编译 `TracyWorker.cpp` + 依赖）；之后增量很快。依赖缓存在 `native/.cpm_cache/`。

**冒烟测试**（一次性模式，直接打印 trace 概况）：

```bash
native/build/Release/tracy-mcp-query.exe some.tracy
```

---

## 3. 安装 Python MCP 服务器

```bash
pip install -e ".[dev]"
```

后端可执行文件按以下顺序自动发现：`TRACY_MCP_QUERY_PATH` 环境变量 → `native/build/Release|Debug` → `native/build` → 系统 PATH。

---

## 4. 接入 MCP 客户端

**Claude Desktop**（`claude_desktop_config.json`）：

```json
{
  "mcpServers": {
    "tracy": {
      "command": "tracy-mcp",
      "env": { "TRACY_MCP_QUERY_PATH": "D:/ajin/opensourceProjs/tracy_mcp/native/build/Release/tracy-mcp-query.exe" }
    }
  }
}
```

`env` 可省略（能自动发现时）。任何支持 stdio 传输的 MCP 客户端同理：命令为 `tracy-mcp`。

---

## 5. 时间语义（重要）

所有工具的 `start_second` / `end_second` 输入、以及返回里的 `*_ms` / `*_second`，**统一相对 trace 起点**（trace 开始 = 0）。`tool_get_frame_range` 给出的就是这个口径，可直接喂给其它时间段工具。

---

## 6. 工具参考

> 下面示例用「请求参数 → 关键返回」的形式说明。AI 调用时只需给参数；返回为紧凑 JSON。

### tool_get_trace_info
trace 概况。**排查任何 trace 的第一步。**

- 参数：`trace_file`
- 返回：`capture_name`(程序名@捕获时间)、`span_seconds`、`total_zones`、`total_gpu_zones`、`total_frames`、`thread_count`、`gpu_context_count`、`frame_sets`

### tool_get_zone_stats
按源位置聚合的 zone 统计（Worker 预计算，快）。**找热点。**

- 参数：`trace_file`、`zone_type`(`cpu`/`gpu`/`all`，默认 all)、`filter_name`(子串，大小写不敏感)、`sort_by`(`total_time`/`mean_time`/`count`/`max_time`/`self_time`)、`top_n`(默认 50，上限 500)
- 返回：`zones[]`（`name/src_file/src_line/zone_type/total_ms/total_percent/count/mean_ms/min_ms/max_ms/std_ms`）+ `summary`(`total_time_ms`、`top_5_consume_percent`)
- **self（独占）时间**：CPU zone 额外含 `self_total_ms/self_mean_ms/self_max_ms/self_percent`。`self_percent` 接近 100% 说明是叶子热点（时间花在自身）；偏低说明耗时在子调用。`sort_by:"self_time"` 直接找真热点。GPU zone 无 self（字段为 null）。
- 示例：`{"trace_file":"x.tracy","zone_type":"gpu","filter_name":"Shadow","top_n":10}`

### tool_get_frame_stats
帧时间分布 + 最慢帧。**判断「有没有超帧预算、卡在哪几帧」的第一工具。**

- 参数：`trace_file`、`top_slowest`(默认 10，上限 100)、`budget_ms`(可选，如 16.67=60fps)
- 返回：`frame_count`、`fps_mean`、`frame_ms{mean,p50,p95,p99,min,max}`、`slowest[{frame,start_second,duration_ms}]`；传了 `budget_ms` 还有 `frames_over_budget`、`percent_over_budget`
- 提示：看 `p50`（典型帧）vs `mean`（受启动/卡顿帧拉高）；`p99` 是卡顿尾部。**注意**：profiling 开始前的启动帧 `start_second` 可能为负（如加载帧）。

### tool_get_zone_outliers
某个 zone **最慢的若干次调用**。**把尖刺定位到具体帧。**

- 参数：`trace_file`、`filter_name`(必填，部分匹配)、`zone_type`(`cpu`/`gpu`)、`top_n`(默认 10，上限 200)、`start_second`/`end_second`(可选窗口)
- 返回：`outliers[{duration_ms, start_second, frame, thread, name, src_file, src_line}]`（按耗时降序）+ `matched_locations`、`total_instances`
- 用途：均值正常但偶发卡顿时，先 `zone_stats` 看 `max_ms ≫ mean_ms`，再用本工具看「最慢那几次在第几帧、哪个线程」。

### tool_get_zone_timeline
时间段内的逐个 zone 事件（对时间排序的 zone 做二分查找，不导出全量）。

- 参数：`trace_file`、`start_second`、`end_second`、`filter_name`、`filter_thread`、`aggregation`(`raw`/`by_interval`)、`interval_ms`(默认 16.67)、`limit`(默认 500，上限 5000)、`cursor`(翻页)
- 返回（raw）：`events[]`（`name/thread/src_file/src_line/start_ms/duration_ms`）、`total_events_in_range`、`has_more`、`next_cursor`
- 返回（by_interval）：`intervals[]`（每桶 `top_zones`）
- 提示：窗口内事件极多时用 `by_interval` 看概览，或用 `cursor` 翻页。

### tool_compare_traces
两个 trace 的聚合 zone 对比（按 name+src_file+src_line 匹配）。**回归检测。**

- 参数：`trace_file_a`、`trace_file_b`、`zone_type`、`filter_name`、`sort_by`(`delta_percent`/`total_time_a`/`total_time_b`)、`top_n`、`regression_threshold_pct`(默认 10)
- 返回：`comparisons[]`（每条含 `a`/`b`/`delta` + `regression`/`improvement`）+ `summary`（`regression_count`、`overall_delta_percent`、`top_regression`）

### tool_compare_timelines
两个 trace 在时间窗内的**原生调用树**逐层对比（不是重建，是 Tracy 的 `ZoneEvent::Child()`）。**看「时间花在树的哪个分支」。**

- 参数：`trace_file_a`、`trace_file_b`、`start_second`、`end_second`、`zone_type`(`cpu`/`gpu`)、`filter_name`、`max_depth`(默认 5，上限 12)、`limit`(节点数，默认 300)
- 返回：`tree`（根 `<root>`，每节点 `name`/`a`/`b`/`delta_percent`/`children`，`a`/`b` 含 `count/total_ms/mean_ms/min_ms/max_ms/median_ms`）、`total_nodes`、`truncated`
- GPU 用 `zone_type:"gpu"`：走 GPU 上下文的 `GpuEvent` 树。

### tool_get_messages
时间段内的应用消息/日志。

- 参数：`trace_file`、`start_second`、`end_second`、`filter_text`、`limit`(默认 200)
- 返回：`messages[]`（`timestamp_ms`/`thread`/`text`）、`total_in_range`、`has_more`

### tool_get_plots
时间段内的 plot 时序数据，支持降采样。

- 参数：`trace_file`、`start_second`、`end_second`、`plot_name`(留空则在 `available_plots` 列出所有 plot 名)、`downsample`、`max_points`(默认 500，上限 5000)
- 返回：`plots[]`（`name`/`points[{time_ms,value}]`/`stats{min,max,mean,count}`）

### tool_get_frame_range
帧号 → **精确**时间范围（用真实帧数据，非 60fps 估算）。

- 参数：`trace_file`、`start_frame`、`end_frame`
- 返回：`time_range`（`start_second`/`end_second`/`duration_seconds`，`estimated:false`）

---

## 7. 常见调试工作流

**对比两次 capture 的 GPU 开销，定位回归到具体 pass：**
1. `tool_get_trace_info` 两个文件，确认 `span_seconds` / `total_gpu_zones` 量级相近。
2. `tool_compare_timelines`，`zone_type:"gpu"`，窗口覆盖多帧（如 0~12s），`max_depth:6`。
3. 看树里每个 pass 的 `a`/`b` 均值与 `delta_percent`，定位变慢的分支。

**有没有超帧预算、卡在哪几帧：**
1. `tool_get_frame_stats`，`budget_ms`=你的预算（如 16.67）。看 `p99` / `frames_over_budget` / `slowest`。
2. 对某个慢帧，`tool_get_frame_range`(该帧号) → 时间窗 → `tool_get_zone_timeline` 看那一帧花在哪。

**找某个 zone 的尖刺：**
1. `tool_get_zone_stats` `filter_name`=该 zone，看 `max_ms` 远大于 `mean_ms`，或 `self_percent` 判断是自身还是子调用。
2. `tool_get_zone_outliers` `filter_name`=该 zone → 直接拿到最慢几次的 `frame` / `thread` / `start_second`。
3. 拿可疑帧号 `tool_get_frame_range` → `tool_get_zone_timeline` 看那一段细节。

**找真正的叶子热点：**
`tool_get_zone_stats` `sort_by:"self_time"` —— self 时间排序排除「只是子调用慢」的父节点。

---

## 8. 排错

| 现象 | 原因 / 处理 |
|---|---|
| `backend_not_found` | 后端没构建或没找到。构建 `native/`，或设 `TRACY_MCP_QUERY_PATH`。 |
| `not_a_trace` / `load_failed` | 文件不是 `.tracy`，或 Tracy 源码版本与 trace 不匹配 → 用匹配版本源码重建后端（`-DTRACY_DIR`）。 |
| 首次 cmake 配置卡住 | 多半是 CPM 联网拉依赖慢；耐心等或检查网络。依赖只需拉一次（缓存在 `.cpm_cache`）。 |
| 查询很慢（首次） | 大 trace 首次加载需几秒（一次性）；之后同一文件的查询走内存缓存，秒回。 |
| 时间对不上 | 记住时间是**相对 trace 起点**；用 `tool_get_frame_range` 产出的秒数对齐。 |

---

## 9. 开发与测试

```bash
pytest tests/test_client.py -v                                   # 客户端协议单测（无需后端）
TRACY_TEST_FILE=/path/to/x.tracy pytest tests/test_tools.py -v   # 端到端（需后端 + 真实 trace）
```

加新工具：`native/src/main.cpp` 写 `h_xxx` 处理器 + 在 `dispatch()` 注册一行 + `src/tracy_mcp/tools/` 加薄转发 + `server.py` 加 `@mcp.tool`。
