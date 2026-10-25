# Iris T3 process plugins

Process plugins are ordinary programs that speak one JSON object per line on
stdin/stdout. They run as separate processes, so a crash or a heavy dependency
does not take down Iris.

Available reference implementations:

- `iris_plugin.py` — small Python SDK (`Plugin`, `@plugin`, `serve`).
- `example-check/` — generated-style check plugin (`plugin.json` + `check.py`).

```powershell
# validate the manifest (no process is started)
iris plugin validate plugins/example-check

# start it, perform the describe handshake and print the result
iris plugin invoke example.check.cps --method describe

# invoke it with a JSON input file
iris plugin invoke example.check.cps --method invoke --input input.json --params '{"maxCps": 5}'
```

On Linux/macOS the manifest command is usually `python3`; edit
`plugins/example-check/plugin.json` if `python` is not on `PATH`.
