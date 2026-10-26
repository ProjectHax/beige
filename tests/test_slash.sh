#!/bin/sh
# Line-mode menus and a screen palette check. No network.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN=${BEIGE_BIN:-"$ROOT/beige"}

line_home=$(mktemp -d)
export BEIGE_HOME="$line_home"
printf '/mode\nplan\n/provider\n2\n/quit\n' | "$BIN" >/tmp/beige-slash-line.out
python3 - "$line_home/config.json" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
assert cfg["mode"] == "plan", cfg
assert cfg["provider"] == "openai", cfg
PY
printf '/q\n1\n' | "$BIN" >/dev/null
printf '/exit\n' | "$BIN" >/dev/null
printf '/m\n1\n\n\n/quit\n' | "$BIN" >/tmp/beige-slash-back.out
python3 - /tmp/beige-slash-back.out "$line_home/config.json" <<'PY'
import json, sys
text = open(sys.argv[1], errors="replace").read()
back = text.find("esc back")
if back < 0 or text.find("esc closes", back) < 0:
    sys.stderr.write("back menu missing\n" + text)
    sys.exit(1)
cfg = json.load(open(sys.argv[2]))
assert cfg["mode"] == "plan", cfg
PY
rm -rf "$line_home"
rm -f /tmp/beige-slash-back.out

plan_home=$(mktemp -d)
export BEIGE_HOME="$plan_home"
printf '/status\n/plan\n/quit\n' | "$BIN" >/tmp/beige-plan.out
python3 - /tmp/beige-plan.out "$plan_home/config.json" <<'PY'
import json, sys
text = open(sys.argv[1], errors="replace").read()
for needle in ("provider:", "auth:", "no plan file yet"):
    if needle not in text:
        sys.stderr.write("status/plan missing %s\n%s\n" % (needle, text))
        sys.exit(1)
cfg = json.load(open(sys.argv[2]))
assert cfg["mode"] == "plan", cfg
PY
printf '!printf beige-bang\n/quit\n' | "$BIN" >/tmp/beige-bang.out
grep -q beige-bang /tmp/beige-bang.out
rm -rf "$plan_home"
rm -f /tmp/beige-plan.out /tmp/beige-bang.out

python3 - "$BIN" <<'PY'
import os, pty, select, sys, time
bin_path = sys.argv[1]
home = "/tmp/beige-slash-pty"
os.makedirs(home, exist_ok=True)
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    os.execv(bin_path, [bin_path])

def read_until(pred, limit=2.5):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

first = read_until(lambda b: b"/help" in b and b"/mode" in b)
# The hint is in the log; type slash to open the palette.
os.write(fd, b"/")
pal = read_until(lambda b: b"/provider" in b and b"/quit" in b)
os.write(fd, b"\x1b[B")  # down, highlight moves
time.sleep(0.1)
os.write(fd, b"\r")  # enter the highlighted command, /mode if down from /help... 
# After '/', selection 0 is /help. Down makes /mode. Enter opens the mode menu.
menu = read_until(lambda b: b"plan" in b and b"auto" in b)
os.write(fd, b"\x1b")  # start esc
os.write(fd, b"\x1b")  # lone esc cancels the menu
time.sleep(0.2)
os.write(fd, b"/quit\r")
time.sleep(0.4)
os.kill(pid, 15)
os.waitpid(pid, 0)
text = (first + pal + menu).decode("utf-8", "replace")
missing = []
for needle in ("/help", "/mode", "/provider", "plan", "auto"):
    if needle not in text:
        missing.append(needle)
if missing:
    sys.stderr.write("missing %s\n" % ", ".join(missing))
    sys.exit(1)
PY
rm -f /tmp/beige-slash-line.out

python3 - "$BIN" <<'PY'
import os, pty, select, sys, time
bin_path = sys.argv[1]
home = "/tmp/beige-slash-back-pty"
os.makedirs(home, exist_ok=True)
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    os.execv(bin_path, [bin_path, "--no-context"])

def read_until(pred, limit=2.5):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

