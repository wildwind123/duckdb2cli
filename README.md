# duckb2cli

A command-line SQL client for DuckDB 2 servers that speak the **Quack** client/server protocol.
It runs on Linux and Windows.

```
$ duckb2cli
duckb2cli 0.1.0 (DuckDB v2.0.0)
Connected to quack://192.168.0.94:9494 as database "remote".
Enter ".help" for usage hints.
remote> SELECT id, name FROM items LIMIT 2;
┌────────┬─────────┐
│   id   │  name   │
│ bigint │ varchar │
├────────┼─────────┤
│      0 │ item 0  │
│      1 │ item 1  │
└────────┴─────────┘
2 rows
```

## How it works

duckb2cli embeds DuckDB (`libduckdb`) as an in-memory engine. It loads the `quack` extension (installed
automatically on first use) and attaches the server:

```sql
ATTACH 'quack://HOST:PORT' AS remote (TYPE quack, TOKEN '...');
USE remote;
```

The SQL you type runs against the attached remote catalog. That includes DDL, DML and transactions.
Session settings (`SET ...`) apply to the local engine. Use `.remote SQL` to run a statement on the server
exactly as written, for example a server-side `SET` or `PRAGMA`.

Ctrl+C cancels the running query. It interrupts the local engine and also sends `quack_cancel` to the
server, so the server stops working too.

The client uses only DuckDB's stable C API (`duckdb.h`). The build defines `DUCKDB_API_NO_DEPRECATED`, so
nothing deprecated is compiled in. The `duckdb_v2.h` API is not used because every function in it is
still marked unstable in 2.0.0.

## Configuration

Settings come from three places. Later ones win:

1. A config file with `KEY=VALUE` lines. `--config FILE` picks one explicitly. Otherwise the first of these
   that exists is used: `$DUCKB2CLI_CONFIG`, `./duckb2cli.env`, or the per-user file
   (`~/.config/duckb2cli/config.env` on Linux, `%APPDATA%\duckb2cli\config.env` on Windows).
2. Environment variables with the same names.
3. Command-line options.

| Key | Option | Meaning |
|---|---|---|
| `DUCKDB_HOST_BIND` | `-H`, `--host` | Server host or IP |
| `DUCKDB_HOST_PORT` | `-p`, `--port` | Server port (default `9494`) |
| `DUCKDB_QUACK_TOKEN` | `-t`, `--token` | Auth token the server was started with |
| `DUCKDB_QUACK_URI` | `-u`, `--uri` | Full `quack://host:port` URI, used instead of host/port |
| `DUCKDB_QUACK_SSL` | `--ssl` / `--no-ssl` | Force TLS on or off (unset: extension default) |
| `DUCKDB_QUACK_SSL_FINGERPRINT` | `--ssl-fingerprint` | Pin the server certificate (SHA-256, hex) |
| `DUCKDB_QUACK_ALIAS` | `-a`, `--alias` | Name of the attached database (default `remote`) |
| `DUCKDB_LOCAL_DATABASE` | `-l`, `--local` | Open a local database file instead of a server |

Example (`duckb2cli.env.example`):

```ini
DUCKDB_HOST_BIND=192.168.0.94
DUCKDB_HOST_PORT=9494
DUCKDB_QUACK_TOKEN=change-me
```

The token is a secret:

- Prefer the config file or an environment variable over `--token`, because command-line arguments are
  visible in the process list.
- On Linux, the client warns if the config file is readable by other users.
- `duckb2cli.env` is git-ignored.
- The token is replaced with `***` in any error message the client prints.

## Usage

```
duckb2cli                                   # interactive shell
duckb2cli -c "SELECT count(*) FROM items"   # run and exit
duckb2cli -f report.sql --csv -o report.csv # run a file, write CSV
echo "SELECT 42;" | duckb2cli --json        # read SQL from stdin
duckb2cli --local my.duckdb                 # work on a local file, no server
```

Statements end with `;` and may span several lines. In scripts, a final statement without `;` still runs.
Exit codes:

| Code | When |
|---|---|
| 0 | Success |
| 1 | A statement failed (in `-c`, `-f` or piped mode) |
| 2 | Bad command-line usage |
| 130 | Interrupted with Ctrl+C |
| N | Set with `.exit N` |

Output modes (`-m MODE`, or shortcuts `--csv`, `--json`, …):

