#!/bin/bash
# triple2 Binder IPC test suite — boundary / edge case testing
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDER_DEV="/dev/binderfs/triple"
SM_BIN="$SCRIPT_DIR/sm2"
SRV_BIN="$SCRIPT_DIR/srv2"
CLI_BIN="$SCRIPT_DIR/cli2"
PASS=0
FAIL=0

cleanup() {
    kill $(jobs -p) 2>/dev/null || true
    wait $(jobs -p) 2>/dev/null || true
    rm -rf "$TMPDIR" 2>/dev/null || true
    rm -f "$BINDER_DEV" 2>/dev/null || true
}

trap cleanup EXIT

# temp dir for SM/Server stdout capture
TMPDIR=$(mktemp -d)

# ---- helpers ----
start_sm() {
    rm -f "$BINDER_DEV"
    "$SM_BIN" > "$TMPDIR/sm.log" 2>&1 &
    SM_PID=$!
    for i in $(seq 1 20); do
        if grep -q "ready" "$TMPDIR/sm.log" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "  SM startup timed out"; cat "$TMPDIR/sm.log"; return 1
}

start_server() {
    "$SRV_BIN" > "$TMPDIR/srv.log" 2>&1 &
    SRV_PID=$!
    for i in $(seq 1 20); do
        if grep -q "registered" "$TMPDIR/srv.log" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "  Server startup timed out"; cat "$TMPDIR/srv.log"; return 1
}

stop_all() {
    [ -n "${SM_PID:-}" ]  && kill "$SM_PID"  2>/dev/null || true
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    sleep 0.2
    rm -f "$BINDER_DEV"
}

run_client() {
    timeout 3 "$CLI_BIN" "$1" "$2" 2>&1 || true
}

result_line() { echo "$1" | grep "result:" || echo ""; }

assert_result() {
    local name="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  ✅ PASS: $name"; ((PASS++))
    else
        echo "  ❌ FAIL: $name"; echo "    expected: '$expected'"; echo "    actual:   '$actual'"; ((FAIL++))
    fi
}

assert_contains() {
    local name="$1" needle="$2" output="$3"
    if echo "$output" | grep -qF "$needle"; then
        echo "  ✅ PASS: $name"; ((PASS++))
    else
        echo "  ❌ FAIL: $name"; echo "    should contain: '$needle'"; echo "    got: $output"; ((FAIL++))
    fi
}

# ---- pre-checks ----
if [ "$EUID" -ne 0 ]; then echo "Run as root (sudo)"; exit 1; fi
if ! mount | grep -qF "binderfs"; then
    echo "binderfs not mounted. Mount with: mount -t binder binder /dev/binderfs"; exit 1
fi

echo "=== Building ==="
cd "$SCRIPT_DIR"
g++ -o sm2   triple2_sm.cpp     || exit 1
g++ -o srv2  triple2_server.cpp || exit 1
g++ -o cli2  triple2_client.cpp || exit 1
echo "Build OK"
echo ""

# =================================================================
# Phase 1: Basic transformations
# =================================================================
echo "=== Phase 1: Basic transformations ==="
start_sm && start_server

T=$(run_client "Hello Binder" "upper")
assert_result "upper: Hello Binder → HELLO BINDER" \
    "[client] result: 'HELLO BINDER'" "$(result_line "$T")"

T=$(run_client "LOWERCASE SENTENCE" "lower")
assert_result "lower: LOWERCASE SENTENCE → lowercase sentence" \
    "[client] result: 'lowercase sentence'" "$(result_line "$T")"

T=$(run_client "SnAkE CaSe TeXt" "upper")
assert_result "mixed: SnAkE CaSe TeXt → SNAKE CASE TEXT" \
    "[client] result: 'SNAKE CASE TEXT'" "$(result_line "$T")"

T=$(run_client "ALREADY UPPER" "upper")
assert_result "upper no-op: ALREADY UPPER → ALREADY UPPER" \
    "[client] result: 'ALREADY UPPER'" "$(result_line "$T")"

