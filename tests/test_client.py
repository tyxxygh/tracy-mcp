"""Unit tests for the query client protocol (no real backend needed)."""

import json

import pytest

from tracy_mcp.backend.query_client import QueryClient, QueryBackendError


class FakeProc:
    """Minimal stand-in for a Popen with stdin/stdout pipes."""

    def __init__(self, responder):
        self._responder = responder
        self._out_lines: list[str] = []
        self.stdin = self
        self.stdout = self
        self._written = ""

    # stdin.write/flush
    def write(self, data):
        self._written += data
        if data.endswith("\n"):
            req = json.loads(self._written.strip())
            self._written = ""
            # responder may return several lines; queue each separately.
            for ln in self._responder(req).split("\n"):
                self._out_lines.append(ln)

    def flush(self):
        pass

    # stdout.readline
    def readline(self):
        if self._out_lines:
            return self._out_lines.pop(0) + "\n"
        return ""  # EOF

    def poll(self):
        return None


def make_client(responder) -> QueryClient:
    c = QueryClient()
    c._proc = FakeProc(responder)  # inject fake process
    return c


def test_success_response():
    def responder(req):
        assert req["method"] == "zone_stats"
        return json.dumps({"id": req["id"], "ok": True, "result": {"zones": [1, 2]}})

    c = make_client(responder)
    out = c.call("zone_stats", {"trace_file": "x"})
    assert out == {"zones": [1, 2]}


def test_error_response_raises():
    def responder(req):
        return json.dumps({"id": req["id"], "ok": False,
                           "error": {"code": "not_found", "message": "nope"}})

    c = make_client(responder)
    with pytest.raises(QueryBackendError) as ei:
        c.call("trace_info", {"trace_file": "missing"})
    assert ei.value.code == "not_found"


def test_id_correlation_skips_stale_lines():
    # Backend emits an unrelated line first, then the real answer.
    state = {"n": 0}

    def responder(req):
        # Return two lines: a stale id, then the correct one.
        stale = json.dumps({"id": 999, "ok": True, "result": {"stale": True}})
        good = json.dumps({"id": req["id"], "ok": True, "result": {"ok": 1}})
        return stale + "\n" + good

    c = make_client(responder)
    out = c.call("trace_info", {"trace_file": "x"})
    assert out == {"ok": 1}


def test_query_helper_converts_errors(monkeypatch):
    import tracy_mcp.backend.query_client as qc

    def boom(method, params, timeout=None):
        raise QueryBackendError("backend_not_found", "no exe")

    monkeypatch.setattr(qc, "get_client", lambda: type("C", (), {"call": staticmethod(boom)})())
    out = qc.query("trace_info", {"trace_file": "x"})
    assert out == {"error": "backend_not_found", "message": "no exe"}
