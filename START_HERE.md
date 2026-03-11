# The Lay Of The Land

## Add LLDB MCP

See https://github.com/stass/lldb-mcp

```json
"lldb-mcp": {
  "command": "/Users/<me>/dev/lldb-mcp/.venv/bin/python",
  "args": ["-u", "/Users/<me>/dev/lldb-mcp/lldb_mcp.py"],
  "disabled": false
},
```

## Workflow

1. use ai to make source code changes and maybe use a tdd workflow
2. use [trexity-tests.js](./scripts/trexity-tests.js) as the test suite, all tests must pass.
3. committing and pushing your changes as you go
4. when changes are good to be used in prod use [trexity-publish.sh](./scripts/trexity-publish.sh)
5. after new version has been published (printed from the command above), use in the trexity repo use `cd ./devbox.d/packages/vroom && ./update-vroom.sh trexity-1.15.0-<timestamp>`
6. deploy the routing server
