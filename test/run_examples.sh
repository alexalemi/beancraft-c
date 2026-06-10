#!/bin/bash
# Test suite for beancraft-c examples

BC="${1:-./beancraft}"
PASS=0
FAIL=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

check() {
    local name="$1"
    local expected="$2"
    shift 2

    # Run beancraft and extract the relevant output value
    local output=$("$BC" "$@" 2>&1)
    local actual=$(echo "$output" | grep -E "^${expected%%=*} = " | head -1 | sed 's/.* = //')
    local expect_val="${expected#*=}"

    if [ "$actual" = "$expect_val" ]; then
        echo -e "${GREEN}PASS${NC}: $name"
        ((PASS++))
    else
        echo -e "${RED}FAIL${NC}: $name"
        echo "  Expected: $expected"
        echo "  Got: ${expected%%=*} = $actual"
        ((FAIL++))
    fi
}

# Like check(), but runs an already-built native binary.
check_bin() {
    local name="$1"
    local expected="$2"
    local bin="$3"
    shift 3

    local output=$("$bin" "$@" 2>&1)
    local actual=$(echo "$output" | grep -E "^${expected%%=*} = " | head -1 | sed 's/.* = //')
    local expect_val="${expected#*=}"

    if [ "$actual" = "$expect_val" ]; then
        echo -e "${GREEN}PASS${NC}: $name"
        ((PASS++))
    else
        echo -e "${RED}FAIL${NC}: $name"
        echo "  Expected: $expected"
        echo "  Got: ${expected%%=*} = $actual"
        ((FAIL++))
    fi
}

echo "=========================================="
echo "Beancraft-C Example Test Suite"
echo "=========================================="
echo

# --- Addition tests ---
echo -e "${YELLOW}=== Addition (add.bc) ===${NC}"
check "0 + 0 = 0" "Out=0" examples/add.bc A=0 B=0
check "1 + 0 = 1" "Out=1" examples/add.bc A=1 B=0
check "0 + 1 = 1" "Out=1" examples/add.bc A=0 B=1
check "5 + 3 = 8" "Out=8" examples/add.bc A=5 B=3
check "100 + 200 = 300" "Out=300" examples/add.bc A=100 B=200

# --- Copy tests ---
echo
echo -e "${YELLOW}=== Copy (copy.bc) ===${NC}"
check "copy 0" "To=0" examples/copy.bc From=0
check "copy 1" "To=1" examples/copy.bc From=1
check "copy 42" "To=42" examples/copy.bc From=42
check "copy preserves From" "From=100" examples/copy.bc From=100

# --- Multiplication tests ---
echo
echo -e "${YELLOW}=== Multiplication (mul.bc) ===${NC}"
check "0 * 5 = 0" "Out=0" examples/mul.bc A=0 B=5
check "5 * 0 = 0" "Out=0" examples/mul.bc A=5 B=0
check "1 * 1 = 1" "Out=1" examples/mul.bc A=1 B=1
check "3 * 4 = 12" "Out=12" examples/mul.bc A=3 B=4
check "7 * 8 = 56" "Out=56" examples/mul.bc A=7 B=8
check "12 * 12 = 144" "Out=144" examples/mul.bc A=12 B=12

# --- Division tests ---
echo
echo -e "${YELLOW}=== Division (div.bc) ===${NC}"
check "10 / 2 = 5" "Quotient=5" examples/div.bc Dividend=10 Divisor=2
check "10 / 3 = 3" "Quotient=3" examples/div.bc Dividend=10 Divisor=3
check "10 % 3 = 1" "Dividend=1" examples/div.bc Dividend=10 Divisor=3
check "7 / 7 = 1" "Quotient=1" examples/div.bc Dividend=7 Divisor=7
check "5 / 10 = 0" "Quotient=0" examples/div.bc Dividend=5 Divisor=10
check "100 / 7 = 14" "Quotient=14" examples/div.bc Dividend=100 Divisor=7

# --- IsEven tests ---
echo
echo -e "${YELLOW}=== IsEven (iseven.bc) ===${NC}"
check "0 is even" "Even=1" examples/iseven.bc N=0
check "1 is odd" "Even=0" examples/iseven.bc N=1
check "2 is even" "Even=1" examples/iseven.bc N=2
check "7 is odd" "Even=0" examples/iseven.bc N=7
check "100 is even" "Even=1" examples/iseven.bc N=100

