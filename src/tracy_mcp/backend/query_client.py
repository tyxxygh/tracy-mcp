"""Client for the persistent tracy-mcp-query C++ backend.

The backend loads `.tracy` traces into Tracy's Worker once and answers queries
over line-delimited JSON on stdio. This client spawns it lazily, serializes
requests (single stdin/stdout pipe), and correlates responses by id.
"""

import json
import logging
import os
import shutil
import subprocess
import threading
from pathlib import Path

logger = logging.getLogger(__name__)


class QueryBackendError(Exception):
    """A structured error returned by the backend, or a transport failure."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


# Candidate locations for the built backend executable.
def _candidate_paths() -> list[str]:
    exe = "tracy-mcp-query.exe" if os.name == "nt" else "tracy-mcp-query"
    here = Path(__file__).resolve()
    native = here.parents[3] / "native"  # repo_root/native
    return [
        # Visual Studio multi-config output
        str(native / "build" / "Release" / exe),
        str(native / "build" / "Debug" / exe),
        # Single-config (Ninja/Make) output
        str(native / "build" / exe),
    ]


def _find_backend() -> str:
    env = os.environ.get("TRACY_MCP_QUERY_PATH")
    if env and os.path.isfile(env):
        return env
    for p in _candidate_paths():
        if os.path.isfile(p):
            return p
    found = shutil.which("tracy-mcp-query")
    if found:
        return found
    raise QueryBackendError(
        "backend_not_found",
        "tracy-mcp-query executable not found. Build native/ (see native/README.md) "
        "or set TRACY_MCP_QUERY_PATH.",
    )


class QueryClient:
    """Manages the backend subprocess and request/response correlation."""

    def __init__(self, exe_path: str | None = None, default_timeout: float = 120.0):
        self._exe_path = exe_path
        self._default_timeout = default_timeout
        self._proc: subprocess.Popen | None = None
        self._lock = threading.Lock()
        self._next_id = 1

    def _ensure_started(self) -> subprocess.Popen:
        if self._proc is not None and self._proc.poll() is None:
            return self._proc
        exe = self._exe_path or _find_backend()
        logger.info("Starting tracy-mcp-query backend: %s", exe)
        self._proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,  # backend logs to stderr; ignore here
            text=True,
            encoding="utf-8",
            bufsize=1,  # line-buffered
        )
        return self._proc

    def call(self, method: str, params: dict, timeout: float | None = None) -> dict:
        """Send a request and return the `result` dict, or raise QueryBackendError."""
        with self._lock:
            proc = self._ensure_started()
            req_id = self._next_id
            self._next_id += 1

            request = json.dumps({"id": req_id, "method": method, "params": params})
            try:
                assert proc.stdin is not None and proc.stdout is not None
                proc.stdin.write(request + "\n")
                proc.stdin.flush()
            except (BrokenPipeError, OSError) as e:
                self._kill()
                raise QueryBackendError("transport", f"failed to send request: {e}")

            # Read until we get the line matching our id (backend answers in order,
            # but tolerate interleaved log/blank lines just in case).
            while True:
                line = proc.stdout.readline()
                if line == "":
                    self._kill()
                    raise QueryBackendError(
                        "transport", "backend closed the connection unexpectedly"
                    )
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    # Not protocol output; skip.
                    continue
                if msg.get("id") != req_id:
                    continue
                if msg.get("ok"):
                    return msg.get("result", {})
                err = msg.get("error", {})
                raise QueryBackendError(
                    err.get("code", "internal"), err.get("message", "unknown error")
                )

    def _kill(self) -> None:
        if self._proc is not None:
            try:
                self._proc.kill()
            except Exception:
                pass
            self._proc = None

    def shutdown(self) -> None:
        with self._lock:
            if self._proc is not None and self._proc.poll() is None:
                try:
                    self._proc.stdin.close()  # type: ignore[union-attr]
                except Exception:
                    pass
            self._kill()


# Process-wide singleton.
_client: QueryClient | None = None
_client_lock = threading.Lock()


def get_client() -> QueryClient:
    global _client
    with _client_lock:
        if _client is None:
            _client = QueryClient()
        return _client


def query(method: str, params: dict, timeout: float | None = None) -> dict:
    """Call a backend method, returning the result dict, or a structured
    ``{"error": code, "message": ...}`` dict on failure (so MCP tools never
    raise out to the transport)."""
    try:
        return get_client().call(method, params, timeout=timeout)
    except QueryBackendError as e:
        return {"error": e.code, "message": e.message}
    except Exception as e:  # pragma: no cover - defensive
        logger.exception("query(%s) failed", method)
        return {"error": "internal_error", "message": str(e)}
