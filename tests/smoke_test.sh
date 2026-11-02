#!/usr/bin/env bash
# End-to-end test: starts a throwaway Quack server with the duckdb CLI and drives duckb2cli against it.
# Usage: tests/smoke_test.sh <path-to-duckb2cli> [path-to-duckdb-cli]
set -u

CLIENT=$(cd "$(dirname "${1:?usage: smoke_test.sh <duckb2cli> [duckdb]}")" && pwd)/$(basename "$1")
DUCKDB=$(command -v "${2:-duckdb}") || { echo "duckdb CLI not found"; exit 1; }
PORT=$((20000 + RANDOM % 20000))
TOKEN="smoke_${RANDOM}${RANDOM}_token"
WORK=$(mktemp -d)
FAILS=0
PASSES=0

# Keep the developer's own settings out of the test.
unset DUCKDB_HOST_BIND DUCKDB_HOST_PORT DUCKDB_QUACK_TOKEN DUCKDB_QUACK_URI DUCKDB_QUACK_SSL \
      DUCKDB_QUACK_SSL_FINGERPRINT DUCKDB_QUACK_ALIAS DUCKDB_LOCAL_DATABASE DUCKB2CLI_CONFIG
cd "$WORK" || exit 1

cleanup() {
	exec 3>&- 2>/dev/null
	[ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null
	wait 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

pass() { PASSES=$((PASSES + 1)); echo "ok   - $1"; }
fail() { FAILS=$((FAILS + 1)); echo "FAIL - $1"; [ $# -gt 1 ] && printf '%s\n' "$2" | sed 's/^/       /'; }

expect_eq() { # name expected actual
	if [ "$2" == "$3" ]; then pass "$1"; else fail "$1" "expected: $2"$'\n'"actual:   $3"; fi
}
expect_contains() { # name needle haystack
	if [[ "$3" == *"$2"* ]]; then pass "$1"; else fail "$1" "missing '$2' in: $3"; fi
}

# ---- Server ----
mkfifo server.in
"$DUCKDB" -no-agent < server.in > server.log 2>&1 &
SERVER_PID=$!
exec 3> server.in
cat >&3 <<SQL
INSTALL quack;
LOAD quack;
CREATE TABLE items AS SELECT range AS id, 'item ' || range AS name, range * 2.5 AS price FROM range(5);
FROM quack_serve('quack://127.0.0.1:${PORT}', token := '${TOKEN}', disable_ssl := true);
SQL

export DUCKDB_HOST_BIND=127.0.0.1 DUCKDB_HOST_PORT=$PORT DUCKDB_QUACK_TOKEN=$TOKEN
ready=0
for _ in $(seq 1 60); do
	if "$CLIENT" -c "SELECT 1" > /dev/null 2>&1; then ready=1; break; fi
	sleep 0.5
done
if [ $ready -ne 1 ]; then
	echo "server did not come up:"; cat server.log
	exit 1
fi

# ---- Queries and output modes ----
out=$("$CLIENT" -c "SELECT id, name FROM items WHERE id < 2 ORDER BY id")
expect_contains "box output shows rows" "│      1 │ item 1  │" "$out"
expect_contains "box output shows row count" "2 rows" "$out"

out=$("$CLIENT" --csv -c "SELECT id, name, price FROM items WHERE id < 2 ORDER BY id")
expect_eq "csv output" $'id,name,price\n0,item 0,0.0\n1,item 1,2.5' "$out"

out=$("$CLIENT" --json -c "SELECT id, name, NULL AS n, [1, 2] AS l FROM items WHERE id = 3")
expect_eq "json output" $'[\n{"id":3,"name":"item 3","n":null,"l":"[1, 2]"}\n]' "$out"

out=$("$CLIENT" -m tsv --no-header -c "SELECT 'a' || chr(9) || 'b' AS x")
expect_eq "tsv escapes tabs" 'a\tb' "$out"

out=$("$CLIENT" -m line -c "SELECT 1 AS a, 'x' AS bb")
expect_eq "line output" $' a = 1\nbb = x' "$out"

# ---- DML, transactions, scripts ----
out=$("$CLIENT" --csv -c "CREATE TABLE t2 AS SELECT 1 AS a; INSERT INTO t2 VALUES (2); INSERT INTO t2 VALUES (3) RETURNING a; SELECT sum(a) AS s FROM t2; DROP TABLE t2;")
expect_eq "DML is quiet, RETURNING and SELECT print" $'a\n3\ns\n6' "$out"

out=$("$CLIENT" --csv -c "BEGIN; CREATE TABLE tx AS SELECT 1 AS a; ROLLBACK;" -c "SELECT count(*) AS n FROM duckdb_tables() WHERE table_name = 'tx'")
expect_eq "rollback is honoured" $'n\n0' "$out"

out=$(printf "SELECT 1 AS a, -- comment ;\n  'x;y' AS b,\n  \$\$p;q\$\$ AS c\n;\n.mode line\nSELECT 42 AS z;\n" | "$CLIENT" --csv)
expect_eq "multi-line script with comments, quotes and dot commands" $'a,b,c\n1,x;y,p;q\nz = 42' "$out"

printf "SELECT 1;\nSELECT nope;\nSELECT 2;\n" | "$CLIENT" --csv --bail > /dev/null 2>&1
expect_eq "--bail stops with exit 1" 1 $?

out=$(printf "SELECT 1 AS a;\nSELECT nope;\nSELECT 2 AS b;\n" | "$CLIENT" --csv 2>/dev/null)
expect_eq "errors without --bail continue" $'a\n1\nb\n2' "$out"

"$CLIENT" -c ".exit 7" > /dev/null 2>&1
expect_eq ".exit sets the exit code" 7 $?

out=$("$CLIENT" -c ".remote SELECT current_database() AS db" --csv 2>&1)
expect_contains ".remote runs on the server" "memory" "$out"

# ---- Configuration and errors ----
printf 'DUCKDB_HOST_BIND=127.0.0.1\nDUCKDB_HOST_PORT=%s\nDUCKDB_QUACK_TOKEN="%s"\n' "$PORT" "$TOKEN" > good.env
chmod 600 good.env
out=$(env -u DUCKDB_HOST_BIND -u DUCKDB_HOST_PORT -u DUCKDB_QUACK_TOKEN "$CLIENT" --config good.env --csv -c "SELECT 5 AS v")
expect_eq "config file is used" $'v\n5' "$out"

printf 'DUCKDB_HOST_BIND=127.0.0.1\nDUCKDB_HOST_PORT=%s\nDUCKDB_QUACK_TOKEN=wrong\n' "$PORT" > bad.env
chmod 600 bad.env
out=$("$CLIENT" --config bad.env --csv -c "SELECT 6 AS v")
expect_eq "environment overrides config file" $'v\n6' "$out"

out=$("$CLIENT" -t "not-the-token" -c "SELECT 1" 2>&1)
code=$?
expect_eq "wrong token exits 1" 1 $code
expect_contains "wrong token is reported" "Authentication failed" "$out"

out=$("$CLIENT" --csv -c "SELECT ${TOKEN}" 2>&1)
expect_contains "token is redacted from errors" "***" "$out"
if [[ "$out" == *"$TOKEN"* ]]; then fail "token does not leak into errors" "$out"; else pass "token does not leak into errors"; fi

out=$(env -u DUCKDB_HOST_BIND "$CLIENT" -c "SELECT 1" 2>&1)
expect_contains "missing host is explained" "no server configured" "$out"

out=$("$CLIENT" --local :memory: --csv -c "SELECT 40 + 2 AS answer")
expect_eq "--local works without a server" $'answer\n42' "$out"

# ---- Ctrl+C cancels server-side work ----
start=$(date +%s)
"$CLIENT" -c ".remote SELECT count(*) FROM range(500000000000)" -c "SELECT 'not reached'" > cancel.out 2>&1 &
pid=$!
sleep 2
kill -INT $pid
for _ in $(seq 1 40); do kill -0 $pid 2>/dev/null || break; sleep 0.25; done
if kill -0 $pid 2>/dev/null; then
	kill -9 $pid
	fail "Ctrl+C cancels a running server query" "client still running"
else
	wait $pid
	expect_eq "Ctrl+C ends a script with exit 130" 130 $?
	expect_contains "Ctrl+C reports the interrupt" "Interrupted" "$(cat cancel.out)"
fi
elapsed=$(( $(date +%s) - start ))
[ $elapsed -lt 15 ] && pass "cancel is prompt (${elapsed}s)" || fail "cancel is prompt" "${elapsed}s"

echo
echo "$PASSES passed, $FAILS failed"
[ $FAILS -eq 0 ]