T=$(run_client "already lower" "lower")
assert_result "lower no-op: already lower → already lower" \
    "[client] result: 'already lower'" "$(result_line "$T")"

stop_all; echo ""

# =================================================================
# Phase 2: Boundary values
# =================================================================
echo "=== Phase 2: Boundary values ==="
start_sm && start_server

T=$(run_client "a" "upper")
assert_result "single char a→A" \
    "[client] result: 'A'" "$(result_line "$T")"

T=$(run_client "Z" "lower")
assert_result "single char Z→z" \
    "[client] result: 'z'" "$(result_line "$T")"

T=$(run_client "123!@#$%^&*()" "upper")
assert_result "digits + symbols: unchanged" \
    "[client] result: '123!@#\$%^&*()'" "$(result_line "$T")"

T=$(run_client "   " "upper")
assert_result "whitespace only" \
    "[client] result: '   '" "$(result_line "$T")"

# 200-char string (under 255 limit)
S_IN=$(python3 -c "print('a'*200)")
S_OUT=$(python3 -c "print('A'*200)")
T=$(run_client "$S_IN" "upper")
assert_result "200-char string" \
    "[client] result: '$S_OUT'" "$(result_line "$T")"

# 255-char string (at boundary)
S_IN=$(python3 -c "print('a'*255)")
S_OUT=$(python3 -c "print('A'*255)")
T=$(run_client "$S_IN" "upper")
assert_result "255-char string (truncation boundary)" \
    "[client] result: '$S_OUT'" "$(result_line "$T")"

stop_all; echo ""

# =================================================================
# Phase 3: Error paths
# =================================================================
echo "=== Phase 3: Error paths ==="

# 3a — SM running, no server → service not found
echo "  --- 3a: No server ---"
start_sm
T=$(run_client "hello" "upper")
assert_contains "service not found (no server)" "service not found" "$T"
stop_all; echo ""

# 3b — Server registers then dies → client gets BR_DEAD_REPLY
echo "  --- 3b: Server dies after registration ---"
start_sm && start_server
kill $SRV_PID 2>/dev/null; wait $SRV_PID 2>/dev/null || true
sleep 0.3
T=$(run_client "hello" "upper")
assert_contains "dead server → empty result" "[client] result: ''" "$T"
stop_all; echo ""

# =================================================================
# Phase 4: Burst requests
# =================================================================
echo "=== Phase 4: Burst requests ==="
start_sm && start_server

N=30; ok=true
for i in $(seq 1 $N); do
    OUT=$(run_client "msg $i" "upper")
    if ! echo "$OUT" | grep -q "result: 'MSG $i'"; then
        ok=false; echo "  Request $i mismatch: $OUT"; break
    fi
done
if $ok; then echo "  ✅ PASS: $N burst requests all correct"; ((PASS++))
else echo "  ❌ FAIL: burst requests"; ((FAIL++)); fi

stop_all; echo ""

# =================================================================
# Phase 5: Alternating upper/lower stress
# =================================================================
echo "=== Phase 5: Alternating upper/lower ==="
start_sm && start_server

N=60; ok=true
for i in $(seq 1 $N); do
    if (( i % 2 )); then
        OUT=$(run_client "abc$i" "upper")
        if ! echo "$OUT" | grep -q "ABC$i"; then ok=false; break; fi
    else
        OUT=$(run_client "XYZ$i" "lower")
        if ! echo "$OUT" | grep -q "xyz$i"; then ok=false; break; fi
    fi
done
if $ok; then echo "  ✅ PASS: $N alternating upper/lower correct"; ((PASS++))
else echo "  ❌ FAIL: alternating at request $i"; ((FAIL++)); fi

stop_all; echo ""

# =================================================================
# Summary
# =================================================================
echo "========================================"
printf "  Results:  \033[32m✓ %d passed\033[0m  \033[33m✗ %d failed\033[0m\n" $PASS $FAIL
echo "========================================"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