read_until(lambda b: b"type /" in b)
os.write(fd, b"/m \r")
opened = read_until(lambda b: b"esc closes" in b and b"/model" in b)
os.write(fd, b"\r")
nested = read_until(lambda b: b"esc back" in b and b"plan" in b)
os.write(fd, b"\x1b")
again = read_until(lambda b: b"esc closes" in b)
os.write(fd, b"\x1b")
time.sleep(0.2)
os.write(fd, b"/exit\r")
time.sleep(0.4)
os.kill(pid, 15)
os.waitpid(pid, 0)
if b"esc back" not in nested or b"esc closes" not in again:
    sys.stderr.write("screen back failed\n")
    sys.stderr.write((opened + nested + again).decode("utf-8", "replace"))
    sys.exit(1)
PY

python3 - "$BIN" <<'PY'
import os, pty, select, sys, time
bin_path = sys.argv[1]
home = "/tmp/beige-keys-pty"
os.system("rm -rf " + home)
os.makedirs(home, exist_ok=True)
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    os.execv(bin_path, [bin_path, "--no-context"])

def read_until(pred, limit=3.0):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

read_until(lambda b: b"type /" in b)
os.write(fd, b"?")
helped = read_until(lambda b: b"expand tool output" in b)
os.write(fd, b"\x7f")
closed = read_until(lambda b: b"[ ASK ]" in b or b"[ AUTO ]" in b)
os.write(fd, b"\x1b[Z")
shifted = read_until(lambda b: b"[ AUTO ]" in b)
os.write(fd, b"one\x1b\rtwo\r")
wrapped = read_until(lambda b: b"> one" in b and b"two" in b and b"no credentials" in b, 4)
os.write(fd, b"!sleep 30\r")
started = read_until(lambda b: b"sleep 30" in b)
os.write(fd, b"\x18")
killed = read_until(lambda b: b"killed" in b)
os.write(fd, b"/exit\r")
time.sleep(0.3)
os.kill(pid, 15)
os.waitpid(pid, 0)
bad = []
if b"expand tool output" not in helped:
    bad.append("help")
if b"[ ASK ]" not in closed and b"[ AUTO ]" not in closed:
    bad.append("help-close")
if b"[ AUTO ]" not in shifted:
    bad.append("shift-tab")
if b"> one" not in wrapped or b"two" not in wrapped or wrapped.count(b"no credentials") != 1:
    bad.append("alt-enter")
if b"killed" not in killed:
    bad.append("ctrl-x")
if bad:
    sys.stderr.write("keys failed: %s\n" % ", ".join(bad))
    sys.stderr.write((helped + closed + shifted + wrapped + started + killed).decode("utf-8", "replace"))
    sys.exit(1)
PY

model_home=$(mktemp -d)
export BEIGE_HOME="$model_home"
port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$ROOT/tests/mock_sse.py" "$port" &
mock_pid=$!
trap 'kill "$mock_pid" 2>/dev/null || true' EXIT
sleep 0.2
cat > "$model_home/config.json" <<EOF
{"provider":"anthropic","model":"old","mode":"ask","anthropic":{"base_url":"http://127.0.0.1:$port"}}
EOF
printf '%s\n' '{"anthropic":{"api_key":"test"}}' > "$model_home/auth.json"
chmod 600 "$model_home/auth.json"
printf '/model\n1\n/quit\n' | "$BIN" >/dev/null
python3 - "$model_home/config.json" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
assert cfg["model"] == "claude-from-api", cfg
assert cfg["provider"] == "anthropic", cfg
PY

disp_home=$(mktemp -d)
cat > "$disp_home/config.json" <<EOF
{"provider":"anthropic","model":"claude-from-api","mode":"auto","anthropic":{"base_url":"http://127.0.0.1:$port"}}
EOF
printf '%s\n' '{"anthropic":{"api_key":"test"}}' > "$disp_home/auth.json"
chmod 600 "$disp_home/auth.json"
python3 - "$BIN" "$disp_home" <<'PY'
import os, pty, select, sys, time
bin_path, home = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    os.execv(bin_path, [bin_path, "--no-context"])

def read_until(pred, limit=6):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

import re
def strip_sgr(raw):
    return re.sub(br"\x1b\[[0-9;?]*[A-Za-z]", b"", raw)
opened = read_until(lambda b: b"type /" in b or b"ask >" in b)
os.write(fd, b"hello from me\r")
got = read_until(lambda b: b"done" in b and b"beige-ok" in b)
plain = strip_sgr(opened + got)
os.write(fd, b"/exit\r")
time.sleep(0.3)
os.kill(pid, 15)
os.waitpid(pid, 0)
missing = []
if b"thinking" not in got:
    missing.append("thinking spinner")
