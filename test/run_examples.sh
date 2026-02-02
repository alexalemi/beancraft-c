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

# --- Summary ---
echo
echo "=========================================="
echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
echo "=========================================="

exit $FAIL
