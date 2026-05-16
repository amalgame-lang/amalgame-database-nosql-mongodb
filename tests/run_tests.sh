#!/bin/bash
# ─────────────────────────────────────────────────────
#  amalgame-database-nosql-mongodb — Test Runner
#  Usage: ./tests/run_tests.sh [/path/to/amc]
#
#  Double-gated:
#    1. libmongoc + libbson headers present
#    2. MongoDB server reachable at $MONGO_URI host:port
#       (default mongodb://127.0.0.1:27017)
#
#  Start a server locally:
#    docker run --rm -p 27017:27017 mongo:7
# ─────────────────────────────────────────────────────

set -u

if [ $# -ge 1 ]; then
    AMC="$1"
elif [ -n "${AMC:-}" ]; then
    :
elif command -v amc >/dev/null 2>&1; then
    AMC="$(command -v amc)"
else
    echo "ERROR: amc not found." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_RUNTIME="$PKG_ROOT/runtime"

AMC_DIR="$(cd "$(dirname "$AMC")" && pwd)"
if [ -d "$AMC_DIR/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/runtime"
elif [ -d "$AMC_DIR/../share/amalgame/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/../share/amalgame/runtime"
elif [ -n "${AMC_RUNTIME:-}" ]; then
    :
else
    echo "ERROR: amc runtime/ not found. Set AMC_RUNTIME=..." >&2
    exit 2
fi

BUILD_DIR="$(mktemp -d -t amongo-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT
PROJ_DIR="$BUILD_DIR/proj"
mkdir -p "$PROJ_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

echo ""
echo "════════════════════════════════════════════"
echo "  amalgame-database-nosql-mongodb — Tests"
echo "════════════════════════════════════════════"
echo "  amc:     $AMC ($("$AMC" --version 2>&1 | head -1))"
echo "  runtime: $AMC_RUNTIME"
echo ""

# ── Gate 1 : libmongoc + libbson present? ───────────
echo "── Probing libmongoc + libbson ─────────────"
MONGOC_OK=0
MONGOC_CFLAGS=""
for prefix in '' '/usr/include/libmongoc-1.0' '/usr/include/libbson-1.0' \
              '/usr/local/include/libmongoc-1.0' '/usr/local/include/libbson-1.0' \
              '/opt/homebrew/include/libmongoc-1.0' '/opt/homebrew/include/libbson-1.0'; do
    if [ -n "$prefix" ]; then MONGOC_CFLAGS="$MONGOC_CFLAGS -I$prefix"; fi
done
echo "#include <mongoc/mongoc.h>" > "$BUILD_DIR/_inc.c"
if gcc $MONGOC_CFLAGS -E "$BUILD_DIR/_inc.c" >/dev/null 2>&1; then
    MONGOC_OK=1
    echo "  mongoc/mongoc.h:   found"
fi
if [ "$MONGOC_OK" = "0" ]; then
    echo "  mongoc/mongoc.h:   NOT FOUND (install libmongoc-dev / mongo-c-driver-devel / mongo-c-driver)"
fi
echo ""

# ── Gate 2 : MongoDB server reachable? ──────────────
MONGO_URI="${MONGO_URI:-mongodb://127.0.0.1:27017}"
MONGO_HOST=$(echo "$MONGO_URI" | sed -E 's|^mongodb(\+srv)?://([^@/]*@)?([^:/?]+).*|\3|')
MONGO_PORT=$(echo "$MONGO_URI" | sed -nE 's|^mongodb(\+srv)?://([^@/]*@)?[^:/?]+:([0-9]+).*|\3|p')
[ -z "$MONGO_PORT" ] && MONGO_PORT=27017
MG_OK=0
if (echo > /dev/tcp/$MONGO_HOST/$MONGO_PORT) 2>/dev/null; then
    MG_OK=1
    echo "  mongo:           reachable at $MONGO_HOST:$MONGO_PORT"
else
    echo "  mongo:           NOT reachable at $MONGO_HOST:$MONGO_PORT"
fi
echo ""

# ── Stage fake cache for the test fixture ───────────
FAKE_CACHE="$BUILD_DIR/cache"
PKG_GIT="github.com/amalgame-lang/amalgame-database-nosql-mongodb"
PKG_TAG="${PKG_TAG:-v0.1.0}"
FAKE_SHA="deadbeefcafebabe0000000000000000000000ab"
SHORT_SHA="${FAKE_SHA:0:8}"
PKG_CACHE_DIR="$FAKE_CACHE/$PKG_GIT/${PKG_TAG}_${SHORT_SHA}"

mkdir -p "$(dirname "$PKG_CACHE_DIR")"
rm -rf "$PKG_CACHE_DIR"
ln -s "$PKG_ROOT" "$PKG_CACHE_DIR"

cat > "$PROJ_DIR/amalgame.lock" <<EOF
[[package]]
name = "amalgame-database-nosql-mongodb"
git  = "$PKG_GIT"
tag  = "$PKG_TAG"
rev  = "$FAKE_SHA"
EOF

export AMALGAME_PACKAGES_DIR="$FAKE_CACHE"

run_test() {
    local name="$1"
    local expected="$2"
    printf "  %-38s" "$name"
    if [ "$MONGOC_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (libmongoc-dev not installed)"
        SKIP=$((SKIP + 1)); return
    fi
    if [ "$MG_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (no mongo at $MONGO_HOST:$MONGO_PORT)"
        SKIP=$((SKIP + 1)); return
    fi
    cp "$SCRIPT_DIR/stdlib_mongodb.am" "$PROJ_DIR/test.am"
    local out_base="$PROJ_DIR/test"
    local out
    out=$(cd "$PROJ_DIR" && "$AMC" -o test test.am 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (amc)"; echo "$out" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    if [ ! -f "$out_base.c" ]; then
        echo -e "${RED}FAIL${NC} (no .c)"; FAIL=$((FAIL + 1)); return
    fi
    gcc -O2 -w $MONGOC_CFLAGS \
        -I"$AMC_RUNTIME" -I"$PKG_RUNTIME" \
        "$out_base.c" \
        -lgc -lm -lcurl -ldl -lpthread -lmongoc-1.0 -lbson-1.0 \
        -o "$out_base" 2>"$BUILD_DIR/link.log"
    if [ ! -x "$out_base" ]; then
        echo -e "${RED}FAIL${NC} (gcc link)"
        cat "$BUILD_DIR/link.log" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    local run_output
    run_output=$("$out_base" 2>&1)
    if echo "$run_output" | grep -qF "$expected"; then
        echo -e "${GREEN}PASS${NC}"; PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "    expected: $expected"
        echo "    got:      $(echo "$run_output" | head -3 | tr '\n' '|')"
        FAIL=$((FAIL + 1))
    fi
}

echo "── MongoDB ─────────────────────────────────"
run_test "connect"               "[PASS] connect"
run_test "ping"                  "[PASS] ping"
run_test "ServerVersion"         "[PASS] ServerVersion"
run_test "clean slate"           "[PASS] clean slate"
run_test "3 inserts"             "[PASS] 3 inserts"
run_test "Count == 3"            "[PASS] Count == 3"
run_test "FindOne(beta)"         "[PASS] FindOne(beta) found"
run_test "FindMany returns 3"    "[PASS] FindMany returns 3 docs"
run_test "UpdateOne"             "[PASS] UpdateOne"
run_test "UpdateOne persisted"   "[PASS] UpdateOne persisted n=42"
run_test "DeleteOne"             "[PASS] DeleteOne"
run_test "Count after delete"    "[PASS] Count after delete == 2"
run_test "bad JSON error"        "[PASS] bad JSON reports error"
run_test "Close drops connection" "[PASS] Close drops connection"

echo ""
echo "────────────────────────────────────────────"
echo -e "  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}  |  ${YELLOW}SKIP: $SKIP${NC}"
echo "────────────────────────────────────────────"
echo ""

[ $FAIL -eq 0 ] && exit 0 || exit 1