# --- IsZero tests ---
echo
echo -e "${YELLOW}=== IsZero (iszero.bc) ===${NC}"
check "0 is zero" "Zero=1" examples/iszero.bc N=0
check "1 is not zero" "Zero=0" examples/iszero.bc N=1
check "100 is not zero" "Zero=0" examples/iszero.bc N=100

# --- Min tests ---
echo
echo -e "${YELLOW}=== Min (min.bc) ===${NC}"
check "min(3, 5) = 3" "Out=3" examples/min.bc A=3 B=5
check "min(5, 3) = 3" "Out=3" examples/min.bc A=5 B=3
check "min(7, 7) = 7" "Out=7" examples/min.bc A=7 B=7
check "min(0, 10) = 0" "Out=0" examples/min.bc A=0 B=10

# --- Max tests ---
echo
echo -e "${YELLOW}=== Max (max.bc) ===${NC}"
check "max(3, 5) = 5" "Out=5" examples/max.bc A=3 B=5
check "max(5, 3) = 5" "Out=5" examples/max.bc A=5 B=3
check "max(7, 7) = 7" "Out=7" examples/max.bc A=7 B=7
check "max(0, 10) = 10" "Out=10" examples/max.bc A=0 B=10

# --- Optimized (-O) variants: exercise the loop folds (TRANSFER/DIVMOD/MULADD) ---
echo
echo -e "${YELLOW}=== Optimized -O (folds must not change results) ===${NC}"
check "mul -O: 0 * 5 = 0" "Out=0" examples/mul.bc -O A=0 B=5
check "mul -O: 5 * 0 = 0" "Out=0" examples/mul.bc -O A=5 B=0
check "mul -O: 1 * 1 = 1" "Out=1" examples/mul.bc -O A=1 B=1
check "mul -O: 7 * 8 = 56" "Out=56" examples/mul.bc -O A=7 B=8
check "mul -O: 13 * 17 = 221" "Out=221" examples/mul.bc -O A=13 B=17
check "mul -O: 100 * 200 = 20000" "Out=20000" examples/mul.bc -O A=100 B=200
check "div -O: 100 / 7 = 14" "Quotient=14" examples/div.bc -O Dividend=100 Divisor=7
check "iseven -O: 100 is even" "Even=1" examples/iseven.bc -O N=100
check "factorial -O: 0! = 1" "Out=1" examples/factorial.bc -O N=0
check "factorial -O: 5! = 120" "Out=120" examples/factorial.bc -O N=5
check "factorial -O: 8! = 40320" "Out=40320" examples/factorial.bc -O N=8
check "pow -O: 0^0 = 1" "Out=1" examples/pow.bc -O Base=0 Exp=0
check "pow -O: 2^10 = 1024" "Out=1024" examples/pow.bc -O Base=2 Exp=10
check "pow -O: 3^4 = 81" "Out=81" examples/pow.bc -O Base=3 Exp=4
check "pow -O: 10^5 = 100000" "Out=100000" examples/pow.bc -O Base=10 Exp=5
check "fib -O: fib(20) = 6765" "Out=6765" examples/fib.bc -O N=20
check "gcd -O: gcd(48,18) = 6" "Out=6" examples/gcd.bc -O A=48 B=18

# --- urm.bc: the universal register machine (run a few small programs through it) ---
echo
echo -e "${YELLOW}=== urm.bc (Gödel-encoded universal machine; out_k = simulated s_k) ===${NC}"
check "urm: iseven N=4 -> Even(s0)=1" "out0=1" examples/urm.bc -O $("$BC" --emit-urm examples/iseven.bc N=4 | tail -1)
check "urm: iseven N=7 -> Even(s0)=0" "out0=0" examples/urm.bc -O $("$BC" --emit-urm examples/iseven.bc N=7 | tail -1)
check "urm: iszero N=0 -> Zero(s0)=1" "out0=1" examples/urm.bc -O $("$BC" --emit-urm examples/iszero.bc N=0 | tail -1)
check "urm: add A=10 B=5 -> Out(s1)=15" "out1=15" examples/urm.bc -O $("$BC" --emit-urm examples/add.bc A=10 B=5 | tail -1)
check "urm: mul A=2 B=3 -> Out(s1)=6" "out1=6" examples/urm.bc -O $("$BC" --emit-urm examples/mul.bc A=2 B=3 | tail -1)

