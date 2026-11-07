# Iris T3 process plugins

T3 plugins are ordinary programs that communicate with Iris using one UTF-8
JSON object per line on stdin/stdout. They run in separate processes, so a
plugin crash or dependency conflict does not crash the host.

## Minimal manifest

```json
{
  "id": "acme.check.cps",
  "kind": "check",
  "version": "0.1.0",
  "apiVersion": 1,
  "protocol": "stdio-jsonl",
  "command": "python",
  "args": ["check.py"],
  "timeoutMs": 5000,
  "enabled": true
}
```

stdout is reserved for protocol messages. Write diagnostics to stderr or emit
`log` notifications. The first request is normally `describe`; its response
must match the manifest ID, kind, and API version.

## CLI

```powershell
iris plugin validate plugins/example-check
iris plugin list
iris plugin invoke example.check.cps --method describe
iris plugin invoke example.check.cps --method invoke `
  --input input.json --params '{"maxCps":5}'
iris plugin check input.json --params '{"maxCps":5}'
iris plugin new my.check.rules --kind check --dir plugins
```

The host detects timeouts and process exits, reports the plugin as degraded,
and starts a new process on the next call. `--json` makes the host result
machine-readable.

## Python SDK

[`iris_plugin.py`](iris_plugin.py) is standard-library-only and exposes
`Plugin`, `@plugin`, and `serve()`. The
[`example-check`](example-check) directory is a generated-style working
example.

Plugins run with the current user's permissions. Manifest permissions are
declarations for discovery and auditing, not an OS sandbox. `iris plugin new`
creates a scaffold only; Iris does not currently install or sign plugin
packages, load T2 C ABI plugins, or provide OS sandboxing. Do not load
untrusted code.
