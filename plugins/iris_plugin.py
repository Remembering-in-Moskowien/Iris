#!/usr/bin/env python3
"""Iris T3 process-plugin SDK.

A plugin is a normal process that speaks one JSON object per line on
stdin/stdout.  The host sends `describe` once after start, then `invoke`
for each call.  Anything written to stderr is treated as a log line.

Minimal plugin:

    from iris_plugin import Plugin, plugin, serve

    @plugin(id="acme.check.cps", kind="check", version="0.1.0")
    class CpsCheck(Plugin):
        def invoke(self, input_data, params):
            return {"ok": True, "input": input_data}

    if __name__ == "__main__":
        serve(CpsCheck)

The `input_data` and `params` values are exactly the JSON objects passed by
`iris plugin invoke --input ... --params ...`.
"""

from __future__ import annotations

import json
import sys
from typing import Any


class Plugin:
    """Base class for a process plugin."""

    id = "example.plugin"
    kind = "check"
    version = "0.1.0"
    api_version = 1

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "kind": self.kind,
            "version": self.version,
            "apiVersion": self.api_version,
        }

    def invoke(self, input_data: Any, params: dict[str, Any]) -> Any:
        raise NotImplementedError("implement invoke() or use a plain @plugin class")


def plugin(id: str | None = None, kind: str = "check", version: str = "0.1.0"):
    """Class decorator that fills in the plugin identity."""

    def decorate(cls):
        if id is not None:
            cls.id = id
        cls.kind = kind
        cls.version = version
        return cls

    return decorate


def _send(document: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(document, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(level: str, message: str, **extra: Any) -> None:
    """Send a log notification to the host (not a response)."""
    params = {"level": level, "message": message}
    params.update(extra)
    _send({"method": "log", "params": params})


def serve(plugin_class: type[Plugin] | Plugin) -> int:
    """Run the stdio JSON-lines loop until EOF or a `shutdown` request."""
    instance = plugin_class() if isinstance(plugin_class, type) else plugin_class
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            log("warn", "ignoring non-JSON line")
            continue

        request_id = request.get("id")
        method = request.get("method")
        params = request.get("params") or {}

        if method == "describe":
            _send({"id": request_id, "result": instance.describe()})
        elif method == "invoke":
            try:
                result = instance.invoke(params.get("input", {}), params.get("params") or {})
                _send({"id": request_id, "result": result})
            except Exception as exc:  # noqa: BLE001 - report to host, keep process alive
                _send(
                    {
                        "id": request_id,
                        "error": {"code": "plugin_error", "message": str(exc)},
                    }
                )
        elif method == "shutdown":
            return 0
        else:
            _send(
                {
                    "id": request_id,
                    "error": {"code": "unknown_method", "message": str(method)},
                }
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(serve(Plugin))