if b"\x1b[100m" not in got or b"> hello from me" not in got:
    missing.append("gray user line")
if b"\x1b[32m*" not in got:
    missing.append("green tool bullet")
if b"* Bash(echo beige-ok)" not in plain or b"`- beige-ok" not in plain:
    missing.append("tool elbow")
if b"* done" not in plain:
    missing.append("assistant bullet")
if b"type /" not in opened and b"type /" not in got:
    missing.append("welcome hint")
if missing:
    sys.stderr.write("display missing %s\n" % ", ".join(missing))
    sys.stderr.write(got.decode("utf-8", "replace"))
    sys.exit(1)
PY
ask_home=$(mktemp -d)
cat > "$ask_home/config.json" <<EOF
{"provider":"anthropic","model":"claude-from-api","mode":"ask","glyphs":"ascii","anthropic":{"base_url":"http://127.0.0.1:$port"}}
EOF
printf '%s\n' '{"anthropic":{"api_key":"test"}}' > "$ask_home/auth.json"
chmod 600 "$ask_home/auth.json"
python3 - "$BIN" "$ask_home" <<'PY'
import os, pty, select, sys, time
bin_path, home = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    os.execv(bin_path, [bin_path, "--no-context"])

def read_until(pred, limit=8):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

read_until(lambda b: b"type /" in b or b"[ ASK ]" in b)
os.write(fd, b"hello\r")
asked = read_until(lambda b: b"Do you want to proceed?" in b)
os.write(fd, b"1")
got = read_until(lambda b: b"done" in b and b"beige-ok" in b)
os.write(fd, b"/exit\r")
time.sleep(0.3)
os.kill(pid, 15)
os.waitpid(pid, 0)
missing = []
for needle in (b"Bash command", b"echo beige-ok", b"Yes, always for echo", b"Do you want to proceed?"):
    if needle not in asked:
        missing.append(needle.decode())
if b"done" not in got or b"beige-ok" not in got:
    missing.append("after yes")
if missing:
    sys.stderr.write("ask dialog missing %s\n" % ", ".join(missing))
    sys.stderr.write((asked + got).decode("utf-8", "replace"))
    sys.exit(1)
PY
rm -rf "$ask_home"
rm -rf "$disp_home"
kill "$mock_pid" 2>/dev/null || true
rm -rf "$model_home"

python3 - "$BIN" <<'PY'
import fcntl, os, pty, select, struct, sys, termios, time
bin_path = sys.argv[1]
home = "/tmp/beige-tiny-pty"
os.system("rm -rf " + home)
os.makedirs(home, exist_ok=True)
pid, fd = pty.fork()
if pid == 0:
    os.environ["BEIGE_HOME"] = home
    os.environ["TERM"] = "xterm"
    os.environ["LC_ALL"] = "C"
    os.chdir("/tmp")
    fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
    os.execv(bin_path, [bin_path, "--no-context"])

def read_until(pred, limit=2.5):
    buf = b""
    deadline = time.time() + limit
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.15)
        if not r:
            if pred(buf):
                return buf
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if pred(buf):
            return buf
    return buf

first = read_until(lambda b: b"[ ASK ]" in b)
if b"[ ASK ]" not in first or b"type /" in first:
    sys.stderr.write("40x8 welcome not skipped\n")
    sys.stderr.write(first.decode("utf-8", "replace"))
    os.kill(pid, 15)
    os.waitpid(pid, 0)
    sys.exit(1)
os.write(fd, b"/")
time.sleep(0.15)
os.write(fd, b"\x1b")
time.sleep(0.2)
os.write(fd, b"?")
helped = read_until(lambda b: b"expand tool output" in b)
os.write(fd, b"/exit\r")
deadline = time.time() + 3
status = None
while time.time() < deadline:
    wpid, status = os.waitpid(pid, os.WNOHANG)
    if wpid == pid:
        break
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            os.read(fd, 4096)
        except OSError:
            pass
else:
    os.kill(pid, 15)
    os.waitpid(pid, 0)
    sys.stderr.write("40x8 did not exit\n")
    sys.stderr.write(helped.decode("utf-8", "replace"))
    sys.exit(1)
if b"expand tool output" not in helped:
    sys.stderr.write("40x8 help missing\n")
    sys.stderr.write(helped.decode("utf-8", "replace"))
    sys.exit(1)
PY
rm -rf /tmp/beige-tiny-pty
echo "slash ok"