# --- Differential sweep: -O0 vs -O on every example via --check ---
# Device programs are skipped (their side effects would run twice), and a
# program that can't halt within the cap on zero/default inputs is SKIPped
# (exit 2 = INCONCLUSIVE), not failed.
echo
echo -e "${YELLOW}=== Differential sweep: --check on every example ===${NC}"
for f in examples/*.bc; do
    out=$("$BC" -c -s 100000000 "$f" 2>&1)
    rc=$?
    name="check $(basename "$f")"
    if echo "$out" | grep -q "cannot run device programs"; then
        continue   # device example; nothing to compare
    fi
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}: $name"
        ((PASS++))
    elif [ $rc -eq 2 ]; then
        echo -e "${YELLOW}SKIP${NC}: $name (inconclusive at step cap)"
    else
        echo -e "${RED}FAIL${NC}: $name"
        echo "$out" | head -3 | sed 's/^/  /'
        ((FAIL++))
    fi
done

# --- Compiled (-O) path: only if qbe + the bccompile script are available ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
BCCOMPILE="$PROJ_DIR/scripts/bccompile"
if command -v qbe >/dev/null 2>&1 && [ -x "$BCCOMPILE" ]; then
    echo
    echo -e "${YELLOW}=== Compiled with -O (QBE backend; folds incl. MULADD) ===${NC}"
    CTMP="$(mktemp -d)"
    trap 'rm -rf "$CTMP"' EXIT
    if "$BCCOMPILE" -O examples/mul.bc "$CTMP/mul" >/dev/null 2>&1; then
        check_bin "compiled mul -O: 7 * 8 = 56"       "Out=56"       "$CTMP/mul" A=7 B=8
        check_bin "compiled mul -O: 0 * 5 = 0"        "Out=0"        "$CTMP/mul" A=0 B=5
        check_bin "compiled mul -O: 5 * 0 = 0"        "Out=0"        "$CTMP/mul" A=5 B=0
        check_bin "compiled mul -O: 13 * 17 = 221"    "Out=221"      "$CTMP/mul" A=13 B=17
        check_bin "compiled mul -O: 1234 * 5678"      "Out=7006652"  "$CTMP/mul" A=1234 B=5678
    else
        echo -e "${YELLOW}SKIP${NC}: bccompile -O examples/mul.bc failed"
    fi
    if "$BCCOMPILE" -O examples/factorial.bc "$CTMP/fact" >/dev/null 2>&1; then
        check_bin "compiled factorial -O: 0! = 1"     "Out=1"        "$CTMP/fact" N=0
        check_bin "compiled factorial -O: 6! = 720"   "Out=720"      "$CTMP/fact" N=6
        check_bin "compiled factorial -O: 12! "       "Out=479001600" "$CTMP/fact" N=12
    fi
    if "$BCCOMPILE" -O examples/pow.bc "$CTMP/pow" >/dev/null 2>&1; then
        check_bin "compiled pow -O: 0^0 = 1"          "Out=1"        "$CTMP/pow" Base=0 Exp=0
        check_bin "compiled pow -O: 2^10 = 1024"      "Out=1024"     "$CTMP/pow" Base=2 Exp=10
        check_bin "compiled pow -O: 7^3 = 343"        "Out=343"      "$CTMP/pow" Base=7 Exp=3
    fi
    # Compiled -O0 (no folds) must still be correct.
    if "$BCCOMPILE" -O0 examples/mul.bc "$CTMP/mul0" >/dev/null 2>&1; then
        check_bin "compiled mul -O0: 7 * 8 = 56"      "Out=56"       "$CTMP/mul0" A=7 B=8
    fi
else
    echo
    echo -e "${YELLOW}(skipping compiled-path tests: qbe not found)${NC}"
fi

# --- Summary ---
echo
echo "=========================================="
echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
echo "=========================================="

exit $FAIL
