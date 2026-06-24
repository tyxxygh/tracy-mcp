# Tracy MCP Server

MCP (Model Context Protocol) 服务器，让 AI 代理能够访问 [Tracy Profiler](https://github.com/wolfpld/tracy) 的性能分析数据。

数据后端是一个**常驻的 C++ 查询进程**，直接链接 Tracy 自己的分析引擎（`TracyServer` / `Worker`，与 GUI 和 csvexport 同源）。它把 `.tracy` 文件加载进内存一次，之后所有查询都在内存里完成 —— 直接复用 Tracy 预计算好的统计、时间排序的 zone 向量、原生调用树、精确帧时间等。相比早期基于 `tracy-csvexport` 的方案，避免了 GB 级 CSV 导出、Python 端重建调用树、以及帧时间估算。

## 功能

- **聚合统计**: 每个源位置的 均值/最小/最大/标准差/总耗时/调用次数（Worker 预计算，秒级）
- **时间线查询**: 指定时间段内的 zone 事件（对时间排序的 zone 做二分查找，不导出全量）
- **双文件对比**: 按 (name, src_file, src_line) 匹配，计算差值、回退检测
- **树状对比**: 使用 Tracy **原生调用树**（`ZoneEvent::Child()`）逐层对比，精确而非重建
- **帧 ↔ 时间**: 用真实帧数据精确换算（不再是 60fps 估算）
- **消息 / 图表 / GPU**: 原生消息流、plot 时序、GPU/CPU zone

## 架构

```
.tracy ──(加载一次)──► tracy-mcp-query (C++, 常驻)
                          │  内存中的 Tracy Worker：
                          │   • SourceLocationZones（预计算 total/min/max/sumSq）
                          │   • 时间排序 zone 向量（二分查找时间段）
                          │   • 原生调用树 ZoneEvent::Child()
                          │   • 帧 / 消息 / plot / GPU
                          ▼
       行分隔 JSON (stdio)  ◄──►  Python MCP server (fastmcp)  ◄──► AI
```

- Python 侧只做协议转发，8 个 MCP 工具是后端方法的薄封装。
- 后端按 trace 路径缓存已加载的 Worker，一次会话内多次查询共享同一次加载。
- 协议细节见 [native/PROTOCOL.md](native/PROTOCOL.md)。

## 项目结构

```
native/                       C++ 数据/计算层（干活的地方）
├── src/main.cpp                Worker 加载缓存 + 8 个查询处理器 + stdio JSON 循环
├── CMakeLists.txt              最小化构建，仅链接 TracyServer
├── PROTOCOL.md                 stdio JSON 协议规范
└── third_party/                内置 nlohmann/json

src/tracy_mcp/                Python 协议/编排层（薄封装）
├── server.py                   FastMCP，定义 8 个 MCP 工具 + 启动入口
├── tools/*.py                  每个工具：组 params → 调后端 → 回填 hint
└── backend/query_client.py     拉起并喂养常驻后端进程，stdio 读写、按 id 关联响应
```

职责：**计算下沉到 C++/Worker，Python 只做编排**。加一个新工具 = `main.cpp` 写个 `h_*` 处理器
+ `dispatch` 注册一行 + `tools/` 一个薄转发。详见 [docs/USAGE.md](docs/USAGE.md)。

## 安装

### 1. 构建 C++ 查询后端

需要 CMake ≥ 3.16、C++20 编译器，首次配置需联网（CPM 拉取 capstone/zstd/PPQSort）。**不会**构建 GUI 依赖（ImGui/glfw/curl 等）。详见 [native/README.md](native/README.md)。

```bash
cd native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# → native/build/Release/tracy-mcp-query.exe
```

`TRACY_DIR` 指向 Tracy 源码（默认 `D:/ajin/opensourceProjs/tracy_0.13.1`），可用 `-DTRACY_DIR=...` 覆盖。

### 2. 安装 MCP 服务器

```bash
pip install -e ".[dev]"
```

后端可执行文件会被自动发现（`native/build/Release|Debug`、`native/build`、PATH），或用环境变量 `TRACY_MCP_QUERY_PATH` 指定。

### 在 Claude Desktop 中配置

```json
{
  "mcpServers": {
    "tracy": { "command": "tracy-mcp" }
  }
}
```

## 工具列表

| 工具 | 说明 |
|------|------|
| `tool_get_trace_info` | trace 元数据（程序名、捕获时间、时长、zone/帧/线程数） |
| `tool_get_zone_stats` | zone 聚合统计（含 self 独占时间，支持 cpu/gpu/all） |
| `tool_get_zone_timeline` | 时间段内的 zone 事件（raw / by_interval） |
| `tool_get_frame_stats` | 帧时间分布（p50/p95/p99）+ 最慢帧 + 超预算统计 |
| `tool_get_zone_outliers` | 某 zone 最慢的若干次调用（定位尖刺到具体帧） |
| `tool_compare_traces` | 两个 trace 的 zone 统计对比 + 回退检测 |
| `tool_compare_timelines` | 原生调用树的逐层对比（cpu/gpu） |
| `tool_get_messages` | 应用消息/日志 |
| `tool_get_plots` | plot 时序数据（支持降采样） |
| `tool_get_frame_range` | 帧号 → 精确时间范围 |

时间输入/输出统一以**相对 trace 起点**的秒/毫秒表示。

## 开发

```bash
# 客户端协议单元测试（无需后端）
pytest tests/test_client.py -v

# 端到端集成测试（需后端 + 真实 trace）
TRACY_TEST_FILE=/path/to/test.tracy pytest tests/test_tools.py -v
```

## 许可

MIT