| Mode | Output |
|---|---|
| `box` | Default. Table that fits the terminal width |
| `csv` | RFC 4180 CSV |
| `tsv` | Tab-separated, with `\t`, `\n` and `\\` escaped |
| `json` | A JSON array of objects |
| `jsonlines` | One JSON object per line |
| `line` | One `column = value` line per column |
| `markdown` | Markdown table |

DML (`INSERT`/`UPDATE`/`DELETE`/`COPY`/`CREATE TABLE AS`) prints `N rows changed` in the interactive shell
and nothing in scripts, so CSV and JSON output stays clean. `... RETURNING` prints its rows.

Dot commands (`.help` lists them):

| Command | Does |
|---|---|
| `.mode`, `.headers`, `.nullvalue`, `.maxrows`, `.maxwidth` | Control output |
| `.timer on` | Show how long each statement takes |
| `.output FILE` / `.output` | Redirect results to a file / back to the terminal |
| `.read FILE` | Run a SQL file |
| `.tables`, `.schema [TABLE]`, `.databases` | Browse the catalog |
| `.remote SQL` | Run SQL on the server as-is |
| `.status` | Show the connection, settings and server version |
| `.bail`, `.echo` | Script behaviour |
| `.exit [CODE]` | Leave (also Ctrl+D; Ctrl+Z then Enter on Windows) |

Values are formatted by DuckDB itself, so they look the same as in the official `duckdb` shell. One
exception: `GEOMETRY` and `VARIANT` have no constructor in the stable C API yet, so they show as
`<geometry>` / `<variant>`. Cast them to display them, e.g. `ST_AsText(geom)` or `v::VARCHAR`.

## Building

You need CMake 3.16 or newer, a C11 compiler, and the DuckDB 2.0 C library. Get the library from the
`duckdb-shared-libs-<os>-<arch>` archives on the
[DuckDB releases page](https://github.com/duckdb/duckdb/releases). If it isn't installed system-wide, pass
its directory as `-DDUCKDB_ROOT=...`.

### Linux

```sh
sudo apt install build-essential cmake libreadline-dev   # readline is optional (line editing, history)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/duckb2cli --version
```

The binary finds `libduckdb.so` through the system library path or the path it was found at during the
build. When you ship it, put `libduckdb.so` next to the binary (the rpath includes `$ORIGIN`).

### Windows (Visual Studio)

Download and extract `duckdb-shared-libs-windows-amd64.tar.gz`. It contains `duckdb.h`, `duckdb.lib` and
`duckdb.dll`. Then build:

```bat
cmake -S . -B build -DDUCKDB_ROOT=C:\path\to\duckdb-shared-libs-windows-amd64
cmake --build build --config Release
build\Release\duckb2cli.exe --version
```

The build copies `duckdb.dll` next to `duckb2cli.exe`. Ship the two files together.

### Windows, cross-compiled from Linux

```sh
sudo apt install gcc-mingw-w64-x86-64   # already in the dev container
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DDUCKDB_ROOT=/path/to/duckdb-shared-libs-windows-amd64
cmake --build build-win
```

On Windows the client:

- reads console input as UTF-16 and converts it to UTF-8, so non-ASCII SQL works;
- takes UTF-8 arguments and file paths;
- switches the console to UTF-8 output.

Line editing and history come from the Windows console itself (arrow keys, F7).

## Testing

`tests/smoke_test.sh` starts a throwaway Quack server with the `duckdb` CLI on a random port and token, then
checks the client against it. It covers:

- output modes;
- DML and transactions;
- script parsing;
- config precedence;
- auth failure;
- token redaction;
- `--local` mode;
- Ctrl+C cancelling a server-side query.

```sh
cmake --build build && (cd build && ctest --output-on-failure)
```

## Layout

| File | Contents |
|---|---|
| `src/main.c` | Command-line parsing and the order settings are applied in |
| `src/config.c` | Config file and environment loading |
| `src/shell.c` | Connecting, running statements, dot commands, input loop, Ctrl+C handling |
| `src/render.c` | Output modes |
| `src/format.c` | Turning DuckDB vectors into display text and type names |
| `src/platform.c` | Linux/Windows differences: console, line input, signals, paths |
| `src/util.c` | String buffer, UTF-8 width, small helpers |
