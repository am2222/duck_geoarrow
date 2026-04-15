# Testing DuckDB WASM Extensions Locally

Step-by-step guide for building and testing a DuckDB extension in the browser
via DuckDB-WASM. Written for `duck_geoarrow` but applies to any DuckDB
extension.

## Version matrix (as of 2026-04-14)

| Component | Version | Notes |
|---|---|---|
| DuckDB (source) | `v1.5.2` | git tag checked out in `duckdb/` submodule |
| extension-ci-tools | `v1.5.1` branch | submodule in `extension-ci-tools/` |
| Emscripten (emsdk) | `3.1.56` | **must match** the version used to build `@duckdb/duckdb-wasm` |
| `@duckdb/duckdb-wasm` | `1.33.1-dev53.0` | dev release that bundles DuckDB v1.5.x |

### Why versions matter

A DuckDB WASM extension is loaded at runtime into the DuckDB WASM host. The
extension imports functions from the host by symbol name **and** WASM type
signature. If either doesn't match, you get:

```
LinkError: WebAssembly.Instance(): Import #N "env" "...": imported function does not match the expected type
```

Three things must align:

1. **DuckDB version**: the extension and the `@duckdb/duckdb-wasm` npm package
   must be built from the same (or ABI-compatible) DuckDB source.
2. **Emscripten version**: different Emscripten versions produce different WASM
   calling conventions, especially for `-fwasm-exceptions`. The npm package was
   built with `emsdk 3.1.56` (pinned in the duckdb-wasm CI Docker image).
3. **Unsigned extensions**: `@duckdb/duckdb-wasm` must be initialized with
   `allowUnsignedExtensions: true` since locally-built extensions are not
   signed. The hosted shell at `shell.duckdb.org` does **not** allow this
   setting, so you need a local HTML page.

## Step 1: Install the correct Emscripten version

```sh
# If you don't have emsdk yet:
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Install and activate the version matching duckdb-wasm's CI
./emsdk install 3.1.56
./emsdk activate 3.1.56
source emsdk_env.sh

# Verify
emcc --version
# emcc ... 3.1.56 ...
```

## Step 2: Check out the correct DuckDB version

```sh
cd your-extension-repo

# Update the duckdb submodule to the target version
cd duckdb
git fetch --tags
git checkout v1.5.2
cd ..
```

## Step 3: Build the WASM extension

```sh
# Make sure emsdk is active in this shell
source /path/to/emsdk/emsdk_env.sh

# Clean any previous WASM build (important after version changes)
rm -rf build/wasm_eh

# Build the wasm_eh target (wasm with exception handling)
make wasm_eh
```

This produces the extension at:
```
build/wasm_eh/repository/v1.5.2/wasm_eh/your_extension.duckdb_extension.wasm
```

The `repository/` directory follows the layout DuckDB expects:
`<repo_url>/<duckdb_version>/<platform>/<extension>.duckdb_extension.wasm`

## Step 4: Serve the extension locally with CORS

The browser needs to fetch the `.wasm` file cross-origin, so you need a server
with `Access-Control-Allow-Origin: *` headers.

```sh
# From the repo root:
python3 -c "
from http.server import HTTPServer, SimpleHTTPRequestHandler
import os

class CORSHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        super().end_headers()
    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

os.chdir('build/wasm_eh/repository')
print('Serving at http://localhost:9999')
HTTPServer(('0.0.0.0', 9999), CORSHandler).serve_forever()
"
```

Verify it works:
```sh
curl -I http://localhost:9999/v1.5.2/wasm_eh/duck_geoarrow.duckdb_extension.wasm
# Should return 200 with Access-Control-Allow-Origin: *
```

## Step 5: Open the test page

Open `duckdb-ext-test.html` in Chrome (see below) or use the full example at
`index.html`.

The standalone test page accepts URL parameters so you can reuse it for any
extension:

```
http://localhost:9999/duckdb-ext-test.html?ext=duck_geoarrow&repo=http://localhost:9999
```

Parameters:
- `ext` — extension name (default: `duck_geoarrow`)
- `repo` — extension repository URL (default: `http://localhost:9999`)
- `duckdb` — `@duckdb/duckdb-wasm` npm version (default: `1.33.1-dev53.0`)

## Troubleshooting

| Error | Cause | Fix |
|---|---|---|
| `Extension ... is not available` | DuckDB version in URL path doesn't match the npm package's bundled version | Check `SELECT version()` in the test page, ensure `build/wasm_eh/repository/<that_version>/wasm_eh/` exists |
| `imported function does not match the expected type` | Emscripten version mismatch between your build and the npm package | Rebuild with emsdk 3.1.56 (see step 1) |
| `signature is either missing or invalid` | `allowUnsignedExtensions` not set | Use the local test page, not `shell.duckdb.org` |
| `404 Not Found` on the `.wasm` file | CORS server not running or wrong repo URL | Check `curl` from step 4 |

## How to find the right @duckdb/duckdb-wasm version

When DuckDB publishes a new release, it takes time for a matching
`@duckdb/duckdb-wasm` stable release. In the meantime, use dev releases:

```sh
# List recent versions
npm view @duckdb/duckdb-wasm versions --json | python3 -c "
import json, sys
for v in json.load(sys.stdin)[-10:]: print(v)
"

# Check which DuckDB version a package bundles
cd /tmp
npm pack @duckdb/duckdb-wasm@VERSION
tar xzf duckdb-duckdb-wasm-*.tgz
strings package/dist/duckdb-eh.wasm | grep -oE 'v1\.[0-9]+\.[0-9]+' | sort -Vr | head -3
```

## How to find the Emscripten version used by duckdb-wasm

The version is pinned in the duckdb-wasm CI Docker image:

```
https://github.com/duckdb/duckdb-wasm/blob/main/actions/image/Dockerfile
```

Look for `ARG EMSDK_VERSION="..."`.

## Available functions (duck_geoarrow)

| Function | Arrow type |
|---|---|
| `st_asgeoarrow` | auto-detected from WKB |
| `st_asgeoarrowpoint` | `Struct<x, y>` |
| `st_asgeoarrowlinestring` | `List<Struct<x, y>>` |
| `st_asgeoarrowpolygon` | `List<List<Struct<x, y>>>` |
| `st_asgeoarrowmultipoint` | `List<Struct<x, y>>` |
| `st_asgeoarrowmultilinestring` | `List<List<Struct<x, y>>>` |
| `st_asgeoarrowmultipolygon` | `List<List<List<Struct<x, y>>>>` |
