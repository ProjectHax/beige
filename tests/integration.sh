#!/bin/sh
# Drives beige -p against a local SSE stand-in. No network and no API key.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN=${BEIGE_BIN:-"$ROOT/beige"}
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
HOME_DIR=$(mktemp -d)
export BEIGE_HOME="$HOME_DIR"
python3 "$ROOT/tests/mock_sse.py" "$PORT" &
PID=$!
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$HOME_DIR"' EXIT
sleep 0.2

run_one() {
    provider=$1
    extra=$2
    out=$("$BIN" --provider "$provider" --api-key test --base-url "http://127.0.0.1:$PORT" \
        --no-context --mode auto -p "say hi" $extra)
    printf '%s' "$out" | grep -q beige-ok
    printf '%s' "$out" | grep -q done
}

run_one anthropic ""
run_one openai "--model gpt-test"
# openai defaults to the responses wire
"$BIN" --provider openai --api-key test --base-url "http://127.0.0.1:$PORT" \
    --no-context --mode auto -p "say hi" >/tmp/beige-responses-out
# The default wire is responses, which the mock serves on /responses.
grep -q beige-ok /tmp/beige-responses-out
grep -q done /tmp/beige-responses-out

# Chat wire is selected from config. Write a tiny config and use it.
mkdir -p "$HOME_DIR"
cat > "$HOME_DIR/config.json" <<EOF
{"provider":"openai","model":"gpt-test","mode":"auto","openai":{"base_url":"http://127.0.0.1:$PORT","wire":"chat"}}
EOF
out=$("$BIN" --api-key test --no-context -p "say hi")
printf '%s' "$out" | grep -q beige-ok
printf '%s' "$out" | grep -q done
rm -f /tmp/beige-responses-out
echo "integration ok"
