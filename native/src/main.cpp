// tracy-mcp-query: persistent query backend over Tracy's Worker.
//
// Two modes:
//   * no args      -> serve: read line-delimited JSON requests on stdin,
//                     write one JSON response per line on stdout.
//   * <trace.tracy> -> one-shot summary (smoke test).
//
// Protocol: see native/PROTOCOL.md. All result times are milliseconds unless
// the key ends in _seconds.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <regex>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"

using json = nlohmann::json;
using tracy::Worker;

// --------------------------------------------------------------------------
// Trace loading / caching: load each trace once, reuse across queries.
// --------------------------------------------------------------------------

struct LoadedTrace {
    std::unique_ptr<Worker> worker;
    int64_t mtime = 0;
    int64_t size = 0;
};

struct BackendError {
    std::string code;
    std::string message;
};

static std::map<std::string, LoadedTrace> g_traces;

static bool stat_file(const std::string& path, int64_t& mtime, int64_t& size) {
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) return false;
    mtime = (int64_t)st.st_mtime;
    size = (int64_t)st.st_size;
    return true;
}

// Returns a loaded Worker for `path`, loading (or reloading if changed) as
// needed. Throws BackendError on failure.
static Worker& get_worker(const std::string& path) {
    int64_t mtime = 0, size = 0;
    if (!stat_file(path, mtime, size)) {
        throw BackendError{"not_found", "trace file not found: " + path};
    }

    auto it = g_traces.find(path);
    if (it != g_traces.end()) {
        if (it->second.mtime == mtime && it->second.size == size) {
            return *it->second.worker;
        }
        g_traces.erase(it);  // changed on disk -> reload
    }

    std::unique_ptr<tracy::FileRead> f;
    try {
        f.reset(tracy::FileRead::Open(path.c_str()));
    } catch (const tracy::NotTracyDump&) {
        throw BackendError{"not_a_trace", "not a tracy trace: " + path};
    } catch (const std::exception& e) {
        throw BackendError{"load_failed", std::string("open failed: ") + e.what()};
    }
    if (!f) throw BackendError{"load_failed", "could not open: " + path};

    LoadedTrace lt;
    try {
        lt.worker = std::make_unique<Worker>(*f);
    } catch (const std::exception& e) {
        throw BackendError{"load_failed", std::string("worker init failed: ") + e.what()};
    }
    // Wait for the background statistics pass (SourceLocationZones).
    while (!lt.worker->AreSourceLocationZonesReady()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    lt.mtime = mtime;
    lt.size = size;

    auto& slot = g_traces[path];
    slot = std::move(lt);
    return *slot.worker;
}

// --------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------

static inline double ns_to_ms(int64_t ns) { return (double)ns / 1e6; }

static const char* zone_name(const Worker& w, int16_t srcloc) {
    auto& sl = w.GetSourceLocation(srcloc);
    return w.GetString(sl.name.active ? sl.name : sl.function);
}

static std::string thread_name(const Worker& w, uint16_t threadIdx) {
    uint64_t tid = w.DecompressThread(threadIdx);
    const char* n = w.GetThreadName(tid);
    return n ? std::string(n) : std::to_string(tid);
}

// Fold per-capture numeric ids in zone names so the same logical pass matches
// across traces: "mesh_commands_total#306" -> "mesh_commands_total#N",
// "pps_from:511" -> "pps_from:N". Only "#<digits>" and ":<digits>" are folded,
// so meaningful tokens like resolutions (3840x2103) or params (ShaderQuality=2)
// are left intact.
static std::string normalize_zone_name(const std::string& s) {
    static const std::regex re("([#:])[0-9]+");
    return std::regex_replace(s, re, "$1N");
}

// Population standard deviation (sample, n-1), matching csvexport's formula.
static double zone_std(double sumSq, int64_t total, size_t count) {
    if (count <= 1) return 0.0;
    const double avg = (double)total / (double)count;
    const double ss = sumSq - 2.0 * (double)total * avg + avg * avg * (double)count;
    return ss > 0 ? std::sqrt(ss / (double)(count - 1)) : 0.0;
}

static bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

static std::string param_str(const json& p, const char* key, const std::string& def = "") {
    if (p.contains(key) && p[key].is_string()) return p[key].get<std::string>();
    return def;
}
static double param_num(const json& p, const char* key, double def) {
    if (p.contains(key) && p[key].is_number()) return p[key].get<double>();
    return def;
}
static int64_t param_int(const json& p, const char* key, int64_t def) {
    if (p.contains(key) && p[key].is_number()) return p[key].get<int64_t>();
    return def;
}
static bool param_bool(const json& p, const char* key, bool def) {
    if (p.contains(key) && p[key].is_boolean()) return p[key].get<bool>();
    return def;
}

// --------------------------------------------------------------------------
// Aggregated zone stats (shared by zone_stats / compare_traces)
// --------------------------------------------------------------------------

struct ZoneAgg {
    std::string name, file;
    int line = 0;
    const char* zone_type = "cpu";
    int64_t total_ns = 0, min_ns = 0, max_ns = 0;
    size_t count = 0;
    double std_ns = 0;
    int merged_names = 1;  // how many distinct source names folded into this one (normalize_names)
    // Self (exclusive) time — only CPU SourceLocationZones tracks it.
    bool has_self = false;
    int64_t self_total_ns = 0, self_min_ns = 0, self_max_ns = 0;
};

// The per-srcloc zone-data struct is private in Worker, so we can't name it.
// Detect self-time support (CPU has selfTotal; GPU doesn't) via a trait.
template <typename T, typename = void>
struct has_self_time : std::false_type {};
template <typename T>
struct has_self_time<T, std::void_t<decltype(std::declval<T>().selfTotal)>> : std::true_type {};

template <typename ZonesMap>
static void collect_aggs(const Worker& w, const ZonesMap& zones, const char* type,
                         const std::string& filter, std::vector<ZoneAgg>& out) {
    for (auto& it : zones) {
        const auto& d = it.second;
        if (d.zones.size() == 0 || d.total == 0) continue;
        const char* nm = zone_name(w, it.first);
        if (!nm) nm = "";
        if (!filter.empty() && !icontains(nm, filter)) continue;
        auto& sl = w.GetSourceLocation(it.first);
        ZoneAgg a;
        a.name = nm;
        a.file = w.GetString(sl.file);
        a.line = (int)sl.line;
        a.zone_type = type;
        a.count = d.zones.size();
        a.total_ns = d.total;
        a.min_ns = d.min;
        a.max_ns = d.max;
        a.std_ns = zone_std(d.sumSq, d.total, d.zones.size());
        if constexpr (has_self_time<std::decay_t<decltype(d)>>::value) {
            a.has_self = true;
            a.self_total_ns = d.selfTotal;
            a.self_min_ns = d.selfMin;
            a.self_max_ns = d.selfMax;
        }
        out.push_back(std::move(a));
    }
}

static json agg_to_json(const ZoneAgg& a, double total_all_ns) {
    const double mean_ns = a.count ? (double)a.total_ns / (double)a.count : 0.0;
    json j{
        {"name", a.name},
        {"src_file", a.file},
        {"src_line", a.line},
        {"zone_type", a.zone_type},
        {"total_ms", ns_to_ms(a.total_ns)},
        {"total_percent", total_all_ns > 0 ? (a.total_ns / total_all_ns * 100.0) : 0.0},
        {"count", a.count},
        {"mean_ms", ns_to_ms((int64_t)mean_ns)},
        {"min_ms", ns_to_ms(a.min_ns)},
        {"max_ms", ns_to_ms(a.max_ns)},
        {"std_ms", a.std_ns / 1e6},
    };
    // Exclusive (self) time — distinguishes "slow itself" from "slow children".
    if (a.has_self) {
        const double self_mean = a.count ? (double)a.self_total_ns / (double)a.count : 0.0;
        j["self_total_ms"] = ns_to_ms(a.self_total_ns);
        j["self_mean_ms"] = ns_to_ms((int64_t)self_mean);
        j["self_max_ms"] = ns_to_ms(a.self_max_ns);
        j["self_percent"] = a.total_ns > 0 ? (double)a.self_total_ns / (double)a.total_ns * 100.0 : 0.0;
    } else {
        j["self_total_ms"] = nullptr;
    }
    return j;
}

// A Tracy zone-child / thread-timeline vector can be stored two ways: as an
// array of short_ptr<ZoneEvent>, OR — when is_magic() — as inline ZoneEvents
// by value (reinterpret as Vector<ZoneEvent>). Iterate both correctly.
template <typename F>
static void for_each_zone(const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones, F&& f) {
    if (zones.is_magic()) {
        auto& vec = *(const tracy::Vector<tracy::ZoneEvent>*)&zones;
        for (auto& v : vec) f(v);
    } else {
        for (auto& sp : zones) f(*sp);
    }
}

// --------------------------------------------------------------------------
// Warmup/cooldown frame trimming
//
// The first frames after Tracy connects and the last few before it
// disconnects are perturbed by profiling overhead. Statistics tools trim a
// default head/tail (skip_first_frames=10, skip_last_frames=4), resolving the
// kept frames to a time window. The trim is reported in every response so the
// caller perceives that warmup/cooldown were excluded.
// --------------------------------------------------------------------------

struct Trim {
    bool applied = false;
    int64_t t0 = INT64_MIN, t1 = INT64_MAX;
    int skip_first = 0, skip_last = 0;
    int frames_total = 0, frames_used = 0;
};

static Trim resolve_trim(Worker& w, const json& p, int def_first = 10, int def_last = 4) {
    Trim t;
    t.skip_first = std::max(0, (int)param_int(p, "skip_first_frames", def_first));
    t.skip_last = std::max(0, (int)param_int(p, "skip_last_frames", def_last));
    const tracy::FrameData* fd = w.GetFramesBase();
    if (fd) t.frames_total = (int)w.GetFrameCount(*fd);
    t.frames_used = t.frames_total;
    // No frames, nothing requested, or trim would remove everything -> full range.
    if (!fd || (t.skip_first == 0 && t.skip_last == 0) ||
        t.skip_first + t.skip_last >= t.frames_total) {
        return t;
    }
    const int lo = t.skip_first;
    const int hi = t.frames_total - 1 - t.skip_last;
    t.t0 = w.GetFrameBegin(*fd, lo);
    t.t1 = w.GetFrameEnd(*fd, hi);
    t.frames_used = hi - lo + 1;
    t.applied = true;
    return t;
}

static json trim_json(Worker& w, const Trim& t) {
    json j{
        {"applied", t.applied},
        {"skip_first_frames", t.skip_first},
        {"skip_last_frames", t.skip_last},
        {"frames_total", t.frames_total},
        {"frames_used", t.frames_used},
        {"note", t.applied
             ? "stats exclude warmup/cooldown frames (Tracy connect/disconnect overhead)"
             : "no trimming applied (set skip_first_frames/skip_last_frames)"},
    };
    if (t.applied) {
        const int64_t first = w.GetFirstTime();
        j["window_seconds"] = {ns_to_ms(t.t0 - first) / 1000.0, ns_to_ms(t.t1 - first) / 1000.0};
    }
    return j;
}

// Recompute CPU zone aggregates (incl. self time) from instances in [t0,t1].
static void collect_cpu_windowed(Worker& w, const std::string& filter,
                                 int64_t t0, int64_t t1, std::vector<ZoneAgg>& out) {
    for (auto& it : w.GetSourceLocationZones()) {
        const char* nm = zone_name(w, it.first);
        if (!nm) nm = "";
        if (!filter.empty() && !icontains(nm, filter)) continue;
        const auto& zs = it.second.zones;
        const auto* e = zs.end();
        const auto* lo = std::lower_bound(zs.begin(), e, t0,
            [](const auto& z, int64_t t) { return z.Zone()->Start() < t; });
        int64_t total = 0, mn = INT64_MAX, mx = INT64_MIN;
        int64_t selfT = 0, selfMn = INT64_MAX, selfMx = INT64_MIN;
        double sumSq = 0; size_t cnt = 0;
        for (const auto* zt = lo; zt != e; ++zt) {
            auto* z = zt->Zone();
            const int64_t s = z->Start();
            if (s >= t1) break;
            const int64_t dur = w.GetZoneEnd(*z) - s;
            int64_t self = dur;
            if (z->HasChildren())
                for_each_zone(w.GetZoneChildren(z->Child()), [&](const tracy::ZoneEvent& c) {
                    self -= (w.GetZoneEnd(c) - c.Start());
                });
            total += dur; mn = std::min(mn, dur); mx = std::max(mx, dur);
            sumSq += (double)dur * (double)dur; ++cnt;
            selfT += self; selfMn = std::min(selfMn, self); selfMx = std::max(selfMx, self);
        }
        if (cnt == 0) continue;
        auto& sl = w.GetSourceLocation(it.first);
        ZoneAgg a;
        a.name = nm; a.file = w.GetString(sl.file); a.line = (int)sl.line; a.zone_type = "cpu";
        a.count = cnt; a.total_ns = total; a.min_ns = mn; a.max_ns = mx;
        a.std_ns = zone_std(sumSq, total, cnt);
        a.has_self = true; a.self_total_ns = selfT; a.self_min_ns = selfMn; a.self_max_ns = selfMx;
        out.push_back(std::move(a));
    }
}

// Recompute GPU zone aggregates (no self time) from instances in [t0,t1].
static void collect_gpu_windowed(Worker& w, const std::string& filter,
                                 int64_t t0, int64_t t1, std::vector<ZoneAgg>& out) {
    for (auto& it : w.GetGpuSourceLocationZones()) {
        const char* nm = zone_name(w, it.first);
        if (!nm) nm = "";
        if (!filter.empty() && !icontains(nm, filter)) continue;
        const auto& zs = it.second.zones;
        const auto* e = zs.end();
        const auto* lo = std::lower_bound(zs.begin(), e, t0,
            [](const auto& z, int64_t t) { return z.Zone()->GpuStart() < t; });
        int64_t total = 0, mn = INT64_MAX, mx = INT64_MIN;
        double sumSq = 0; size_t cnt = 0;
        for (const auto* zt = lo; zt != e; ++zt) {
            auto* z = zt->Zone();
            const int64_t s = z->GpuStart();
            if (s < 0) continue;
            if (s >= t1) break;
            const int64_t dur = w.GetZoneEnd(*z) - s;
            total += dur; mn = std::min(mn, dur); mx = std::max(mx, dur);
            sumSq += (double)dur * (double)dur; ++cnt;
        }
        if (cnt == 0) continue;
        auto& sl = w.GetSourceLocation(it.first);
        ZoneAgg a;
        a.name = nm; a.file = w.GetString(sl.file); a.line = (int)sl.line; a.zone_type = "gpu";
        a.count = cnt; a.total_ns = total; a.min_ns = mn; a.max_ns = mx;
        a.std_ns = zone_std(sumSq, total, cnt);
        out.push_back(std::move(a));
    }
}

static void collect_aggs_trimmed(Worker& w, const std::string& zoneType,
                                 const std::string& filter, const Trim& trim,
                                 std::vector<ZoneAgg>& aggs) {
    if (zoneType == "cpu" || zoneType == "all")
        collect_cpu_windowed(w, filter, trim.t0, trim.t1, aggs);
    if (zoneType == "gpu" || zoneType == "all")
        collect_gpu_windowed(w, filter, trim.t0, trim.t1, aggs);
}

// --------------------------------------------------------------------------
// Handlers
// --------------------------------------------------------------------------

static json h_trace_info(const json& p) {
    const std::string path = param_str(p, "trace_file");
    Worker& w = get_worker(path);

    uint64_t cpuZones = 0;
    for (auto& it : w.GetSourceLocationZones()) cpuZones += it.second.zones.size();
    uint64_t gpuZones = 0;
    for (auto& it : w.GetGpuSourceLocationZones()) gpuZones += it.second.zones.size();

    size_t frameSets = w.GetFrames().size();
    size_t totalFrames = 0;
    if (frameSets > 0) totalFrames = w.GetFrameCount(*w.GetFramesBase());

    const double span = ns_to_ms(w.GetLastTime() - w.GetFirstTime()) / 1000.0;
    return json{
        {"file_name", path.substr(path.find_last_of("/\\") + 1)},
        {"capture_name", w.GetCaptureName()},
        {"span_seconds", span},
        {"total_zones", cpuZones},
        {"total_gpu_zones", gpuZones},
        {"total_frames", totalFrames},
        {"frame_sets", frameSets},
        {"thread_count", w.GetThreadData().size()},
        {"gpu_context_count", w.GetGpuData().size()},
    };
}

static json h_zone_stats(const json& p) {
    const std::string path = param_str(p, "trace_file");
    const std::string zoneType = param_str(p, "zone_type", "all");
    const std::string filter = param_str(p, "filter_name");
    const std::string sortBy = param_str(p, "sort_by", "total_time");
    int64_t topN = param_int(p, "top_n", 50);
    if (topN > 500) topN = 500;
    Worker& w = get_worker(path);

    // Default: trim warmup/cooldown frames and recompute over the kept window.
    // skip_first=skip_last=0 uses the (faster) whole-trace precomputed stats.
    const Trim trim = resolve_trim(w, p);
    std::vector<ZoneAgg> aggs;
    if (trim.applied) {
        collect_aggs_trimmed(w, zoneType, filter, trim, aggs);
    } else {
        if (zoneType == "cpu" || zoneType == "all")
            collect_aggs(w, w.GetSourceLocationZones(), "cpu", filter, aggs);
        if (zoneType == "gpu" || zoneType == "all")
            collect_aggs(w, w.GetGpuSourceLocationZones(), "gpu", filter, aggs);
    }

    double totalAll = 0;
    for (auto& a : aggs) totalAll += (double)a.total_ns;

    auto cmp = [&](const ZoneAgg& x, const ZoneAgg& y) {
        if (sortBy == "mean_time")
            return (double)x.total_ns / std::max<size_t>(1, x.count) >
                   (double)y.total_ns / std::max<size_t>(1, y.count);
        if (sortBy == "count") return x.count > y.count;
        if (sortBy == "max_time") return x.max_ns > y.max_ns;
        if (sortBy == "self_time") return x.self_total_ns > y.self_total_ns;
        return x.total_ns > y.total_ns;  // total_time
    };
    std::sort(aggs.begin(), aggs.end(), cmp);

    json zones = json::array();
    int64_t n = 0;
    double top5 = 0;
    for (auto& a : aggs) {
        if (n < 5) top5 += (double)a.total_ns;
        if (n++ >= topN) break;
        zones.push_back(agg_to_json(a, totalAll));
    }
    return json{
        {"zones", zones},
        {"total_zones_matched", (int64_t)aggs.size()},
        {"trim", trim_json(w, trim)},
        {"summary", {
            {"total_time_ms", ns_to_ms((int64_t)totalAll)},
            {"zones_with_stats", zones.size()},
            {"top_5_consume_percent", totalAll > 0 ? top5 / totalAll * 100.0 : 0.0},
        }},
    };
}

// Collect individual zone events in [t0,t1] ns from all source locations.
struct EventRow { int64_t start; int64_t dur; int16_t srcloc; uint16_t thread; bool gpu; };

static void collect_events(Worker& w, int64_t t0, int64_t t1, const std::string& nameFilter,
                           std::vector<EventRow>& out, size_t hardCap) {
    for (auto& it : w.GetSourceLocationZones()) {
        if (!nameFilter.empty()) {
            const char* nm = zone_name(w, it.first);
            if (!nm || !icontains(nm, nameFilter)) continue;
        }
        const auto& zs = it.second.zones;
        const auto* b = zs.begin();
        const auto* e = zs.end();
        // zones are sorted by Zone()->Start(); binary-search the window start.
        const auto* lo = std::lower_bound(b, e, t0,
            [](const auto& z, int64_t t) { return z.Zone()->Start() < t; });
        for (const auto* z = lo; z != e; ++z) {
            const int64_t s = z->Zone()->Start();
            if (s >= t1) break;
            const int64_t end = w.GetZoneEnd(*z->Zone());
            out.push_back({s, end - s, it.first, z->Thread(), false});
            if (out.size() >= hardCap) return;
        }
    }
}

// GPU variant. GpuStart()/GpuEnd() are already CPU-aligned at load (each context's
// timeDiff is applied by the worker), so they can be compared to the CPU-time window
// directly. Zones are sorted by GpuStart(), so the window start is binary-searched.
static void collect_gpu_events(Worker& w, int64_t t0, int64_t t1, const std::string& nameFilter,
                               std::vector<EventRow>& out, size_t hardCap) {
    for (auto& it : w.GetGpuSourceLocationZones()) {
        if (!nameFilter.empty()) {
            const char* nm = zone_name(w, it.first);
            if (!nm || !icontains(nm, nameFilter)) continue;
        }
        const auto& zs = it.second.zones;
        const auto* b = zs.begin();
        const auto* e = zs.end();
        const auto* lo = std::lower_bound(b, e, t0,
            [](const auto& z, int64_t t) { return z.Zone()->GpuStart() < t; });
        for (const auto* z = lo; z != e; ++z) {
            const int64_t s = z->Zone()->GpuStart();
            if (s < 0) continue;
            if (s >= t1) break;
            const int64_t end = w.GetZoneEnd(*z->Zone());
            out.push_back({s, end - s, it.first, z->Thread(), true});
            if (out.size() >= hardCap) return;
        }
    }
}

static json h_zone_timeline(const json& p) {
    const std::string path = param_str(p, "trace_file");
    const double startS = param_num(p, "start_second", 0);
    const double endS = param_num(p, "end_second", 0);
    const std::string nameFilter = param_str(p, "filter_name");
    const std::string threadFilter = param_str(p, "filter_thread");
    const std::string agg = param_str(p, "aggregation", "raw");
    const std::string zoneType = param_str(p, "zone_type", "all");
    const double intervalMs = param_num(p, "interval_ms", 16.67);
    int64_t limit = param_int(p, "limit", 500);
    if (limit > 5000) limit = 5000;
    int64_t cursor = param_int(p, "cursor", 0);
    Worker& w = get_worker(path);

    // Times are relative to the trace start (GetFirstTime), matching frame_range.
    const int64_t first = w.GetFirstTime();
    const int64_t t0 = first + (int64_t)(startS * 1e9);
    const int64_t t1 = first + (int64_t)(endS * 1e9);

    std::vector<EventRow> evs;
    if (zoneType == "cpu" || zoneType == "all")
        collect_events(w, t0, t1, nameFilter, evs, 2'000'000);
    if (zoneType == "gpu" || zoneType == "all")
        collect_gpu_events(w, t0, t1, nameFilter, evs, 2'000'000);

    // thread filter (by name) after collection
    if (!threadFilter.empty()) {
        std::vector<EventRow> f;
        for (auto& e : evs)
            if (icontains(thread_name(w, e.thread), threadFilter)) f.push_back(e);
        evs.swap(f);
    }

    std::sort(evs.begin(), evs.end(),
              [](const EventRow& a, const EventRow& b) { return a.start < b.start; });
    const int64_t totalInRange = (int64_t)evs.size();

    json out;
    out["time_range"] = {{"start_ms", startS * 1000.0}, {"end_ms", endS * 1000.0}};

    if (agg == "by_interval") {
        // bucket -> name -> {total, count}
        std::map<int64_t, std::map<std::string, std::pair<double, int64_t>>> buckets;
        const double iv = intervalMs > 0 ? intervalMs : 16.67;
        for (auto& e : evs) {
            double sms = ns_to_ms(e.start - first);
            int64_t bk = (int64_t)std::floor(sms / iv);
            auto& slot = buckets[bk][zone_name(w, e.srcloc)];
            slot.first += ns_to_ms(e.dur);
            slot.second += 1;
        }
        json intervals = json::array();
        int64_t n = 0;
        for (auto& [bk, names] : buckets) {
            if (n++ >= limit) break;
            std::vector<std::pair<std::string, std::pair<double, int64_t>>> v(names.begin(), names.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
            json tz = json::array();
            for (size_t i = 0; i < v.size() && i < 5; ++i)
                tz.push_back({{"name", v[i].first}, {"total_ms", v[i].second.first}, {"count", v[i].second.second}});
            intervals.push_back({{"start_ms", bk * iv}, {"end_ms", (bk + 1) * iv}, {"top_zones", tz}});
        }
        out["intervals"] = intervals;
        out["total_events_in_range"] = totalInRange;
        out["total_intervals"] = (int64_t)buckets.size();
        out["has_more"] = (int64_t)buckets.size() > limit;
        return out;
    }

    // raw
    json events = json::array();
    int64_t returned = 0;
    for (int64_t i = cursor; i < totalInRange && returned < limit; ++i, ++returned) {
        auto& e = evs[i];
        auto& sl = w.GetSourceLocation(e.srcloc);
        events.push_back({
            {"name", zone_name(w, e.srcloc)},
            {"thread", thread_name(w, e.thread)},
            {"zone_type", e.gpu ? "gpu" : "cpu"},
            {"src_file", w.GetString(sl.file)},
            {"src_line", (int)sl.line},
            {"start_ms", ns_to_ms(e.start - first)},
            {"duration_ms", ns_to_ms(e.dur)},
        });
    }
    const int64_t nextCursor = cursor + returned;
    const bool hasMore = nextCursor < totalInRange;
    out["events"] = events;
    out["total_events_in_range"] = totalInRange;
    out["has_more"] = hasMore;
    if (hasMore) out["next_cursor"] = nextCursor;
    return out;
}

static json h_frame_range(const json& p) {
    const std::string path = param_str(p, "trace_file");
    int64_t startFrame = param_int(p, "start_frame", 0);
    int64_t endFrame = param_int(p, "end_frame", 0);
    Worker& w = get_worker(path);
    if (w.GetFrames().empty())
        throw BackendError{"bad_request", "trace has no frame data"};

    const auto& fd = *w.GetFramesBase();
    const int64_t count = (int64_t)w.GetFrameCount(fd);
    auto clamp = [&](int64_t f) { return std::max<int64_t>(0, std::min(f, count - 1)); };
    const int64_t s = clamp(startFrame), e = clamp(endFrame);
    const int64_t beginNs = w.GetFrameBegin(fd, s);
    const int64_t endNs = w.GetFrameEnd(fd, e);
    const double s0 = ns_to_ms(beginNs - w.GetFirstTime()) / 1000.0;
    const double s1 = ns_to_ms(endNs - w.GetFirstTime()) / 1000.0;
    return json{
        {"frame_range", {{"start_frame", startFrame}, {"end_frame", endFrame}, {"frame_count", e - s + 1}}},
        {"time_range", {{"start_second", s0}, {"end_second", s1}, {"duration_seconds", s1 - s0}}},
        {"total_frames", count},
        {"estimated", false},
    };
}

static json h_messages(const json& p) {
    const std::string path = param_str(p, "trace_file");
    const double startS = param_num(p, "start_second", 0);
    const double endS = param_num(p, "end_second", 1e18);
    const std::string textFilter = param_str(p, "filter_text");
    int64_t limit = param_int(p, "limit", 200);
    if (limit > 1000) limit = 1000;
    Worker& w = get_worker(path);

    const int64_t first = w.GetFirstTime();
    const int64_t t0 = first + (int64_t)(startS * 1e9);
    const int64_t t1 = first + (int64_t)(endS * 1e9);
    json msgs = json::array();
    int64_t total = 0;
    for (auto& m : w.GetMessages()) {
        if (m->time < t0 || m->time >= t1) continue;
        const char* txt = w.GetString(m->ref);
        if (!textFilter.empty() && (!txt || !icontains(txt, textFilter))) continue;
        ++total;
        if ((int64_t)msgs.size() >= limit) continue;
        msgs.push_back({
            {"timestamp_ms", ns_to_ms(m->time - w.GetFirstTime())},
            {"thread", thread_name(w, m->thread)},
            {"text", txt ? txt : ""},
        });
    }
    return json{{"messages", msgs}, {"total_in_range", total}, {"has_more", total > limit}};
}

static json h_plots(const json& p) {
    const std::string path = param_str(p, "trace_file");
    const double startS = param_num(p, "start_second", 0);
    const double endS = param_num(p, "end_second", 1e18);
    const std::string plotName = param_str(p, "plot_name");
    int64_t downsample = std::max<int64_t>(1, param_int(p, "downsample", 1));
    int64_t maxPoints = param_int(p, "max_points", 500);
    if (maxPoints > 5000) maxPoints = 5000;
    Worker& w = get_worker(path);

    const int64_t first = w.GetFirstTime();
    const int64_t t0 = first + (int64_t)(startS * 1e9);
    const int64_t t1 = first + (int64_t)(endS * 1e9);

    json plots = json::array();
    json available = json::array();
    for (auto& plot : w.GetPlots()) {
        const char* nm = w.GetString(plot->name);
        available.push_back(nm ? nm : "");
        if (!plotName.empty() && (!nm || !icontains(nm, plotName))) continue;

        json pts = json::array();
        int64_t orig = 0, idx = 0;
        double mn = 1e300, mx = -1e300, sum = 0;
        for (auto& d : plot->data) {
            const int64_t tt = d.time.Val();
            if (tt < t0 || tt >= t1) continue;
            ++orig;
            if ((idx++ % downsample) != 0) continue;
            if ((int64_t)pts.size() >= maxPoints) continue;
            const double v = d.val;
            mn = std::min(mn, v); mx = std::max(mx, v); sum += v;
            pts.push_back({{"time_ms", ns_to_ms(tt - w.GetFirstTime())}, {"value", v}});
        }
        if (pts.empty() && plotName.empty()) continue;
        plots.push_back({
            {"name", nm ? nm : ""},
            {"points", pts},
            {"stats", {{"min", pts.empty() ? 0 : mn}, {"max", pts.empty() ? 0 : mx},
                       {"mean", pts.empty() ? 0 : sum / (double)pts.size()}, {"count", pts.size()}}},
            {"original_points", orig},
            {"downsampled", downsample > 1},
        });
    }
    json out{{"plots", plots}};
    if (plotName.empty()) out["available_plots"] = available;
    return out;
}

static json h_compare_traces(const json& p) {
    const std::string pathA = param_str(p, "trace_file_a");
    const std::string pathB = param_str(p, "trace_file_b");
    const std::string zoneType = param_str(p, "zone_type", "all");
    const std::string filter = param_str(p, "filter_name");
    const std::string sortBy = param_str(p, "sort_by", "delta_percent");
    int64_t topN = param_int(p, "top_n", 50);
    if (topN > 500) topN = 500;
    const double regPct = param_num(p, "regression_threshold_pct", 10.0);
    const bool normalize = param_bool(p, "normalize_names", true);

    Worker& wa = get_worker(pathA);
    Worker& wb = get_worker(pathB);
    // Each trace trims its own warmup/cooldown (same frame counts, own timing).
    const Trim trimA = resolve_trim(wa, p);
    const Trim trimB = resolve_trim(wb, p);

    auto build = [&](Worker& w, const Trim& trim) {
        std::vector<ZoneAgg> v;
        if (trim.applied) {
            collect_aggs_trimmed(w, zoneType, filter, trim, v);
        } else {
            if (zoneType == "cpu" || zoneType == "all") collect_aggs(w, w.GetSourceLocationZones(), "cpu", filter, v);
            if (zoneType == "gpu" || zoneType == "all") collect_aggs(w, w.GetGpuSourceLocationZones(), "gpu", filter, v);
        }
        std::map<std::string, ZoneAgg> m;
        for (auto& a : v) {
            std::string nm = normalize ? normalize_zone_name(a.name) : a.name;
            std::string fl = normalize ? normalize_zone_name(a.file) : a.file;
            std::string key = nm + "\0" + fl + ":" + std::to_string(a.line);
            auto it = m.find(key);
            if (it == m.end()) {
                ZoneAgg na = a;
                na.name = nm; na.file = fl;  // display the folded name
                m[key] = std::move(na);
            } else {
                // Fold another per-capture-id variant of the same pass into this row.
                auto& e = it->second;
                e.total_ns += a.total_ns;
                e.self_total_ns += a.self_total_ns;
                e.count += a.count;
                e.min_ns = std::min(e.min_ns, a.min_ns);
                e.max_ns = std::max(e.max_ns, a.max_ns);
                e.merged_names += 1;
            }
        }
        return m;
    };
    auto ma = build(wa, trimA), mb = build(wb, trimB);

    std::vector<std::string> keys;
    for (auto& kv : ma) keys.push_back(kv.first);
    for (auto& kv : mb) if (!ma.count(kv.first)) keys.push_back(kv.first);

    json cmps = json::array();
    double totalA = 0, totalB = 0;
    int regressions = 0, improvements = 0;
    std::vector<json> rows;
    for (auto& k : keys) {
        const ZoneAgg* a = ma.count(k) ? &ma[k] : nullptr;
        const ZoneAgg* b = mb.count(k) ? &mb[k] : nullptr;
        const double ta = a ? (double)a->total_ns : 0;
        const double tb = b ? (double)b->total_ns : 0;
        totalA += ta; totalB += tb;
        double deltaPct = ta > 0 ? (tb - ta) / ta * 100.0 : (tb > 0 ? 100.0 : 0.0);
        bool reg = deltaPct > regPct, imp = deltaPct < -regPct;
        if (reg) ++regressions; if (imp) ++improvements;
        const ZoneAgg* ref = a ? a : b;
        auto side = [](const ZoneAgg* z) -> json {
            if (!z) return json{{"total_ms", 0}, {"mean_ms", 0}, {"min_ms", 0}, {"max_ms", 0}, {"count", 0}};
            double mean = z->count ? (double)z->total_ns / z->count : 0;
            json j{{"total_ms", ns_to_ms(z->total_ns)}, {"mean_ms", ns_to_ms((int64_t)mean)},
                   {"min_ms", ns_to_ms(z->min_ns)}, {"max_ms", ns_to_ms(z->max_ns)}, {"count", z->count}};
            if (z->merged_names > 1) j["merged_names"] = z->merged_names;  // folded #N/:N variants
            return j;
        };
        rows.push_back({
            {"name", ref->name}, {"src_file", ref->file}, {"src_line", ref->line},
            {"zone_type", ref->zone_type},
            {"a", side(a)}, {"b", side(b)},
            {"delta", {{"total_ms", ns_to_ms((int64_t)(tb - ta))}, {"total_percent", deltaPct}}},
            {"regression", reg}, {"improvement", imp},
        });
    }
    std::sort(rows.begin(), rows.end(), [&](const json& x, const json& y) {
        if (sortBy == "total_time_a") return x["a"]["total_ms"].get<double>() > y["a"]["total_ms"].get<double>();
        if (sortBy == "total_time_b") return x["b"]["total_ms"].get<double>() > y["b"]["total_ms"].get<double>();
        return std::abs(x["delta"]["total_percent"].get<double>()) > std::abs(y["delta"]["total_percent"].get<double>());
    });
    for (int64_t i = 0; i < (int64_t)rows.size() && i < topN; ++i) cmps.push_back(rows[i]);

    json topReg = nullptr;
    for (auto& r : rows) if (r["regression"].get<bool>()) { topReg = {{"name", r["name"]}, {"delta_percent", r["delta"]["total_percent"]}}; break; }
    return json{
        {"comparisons", cmps},
        {"normalize_names", normalize},
        {"trim", {{"a", trim_json(wa, trimA)}, {"b", trim_json(wb, trimB)}}},
        {"summary", {
            {"regression_count", regressions}, {"improvement_count", improvements},
            {"total_a_time_ms", ns_to_ms((int64_t)totalA)}, {"total_b_time_ms", ns_to_ms((int64_t)totalB)},
            {"overall_delta_percent", totalA > 0 ? (totalB - totalA) / totalA * 100.0 : 0.0},
            {"top_regression", topReg},
        }},
    };
}

// ---- compare_timelines: native call tree via GetZoneChildren --------------

struct PathStat { int64_t total = 0, mn = INT64_MAX, mx = 0; std::vector<int64_t> durs; };

static void walk_zone(Worker& w, const tracy::ZoneEvent& z, int64_t t0, int64_t t1,
                      std::vector<std::string>& path, int depth, int maxDepth,
                      std::map<std::vector<std::string>, PathStat>& acc) {
    const int64_t s = z.Start();
    const int64_t e = w.GetZoneEnd(z);
    if (e <= t0 || s >= t1) return;  // outside window
    const char* nm = zone_name(w, z.SrcLoc());
    path.push_back(nm ? nm : "");
    if ((int)path.size() <= maxDepth) {
        auto& ps = acc[path];
        const int64_t dur = e - s;
        ps.total += dur; ps.mn = std::min(ps.mn, dur); ps.mx = std::max(ps.mx, dur);
        ps.durs.push_back(dur);
    }
    if (z.HasChildren() && depth + 1 < maxDepth) {
        for_each_zone(w.GetZoneChildren(z.Child()), [&](const tracy::ZoneEvent& c) {
            walk_zone(w, c, t0, t1, path, depth + 1, maxDepth, acc);
        });
    }
    path.pop_back();
}

static std::map<std::vector<std::string>, PathStat>
path_stats(Worker& w, int64_t t0, int64_t t1, int maxDepth) {
    std::map<std::vector<std::string>, PathStat> acc;
    std::vector<std::string> path;
    for (auto& td : w.GetThreadData()) {
        for_each_zone(td->timeline, [&](const tracy::ZoneEvent& z) {
            walk_zone(w, z, t0, t1, path, 0, maxDepth, acc);
        });
    }
    return acc;
}

// ---- GPU variant (GpuEvent tree) ------------------------------------------

template <typename F>
static void for_each_gpu(const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& zones, F&& f) {
    if (zones.is_magic()) {
        auto& vec = *(const tracy::Vector<tracy::GpuEvent>*)&zones;
        for (auto& v : vec) f(v);
    } else {
        for (auto& sp : zones) f(*sp);
    }
}

static void walk_gpu(Worker& w, const tracy::GpuEvent& z, int64_t t0, int64_t t1,
                     std::vector<std::string>& path, int depth, int maxDepth,
                     std::map<std::vector<std::string>, PathStat>& acc) {
    const int64_t s = z.GpuStart();
    const int64_t e = w.GetZoneEnd(z);
    if (s < 0 || e <= t0 || s >= t1) return;  // outside window / not-yet-resolved
    const char* nm = zone_name(w, z.SrcLoc());
    path.push_back(nm ? nm : "");
    if ((int)path.size() <= maxDepth) {
        auto& ps = acc[path];
        const int64_t dur = e - s;
        ps.total += dur; ps.mn = std::min(ps.mn, dur); ps.mx = std::max(ps.mx, dur);
        ps.durs.push_back(dur);
    }
    if (z.Child() >= 0 && depth + 1 < maxDepth) {
        for_each_gpu(w.GetGpuChildren(z.Child()), [&](const tracy::GpuEvent& c) {
            walk_gpu(w, c, t0, t1, path, depth + 1, maxDepth, acc);
        });
    }
    path.pop_back();
}

static std::map<std::vector<std::string>, PathStat>
path_stats_gpu(Worker& w, int64_t t0, int64_t t1, int maxDepth) {
    std::map<std::vector<std::string>, PathStat> acc;
    std::vector<std::string> path;
    for (auto& ctx : w.GetGpuData()) {
        for (auto& td : ctx->threadData) {
            for_each_gpu(td.second.timeline, [&](const tracy::GpuEvent& z) {
                walk_gpu(w, z, t0, t1, path, 0, maxDepth, acc);
            });
        }
    }
    return acc;
}

static json stat_json(const PathStat& s) {
    const size_t n = s.durs.size();
    double mean = n ? (double)s.total / n : 0;
    double median = 0;
    if (n) {
        std::vector<int64_t> d = s.durs;
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        median = (double)d[d.size() / 2];
    }
    return json{{"count", n}, {"total_ms", ns_to_ms(s.total)}, {"mean_ms", ns_to_ms((int64_t)mean)},
                {"min_ms", ns_to_ms(n ? s.mn : 0)}, {"max_ms", ns_to_ms(s.mx)}, {"median_ms", ns_to_ms((int64_t)median)}};
}

static json h_compare_timelines(const json& p) {
    const std::string pathA = param_str(p, "trace_file_a");
    const std::string pathB = param_str(p, "trace_file_b");
    const double startS = param_num(p, "start_second", 0);
    const double endS = param_num(p, "end_second", 0);
    const std::string zoneType = param_str(p, "zone_type", "cpu");
    const bool gpu = (zoneType == "gpu");
    int maxDepth = (int)param_int(p, "max_depth", 5);
    if (maxDepth > 12) maxDepth = 12;
    int64_t limit = param_int(p, "limit", 300);
    if (limit > 2000) limit = 2000;

    Worker& wa = get_worker(pathA);
    Worker& wb = get_worker(pathB);
    const int64_t t0a = wa.GetFirstTime() + (int64_t)(startS * 1e9);
    const int64_t t1a = wa.GetFirstTime() + (int64_t)(endS * 1e9);
    const int64_t t0b = wb.GetFirstTime() + (int64_t)(startS * 1e9);
    const int64_t t1b = wb.GetFirstTime() + (int64_t)(endS * 1e9);

    auto pa = gpu ? path_stats_gpu(wa, t0a, t1a, maxDepth) : path_stats(wa, t0a, t1a, maxDepth);
    auto pb = gpu ? path_stats_gpu(wb, t0b, t1b, maxDepth) : path_stats(wb, t0b, t1b, maxDepth);

    using Path = std::vector<std::string>;
    auto deltaPct = [](double a, double b) { return a > 0 ? (b - a) / a * 100.0 : (b > 0 ? 100.0 : 0.0); };
    auto totalOf = [&](const Path& p, bool useB) -> double {
        auto& m = useB ? pb : pa;
        auto it = m.find(p);
        return it != m.end() ? (double)it->second.total : 0.0;
    };
    static const PathStat EMPTY{0, 0, 0, {}};
    auto statOf = [&](const Path& p, bool useB) -> const PathStat& {
        auto& m = useB ? pb : pa;
        auto it = m.find(p);
        return it != m.end() ? it->second : EMPTY;
    };

    // Adjacency: parent path -> its direct child paths (union over A and B).
    std::map<Path, std::vector<Path>> childrenOf;
    {
        std::vector<Path> all;
        for (auto& kv : pa) all.push_back(kv.first);
        for (auto& kv : pb) if (!pa.count(kv.first)) all.push_back(kv.first);
        for (auto& path : all) {
            Path parent(path.begin(), path.end() - 1);
            childrenOf[parent].push_back(path);
        }
        // Most-changed first, so truncation keeps the interesting subtrees.
        for (auto& kv : childrenOf) {
            std::sort(kv.second.begin(), kv.second.end(), [&](const Path& x, const Path& y) {
                return std::abs(deltaPct(totalOf(x, false), totalOf(x, true))) >
                       std::abs(deltaPct(totalOf(y, false), totalOf(y, true)));
            });
        }
    }

    int64_t nodeCount = 0;
    bool truncated = false;
    // Recursively emit json (no pointers into json arrays -> no invalidation).
    std::function<json(const Path&, int)> emit = [&](const Path& path, int depth) -> json {
        json node{
            {"name", path.back()},
            {"a", stat_json(statOf(path, false))},
            {"b", stat_json(statOf(path, true))},
            {"delta_percent", deltaPct(totalOf(path, false), totalOf(path, true))},
            {"children", json::array()},
        };
        if (depth < maxDepth) {
            auto it = childrenOf.find(path);
            if (it != childrenOf.end()) {
                json kids = json::array();
                for (auto& cp : it->second) {
                    if (nodeCount >= limit) { truncated = true; break; }
                    ++nodeCount;
                    kids.push_back(emit(cp, depth + 1));
                }
                node["children"] = std::move(kids);
            }
        }
        return node;
    };

    json rootKids = json::array();
    auto rit = childrenOf.find(Path{});
    if (rit != childrenOf.end()) {
        for (auto& cp : rit->second) {
            if (nodeCount >= limit) { truncated = true; break; }
            ++nodeCount;
            rootKids.push_back(emit(cp, 1));
        }
    }
    json root{{"name", "<root>"}, {"children", std::move(rootKids)}};
    return json{{"tree", root}, {"total_nodes", nodeCount}, {"truncated", truncated}};
}

// ---- frame statistics + slowest-frame finder ------------------------------

static int frame_index_at(Worker& w, const tracy::FrameData* fd, int64_t t) {
    if (!fd) return -1;
    auto r = w.GetFrameRange(*fd, t, t);
    return r.first;
}

static json h_frame_stats(const json& p) {
    const std::string path = param_str(p, "trace_file");
    int64_t topSlow = param_int(p, "top_slowest", 10);
    if (topSlow > 100) topSlow = 100;
    const bool hasBudget = p.contains("budget_ms") && p["budget_ms"].is_number();
    const double budgetMs = param_num(p, "budget_ms", 0);
    Worker& w = get_worker(path);

    const tracy::FrameData* fdp = w.GetFramesBase();
    if (!fdp) throw BackendError{"bad_request", "trace has no frame data"};
    const auto& fd = *fdp;
    const int64_t first = w.GetFirstTime();
    const size_t n = w.GetFrameCount(fd);

    // Default: exclude warmup/cooldown frames from the distribution.
    const Trim trim = resolve_trim(w, p);
    const size_t lo = trim.applied ? (size_t)trim.skip_first : 0;
    const size_t hi = trim.applied ? (size_t)(n - 1 - trim.skip_last) : (n > 0 ? n - 1 : 0);

    struct Fr { int64_t dur; size_t idx; int64_t begin; };
    std::vector<Fr> frames;
    std::vector<int64_t> times;
    frames.reserve(n); times.reserve(n);
    const int64_t budgetNs = (int64_t)(budgetMs * 1e6);
    int64_t over = 0;
    for (size_t i = lo; i <= hi && i < n; i++) {
        const int64_t t = w.GetFrameTime(fd, i);
        if (t <= 0) continue;  // skip incomplete frames
        times.push_back(t);
        frames.push_back({t, i, w.GetFrameBegin(fd, i)});
        if (hasBudget && t > budgetNs) ++over;
    }
    if (times.empty()) throw BackendError{"bad_request", "no measurable frames"};

    std::vector<int64_t> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    int64_t sum = 0; for (auto t : times) sum += t;
    const double meanMs = ns_to_ms(sum / (int64_t)times.size());
    auto pct = [&](double q) {
        const double pos = std::clamp(q / 100.0 * (sorted.size() - 1), 0.0, (double)(sorted.size() - 1));
        return ns_to_ms(sorted[(size_t)pos]);
    };

    const size_t k = std::min((size_t)topSlow, frames.size());
    std::partial_sort(frames.begin(), frames.begin() + k, frames.end(),
                      [](const Fr& a, const Fr& b) { return a.dur > b.dur; });
    json slowest = json::array();
    for (size_t i = 0; i < k; i++) {
        slowest.push_back({
            {"frame", (int64_t)frames[i].idx},
            {"start_second", ns_to_ms(frames[i].begin - first) / 1000.0},
            {"duration_ms", ns_to_ms(frames[i].dur)},
        });
    }

    json out{
        {"frame_count", (int64_t)times.size()},
        {"fps_mean", meanMs > 0 ? 1000.0 / meanMs : 0.0},
        {"frame_ms", {{"mean", meanMs}, {"p50", pct(50)}, {"p95", pct(95)}, {"p99", pct(99)},
                      {"min", ns_to_ms(sorted.front())}, {"max", ns_to_ms(sorted.back())}}},
        {"slowest", slowest},
        {"trim", trim_json(w, trim)},
    };
    if (hasBudget) {
        out["budget_ms"] = budgetMs;
        out["frames_over_budget"] = over;
        out["percent_over_budget"] = (double)over / (double)times.size() * 100.0;
    }
    return out;
}

// ---- worst instances of a zone (outliers) ---------------------------------

static json h_zone_outliers(const json& p) {
    const std::string path = param_str(p, "trace_file");
    const std::string filter = param_str(p, "filter_name");
    const std::string zoneType = param_str(p, "zone_type", "cpu");
    int64_t topN = param_int(p, "top_n", 10);
    if (topN > 200) topN = 200;
    if (topN < 1) topN = 1;
    if (filter.empty()) throw BackendError{"bad_request", "filter_name is required for zone_outliers"};
    Worker& w = get_worker(path);

    const int64_t first = w.GetFirstTime();
    // Explicit start/end takes precedence; otherwise default warmup/cooldown trim.
    const bool explicitWin = p.contains("start_second") && p.contains("end_second");
    const Trim trim = explicitWin ? Trim{} : resolve_trim(w, p);
    int64_t t0, t1;
    bool hasWin;
    if (explicitWin) {
        hasWin = true;
        t0 = first + (int64_t)(param_num(p, "start_second", 0) * 1e9);
        t1 = first + (int64_t)(param_num(p, "end_second", 0) * 1e9);
    } else {
        hasWin = trim.applied;
        t0 = trim.t0; t1 = trim.t1;
    }

    struct Inst { int64_t dur, start; int16_t srcloc; uint16_t thread; };
    auto cmp = [](const Inst& a, const Inst& b) { return a.dur > b.dur; };  // min-heap on dur
    std::priority_queue<Inst, std::vector<Inst>, decltype(cmp)> heap(cmp);
    int matched = 0;
    int64_t total = 0;

    auto consider = [&](int64_t s, int64_t e, int16_t srcloc, uint16_t thread) {
        if (hasWin && (s < t0 || s >= t1)) return;
        ++total;
        Inst inst{e - s, s, srcloc, thread};
        if ((int64_t)heap.size() < topN) heap.push(inst);
        else if (heap.top().dur < inst.dur) { heap.pop(); heap.push(inst); }
    };

    if (zoneType == "gpu") {
        for (auto& it : w.GetGpuSourceLocationZones()) {
            const char* nm = zone_name(w, it.first);
            if (!nm || !icontains(nm, filter)) continue;
            ++matched;
            for (const auto& ztd : it.second.zones) {
                auto* z = ztd.Zone();
                const int64_t s = z->GpuStart();
                if (s < 0) continue;
                consider(s, w.GetZoneEnd(*z), it.first, ztd.Thread());
            }
        }
    } else {
        for (auto& it : w.GetSourceLocationZones()) {
            const char* nm = zone_name(w, it.first);
            if (!nm || !icontains(nm, filter)) continue;
            ++matched;
            for (const auto& ztd : it.second.zones) {
                auto* z = ztd.Zone();
                consider(z->Start(), w.GetZoneEnd(*z), it.first, ztd.Thread());
            }
        }
    }

    std::vector<Inst> v;
    while (!heap.empty()) { v.push_back(heap.top()); heap.pop(); }
    std::reverse(v.begin(), v.end());  // slowest first

    const tracy::FrameData* fd = w.GetFramesBase();
    json outliers = json::array();
    for (auto& inst : v) {
        auto& sl = w.GetSourceLocation(inst.srcloc);
        outliers.push_back({
            {"duration_ms", ns_to_ms(inst.dur)},
            {"start_second", ns_to_ms(inst.start - first) / 1000.0},
            {"frame", frame_index_at(w, fd, inst.start)},
            {"thread", thread_name(w, inst.thread)},
            {"name", zone_name(w, inst.srcloc)},
            {"src_file", w.GetString(sl.file)},
            {"src_line", (int)sl.line},
        });
    }
    json out{
        {"zone", filter},
        {"matched_locations", matched},
        {"total_instances", total},
        {"outliers", outliers},
    };
    if (!explicitWin) out["trim"] = trim_json(w, trim);
    return out;
}

// --------------------------------------------------------------------------
// Dispatch + serve loop
// --------------------------------------------------------------------------

static json dispatch(const std::string& method, const json& params) {
    if (method == "trace_info") return h_trace_info(params);
    if (method == "zone_stats") return h_zone_stats(params);
    if (method == "zone_timeline") return h_zone_timeline(params);
    if (method == "frame_range") return h_frame_range(params);
    if (method == "frame_stats") return h_frame_stats(params);
    if (method == "zone_outliers") return h_zone_outliers(params);
    if (method == "messages") return h_messages(params);
    if (method == "plots") return h_plots(params);
    if (method == "compare_traces") return h_compare_traces(params);
    if (method == "compare_timelines") return h_compare_timelines(params);
    throw BackendError{"unknown_method", "unknown method: " + method};
}

static int serve() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json resp;
        int64_t id = 0;
        try {
            json req = json::parse(line);
            id = req.value("id", (int64_t)0);
            const std::string method = req.value("method", std::string());
            const json params = req.contains("params") ? req["params"] : json::object();
            json result = dispatch(method, params);
            resp = {{"id", id}, {"ok", true}, {"result", result}};
        } catch (const BackendError& e) {
            resp = {{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.message}}}};
        } catch (const json::exception& e) {
            resp = {{"id", id}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", e.what()}}}};
        } catch (const std::exception& e) {
            resp = {{"id", id}, {"ok", false}, {"error", {{"code", "internal"}, {"message", e.what()}}}};
        }
        std::cout << resp.dump() << "\n";
        std::cout.flush();
    }
    return 0;
}

static int one_shot(const char* path) {
    try {
        json r = h_trace_info(json{{"trace_file", path}});
        std::cout << r.dump(2) << "\n";
    } catch (const BackendError& e) {
        fprintf(stderr, "error %s: %s\n", e.code.c_str(), e.message.c_str());
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2) return one_shot(argv[1]);
    return serve();
}
