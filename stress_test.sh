#!/bin/bash
set -e

PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && sudo kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# compile
g++ demo_string_server.cpp -o server
g++ demo_string_client.cpp -o client
echo "[init] compiled OK"

# start server
sudo ./server &
SERVER_PID=$!
echo "[init] server pid=$SERVER_PID"
sleep 0.3

# -------------------------------------------------------
# test helper: $1=input $2=mode $3=expected
# -------------------------------------------------------
run_test() {
    local input="$1"
    local mode="$2"
    local expected="$3"

    local output
    output=$(sudo ./client "$input" "$mode" 2>&1) || true

    local actual
    actual=$(echo "$output" | grep -oP "got reply: '\K[^']*")

    if [ "$actual" = "$expected" ]; then
        echo "  PASS  input='$input' mode=$mode  -> '$actual'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  input='$input' mode=$mode  expected='$expected' got='$actual'"
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "[test] basic cases"
echo "-------------------"

run_test "hello world"     upper  "HELLO WORLD"
run_test "HELLO WORLD"     lower  "hello world"
run_test "AbCdEfGh"        upper  "ABCDEFGH"
run_test "AbCdEfGh"        lower  "abcdefgh"
run_test ""                upper  ""
run_test ""                lower  ""
run_test "12345!@#$%"      upper  "12345!@#$%"
run_test "12345!@#$%"      lower  "12345!@#$%"
run_test "Hello Binder!!!" upper  "HELLO BINDER!!!"
run_test "Hello Binder!!!" lower  "hello binder!!!"

echo ""
echo "[test] long strings (near 256 bytes)"
echo "-------------------"

# 255-char string (max that fits server's str_data[256])
LONG_A=$(python3 -c "print('a'*255)")
LONG_A_EXPECT=$(python3 -c "print('A'*255)")
run_test "$LONG_A" upper "$LONG_A_EXPECT"

LONG_A2=$(python3 -c "print('A'*255)")
LONG_A2_EXPECT=$(python3 -c "print('a'*255)")
run_test "$LONG_A2" lower "$LONG_A2_EXPECT"

# 100-char mixed
MIXED=$(python3 -c "print(''.join(chr(65+i%58) for i in range(100)))")
MIXED_UPPER=$(python3 -c "print(''.join(chr(65+i%58) for i in range(100)).upper())")
MIXED_LOWER=$(python3 -c "print(''.join(chr(65+i%58) for i in range(100)).lower())")
run_test "$MIXED" upper "$MIXED_UPPER"
run_test "$MIXED" lower "$MIXED_LOWER"

# long string with special chars
LONG_SPECIAL=$(python3 -c "print('Hello_' + 'binder'*20 + '!@#' + 'XyZ'*10)")
LONG_SPECIAL_UPPER=$(python3 -c "print(('Hello_' + 'binder'*20 + '!@#' + 'XyZ'*10).upper())")
LONG_SPECIAL_LOWER=$(python3 -c "print(('Hello_' + 'binder'*20 + '!@#' + 'XyZ'*10).lower())")
run_test "$LONG_SPECIAL" upper "$LONG_SPECIAL_UPPER"
run_test "$LONG_SPECIAL" lower "$LONG_SPECIAL_LOWER"

echo ""
echo "[test] medium-long strings"
echo "-------------------"

run_test "the quick brown fox jumps over the lazy dog 1234567890!@#$%^&*()" upper \
         "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890!@#$%^&*()"
run_test "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890!@#$%^&*()" lower \
         "the quick brown fox jumps over the lazy dog 1234567890!@#$%^&*()"

echo ""
echo "[test] stress: random seeded (seed=42, 100 rounds)"
echo "-------------------"
for i in $(seq 1 100); do
    # deterministic random: seeded by iteration number
    S=$(python3 -c "
import random
random.seed($i)
length = random.randint(1, 100)
chars = [chr(random.choice([random.randint(65,90), random.randint(97,122), random.randint(48,57)])) for _ in range(length)]
print(''.join(chars))
")
    mode=$(python3 -c "print('upper' if $i % 2 == 0 else 'lower')")
    if [ "$mode" = "upper" ]; then
        E=$(echo "$S" | tr '[:lower:]' '[:upper:]')
    else
        E=$(echo "$S" | tr '[:upper:]' '[:lower:]')
    fi
    run_test "$S" "$mode" "$E"
done

echo ""
echo "=================================="
echo "  PASS=$PASS  FAIL=$FAIL"
echo "=================================="

[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
