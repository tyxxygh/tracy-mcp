"""Integration tests for the Tracy MCP tools end-to-end through the native backend.

Requires:
  * the built native backend (auto-discovered, or TRACY_MCP_QUERY_PATH)
  * a real .tracy file via TRACY_TEST_FILE
"""

import os

import pytest


def _trace():
    f = os.environ.get("TRACY_TEST_FILE")
    if not f or not os.path.exists(f):
        pytest.skip("set TRACY_TEST_FILE to a .tracy file for integration tests")
    return f


@pytest.mark.integration
class TestTraceInfo:
    def test_returns_metadata(self):
        from tracy_mcp.tools.info import get_trace_info
        r = get_trace_info(_trace())
        assert "error" not in r
        assert r["span_seconds"] > 0
        assert r["total_zones"] > 0
        assert r["thread_count"] >= 1
        assert isinstance(r["capture_name"], str)


@pytest.mark.integration
class TestZoneStats:
    def test_returns_sorted_zones(self):
        from tracy_mcp.tools.stats import get_zone_stats
        r = get_zone_stats(_trace(), top_n=10, sort_by="total_time")
        assert r["zones"], "expected at least one zone"
        z = r["zones"][0]
        assert {"name", "total_ms", "count", "mean_ms", "std_ms"} <= set(z)
        totals = [x["total_ms"] for x in r["zones"]]
        assert totals == sorted(totals, reverse=True)


@pytest.mark.integration
class TestZoneSelfTime:
    def test_cpu_zones_have_self_time(self):
        from tracy_mcp.tools.stats import get_zone_stats
        r = get_zone_stats(_trace(), zone_type="cpu", top_n=5, sort_by="self_time")
        assert r["zones"]
        z = r["zones"][0]
        assert z["self_total_ms"] is not None
        assert 0.0 <= z["self_percent"] <= 100.0
        assert z["self_total_ms"] <= z["total_ms"] + 1e-6  # self <= total
        # sorted by self_total descending
        selfs = [x["self_total_ms"] for x in r["zones"]]
        assert selfs == sorted(selfs, reverse=True)


@pytest.mark.integration
class TestFrameStats:
    def test_distribution_and_slowest(self):
        from tracy_mcp.tools.frames import get_frame_stats
        r = get_frame_stats(_trace(), top_slowest=5, budget_ms=16.67)
        fm = r["frame_ms"]
        assert fm["min"] <= fm["p50"] <= fm["p95"] <= fm["p99"] <= fm["max"]
        assert r["fps_mean"] > 0
        assert len(r["slowest"]) <= 5
        # slowest sorted descending by duration
        durs = [s["duration_ms"] for s in r["slowest"]]
        assert durs == sorted(durs, reverse=True)
        assert r["frames_over_budget"] >= 0


@pytest.mark.integration
class TestZoneOutliers:
    def test_worst_instances(self):
        from tracy_mcp.tools.stats import get_zone_stats, get_zone_outliers
        # pick a real zone name from stats
        top = get_zone_stats(_trace(), zone_type="cpu", top_n=1)["zones"][0]["name"]
        r = get_zone_outliers(_trace(), filter_name=top, top_n=5)
        assert r["outliers"]
        durs = [o["duration_ms"] for o in r["outliers"]]
        assert durs == sorted(durs, reverse=True)
        o = r["outliers"][0]
        assert {"duration_ms", "start_second", "frame", "thread"} <= set(o)


@pytest.mark.integration
class TestWarmupTrim:
    def test_frame_stats_trims_by_default(self):
        from tracy_mcp.tools.frames import get_frame_stats
        trimmed = get_frame_stats(_trace())
        full = get_frame_stats(_trace(), skip_first_frames=0, skip_last_frames=0)
        # default trim is reported and excludes frames
        assert trimmed["trim"]["applied"] is True
        assert trimmed["trim"]["skip_first_frames"] == 10
        assert trimmed["trim"]["skip_last_frames"] == 4
        assert full["trim"]["applied"] is False
        assert trimmed["frame_count"] < full["frame_count"]
        # excluding the warmup frames cannot make the distribution worse
        assert trimmed["frame_ms"]["max"] <= full["frame_ms"]["max"]
        # no negative start times once warmup frames are trimmed
        assert all(s["start_second"] >= 0 for s in trimmed["slowest"])

    def test_zone_stats_reports_trim(self):
        from tracy_mcp.tools.stats import get_zone_stats
        r = get_zone_stats(_trace(), zone_type="cpu", top_n=3)
        assert r["trim"]["applied"] is True
        full = get_zone_stats(_trace(), zone_type="cpu", top_n=3,
                              skip_first_frames=0, skip_last_frames=0)
        assert full["trim"]["applied"] is False


@pytest.mark.integration
class TestTimeline:
    def test_window_events_are_relative(self):
        from tracy_mcp.tools.timeline import get_zone_timeline
        r = get_zone_timeline(_trace(), start_second=1.0, end_second=1.05, limit=10)
        assert "events" in r
        for e in r["events"]:
            assert 1000.0 <= e["start_ms"] <= 1050.0  # trace-relative ms


@pytest.mark.integration
class TestFrameRange:
    def test_exact_frame_times(self):
        from tracy_mcp.server import tool_get_frame_range
        r = tool_get_frame_range(_trace(), start_frame=10, end_frame=20)
        assert r["estimated"] is False
        assert r["time_range"]["end_second"] >= r["time_range"]["start_second"]


@pytest.mark.integration
class TestCompare:
    def test_same_file_zero_delta(self):
        from tracy_mcp.tools.compare import compare_traces
        t = _trace()
        r = compare_traces(t, t, top_n=10)
        assert r["summary"]["overall_delta_percent"] == pytest.approx(0.0, abs=0.01)

    def test_timeline_tree_native(self):
        from tracy_mcp.tools.compare import compare_timelines
        t = _trace()
        r = compare_timelines(t, t, start_second=1.0, end_second=1.5, max_depth=3, limit=20)
        assert "tree" in r and r["tree"]["name"] == "<root>"

        def check(n):
            if n["name"] != "<root>":
                assert n.get("delta_percent", 0) == pytest.approx(0.0, abs=0.01)
            for c in n.get("children", []):
                check(c)
        check(r["tree"])
