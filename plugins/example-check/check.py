#!/usr/bin/env python3
"""Example Iris T3 check plugin.

Input may be a subtitle list:

    {"lines": [{"text": "Hello", "startMs": 0, "endMs": 1000}]}

or an Iris document (sentences are read from `state.tracks`).  Parameters:

    {"maxChars": 42, "maxCps": 6.0}
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from iris_plugin import Plugin, plugin, serve  # noqa: E402


def _sentences(document: Any) -> list[dict[str, Any]]:
    if isinstance(document, list):
        return [item for item in document if isinstance(item, dict)]
    if not isinstance(document, dict):
        return []
    if isinstance(document.get("lines"), list):
        return [item for item in document["lines"] if isinstance(item, dict)]
    state = document.get("state")
    if isinstance(state, dict):
        result: list[dict[str, Any]] = []
        for track in state.get("tracks", []):
            if not isinstance(track, dict):
                continue
            result.extend(item for item in track.get("sentences", []) if isinstance(item, dict))
        return result
    return []


@plugin(id="example.check.cps", kind="check", version="0.1.0")
class CpsCheck(Plugin):
    def invoke(self, input_data: Any, params: dict[str, Any]) -> dict[str, Any]:
        max_chars = int(params.get("maxChars", 42))
        max_cps = float(params.get("maxCps", 6.0))
        issues: list[dict[str, Any]] = []
        sentences = _sentences(input_data)

        for index, sentence in enumerate(sentences):
            text = str(sentence.get("text", ""))
            start = int(sentence.get("startMs", sentence.get("start_ms", -1)))
            end = int(sentence.get("endMs", sentence.get("end_ms", -1)))
            if not text.strip():
                issues.append({"index": index, "code": "empty_text", "text": text})
                continue
            if len(text) > max_chars:
                issues.append(
                    {
                        "index": index,
                        "code": "too_long",
                        "text": text,
                        "chars": len(text),
                        "maxChars": max_chars,
                    }
                )
            if start >= 0 and end > start:
                cps = len(text) / ((end - start) / 1000.0)
                if cps > max_cps:
                    issues.append(
                        {
                            "index": index,
                            "code": "cps",
                            "text": text,
                            "cps": round(cps, 2),
                            "maxCps": max_cps,
                        }
                    )
            else:
                issues.append({"index": index, "code": "missing_timing", "text": text})

        return {
            "checked": len(sentences),
            "issues": issues,
            "maxChars": max_chars,
            "maxCps": max_cps,
        }


if __name__ == "__main__":
    raise SystemExit(serve(CpsCheck))
