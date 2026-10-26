# beige

```
┌─ beige 0.2.0 ───────────────────────────────────────────────────────────────┐
│ _          _                                                                │
│| |__  ___ (_) __ _  ___   terminal coding agent                             │
│| '_ \/ -_)| |/ _` |/ -_)  ~/src/beige                                       │
│|_.__/\___||_|\__, |\___|  type / for commands, ? for shortcuts              │
│              |___/        shift-tab cycles ask, auto, plan                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

beige is a terminal coding agent written in C. It runs on POSIX systems from the early 2000s: ISO C99 as accepted by GCC 3.4, POSIX.1-2001, an 80×24 terminal, and no mouse, LSP, or Node runtime. HTTPS is libcurl built on OpenSSL 1.1.1 or 3.x, because a 2004 system libssl cannot speak to current APIs.

## Warning

The model can do something destructive, especially on an older Linux distribution. There is zero liability and zero responsibility for what it does.

The same warning is printed the first time beige starts, then remembered in `~/.beige/warned`.

The tool loop is the small one from [Pi](https://pi.dev): read, edit, write, and a shell, plus grep, ls, fetch, and search. Permission modes follow OpenCode and Claude Code in the form that matters on a machine you cannot snapshot.

| Mode | What it does |
| --- | --- |
| `ask` | `read`, `grep`, and `ls` run. `write`, `edit`, `bash`, `fetch`, and `search` each wait for yes, always, skip, or deny. Default. |
| `plan` | No source edits. The agent may write `.beige/plan.md`. A fixed list of read-only commands runs; everything else asks. |
| `auto` | Tools run. `rm -rf /`, `mkfs`, `dd` onto a device, shutdown, and a fork bomb still ask. A shell write outside the working directory still asks, and so does a write onto `/etc`, `/usr`, `/bin`, `/sbin`, `/boot`, `/lib`, `/lib64`, `/opt`, `/var`, `/sys`, `/proc`, `/dev`, `/root`, or `/run`. |

In plan mode these commands run when they contain no pipes, redirects, or substitutions: `pwd`, `ls`, `cat`, `grep`, `find`, `uname`, `head`, `tail`, `wc`, `true`, `echo`, `git status`, `git diff`, `git log`, `git show`, and `make -n`. `find` with `-delete`, `-exec`, `-ok`, or `-fprint`, and `git` with `--output`, still ask.

Cycle modes with Ctrl-T or `/mode`. With the sandbox on (the default), `read`, `write`, `edit`, `grep`, and `ls` refuse a path that resolves outside the working directory. `write` and `edit` also refuse to follow a symlink. The shell check looks at redirects and at the destination of `cp`, `mv`, `ln`, `install`, `rsync`, `tee`, `truncate`, and `dd of=`. It is a string check, so a command that builds the path in another program can still get past it. System directories still ask when sandbox is off. `"sandbox": false` allows other absolute paths, such as `/tmp`.

## Build

```sh
make
make test
```

`make` needs a C99 compiler and libcurl (`pkg-config` optional; override with `CURL_CFLAGS` and `CURL_LIBS`).

On an old Linux system, `sh install.sh` downloads OpenSSL and curl, builds them static, builds beige from this directory, and installs it. `gcc`, `make`, and `perl` must already be installed, or the script asks the distro for them when it can. If this machine cannot fetch current HTTPS sites, put the tarballs in the cache directory the script names and run it again.

### Static curl and OpenSSL for an old kernel

Build OpenSSL 3 so it does not require `getrandom` (Linux 3.17):

```sh
./Configure linux-x86_64 --prefix=/opt/beige-deps --with-rand-seed=devrandom,os no-shared
make && make install
```

Build curl against that OpenSSL, without the libraries an old host will not have:

```sh
./configure --prefix=/opt/beige-deps --with-openssl=/opt/beige-deps \
  --disable-shared --enable-static \
  --without-libpsl --without-nghttp2 --without-brotli --without-zstd \
  --disable-ldap --disable-alt-svc
make && make install
```

Then:

```sh
make CURL_CFLAGS="-I/opt/beige-deps/include" \
     CURL_LIBS="/opt/beige-deps/lib/libcurl.a /opt/beige-deps/lib/libssl.a /opt/beige-deps/lib/libcrypto.a -ldl -lpthread"
```

Public APIs will not trust a 2004 certificate store, so beige brings its own. Unless `ca_bundle` is set in the config, it uses the first `cacert.pem` it finds in `~/.beige`, beside the `beige` executable, in `/usr/local/share/beige`, or in `/usr/share/beige`. `install.sh`, the release tarballs, and the packages put a current one in place. `beige status` shows which bundle is in use. curl still honors `http_proxy`, `https_proxy`, and `no_proxy`.

### Static binaries

CI builds beige static against musl with `zig cc`, with OpenSSL and curl inside. A binary needs no libc, libcurl, or OpenSSL on the machine, only Linux 2.6 or later. Each build names the oldest CPU it runs on:

| Build | Oldest CPU |
| --- | --- |
| `x86_64` | any 64-bit x86 |
| `i686-sse2` | Pentium 4, Pentium M, Athlon 64 |
| `i686` | Pentium Pro, Pentium II and III, Athlon, Geode LX. No SSE. |
| `i486` | 486, Pentium, Pentium MMX, K6, VIA C3. No SSE, MMX, or CMOV. |

There is no 80386 build. musl's atomics need the 486's `cmpxchg`, and Linux dropped the 386 in 3.8.

Each release has a tarball per build, plus `.deb` and `.rpm` packages of the `x86_64` and `i486` builds. Each holds `beige`, a current CA bundle, and the licenses of what is linked in, so nothing needs copying by hand. The packages have no dependencies and install `/usr/bin/beige`, with the bundle in `/usr/share/beige`:

```sh
dpkg -i beige_0.2.0-1_amd64.deb        # Debian 4.0 or later; beige_0.2.0-1_i386.deb on 32-bit, from 3.1
rpm -i beige-0.2.0-1.x86_64.rpm        # CentOS 5 or later; beige-0.2.0-1.i486.rpm on 32-bit
```

A tarball runs where it is unpacked, since beige finds the `cacert.pem` beside it:

```sh
tar -xzf beige-0.2.0-linux-i686.tar.gz
./beige-0.2.0-linux-i686/beige
```

CI runs the tests under qemu-user on the oldest CPU model for each build, and scans the `i686` and `i486` binaries for instructions those CPUs lack. It also runs each binary in a CentOS 5 or 6 or Debian 3.1, 4.0, or 5.0 container and installs the packages there with that distribution's `rpm` or `dpkg`, though those containers share a current kernel. A `v` tag attaches the tarballs to a GitHub release, whose notes are that version's section of [CHANGELOG.md](CHANGELOG.md).

To build one here, run `sh scripts/build-static.sh build i486`, then `check i486`. `build` needs zig 0.15, perl, and make. `check` also needs qemu-user and objdump, and `packages i486` needs rpmbuild.

## Credentials

Config lives in `~/.beige/config.json`. Secrets live in `~/.beige/auth.json` (mode 0600). `BEIGE_HOME` overrides that directory. Environment variables win over the file: `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `XAI_API_KEY`.

```sh
beige login anthropic
beige login openai
beige login xai
beige login xai-sub      # SuperGrok / X Premium, device code, no browser on this machine
beige login openai-sub   # ChatGPT subscription, device code
beige status
```

Anthropic is API-key only. Claude subscription login belongs to Claude Code's own client; beige does not impersonate it.

xAI device login is an RFC 8628 grant against `auth.x.ai`. The access token is sent to `https://api.x.ai/v1`. A 403 that says the subscription tier has no API access is reported as-is.

ChatGPT subscription login uses the public Codex device-code endpoints and then calls `https://chatgpt.com/backend-api/codex/responses` with `originator: beige`. OpenAI does not document this for third-party clients, and it can stop working without notice. API-key OpenAI is unaffected. Set `openai_sub.originator` in the config if a gateway requires a different value.

## Instructions

On startup beige loads `~/.beige/AGENTS.md`, then walks from the working directory up to the git root (or `/`). In each directory it reads `AGENTS.md`, or `CLAUDE.md` when `AGENTS.md` is absent. Closer files come later. Each file is capped at 32 KiB and the total at 64 KiB. `--no-context` skips this.

`/init` asks the model to draft an `AGENTS.md` from the README, Makefile or `configure`, and the top of the tree, then writes it through the normal permission check.

## Sessions

Transcripts are JSONL files under `~/.beige/sessions/`. `beige --continue` reopens the newest one for this directory. `/compact` summarizes older turns when the rough token estimate (bytes / 4) crosses `context_budget` (default 80000). File reads default to 256 KiB and tool output to 32 KiB, so a large log cannot exhaust a small machine.

## Use

```sh
beige
beige -p "explain the Makefile"
beige --continue
beige --provider openai --model gpt-4.1
beige --provider anthropic --base-url https://api.anthropic.com
```

The bottom row shows the mode, provider, model, directory, and context. The prompt is `> `.

Type `/` for commands. Up and down move, Tab fills the line, Enter runs it, and Esc clears it. `?` lists shortcuts. `/mode`, `/model`, `/provider`, `/login`, and `/logout` open a numbered list. Esc closes it, or returns to the previous list. A blank line does the same on a line-only terminal.

Shift-Tab or Ctrl-T cycles ask, auto, and plan. Esc interrupts a reply. Ctrl-C cancels it, and a second Ctrl-C exits. Ctrl-D leaves. Alt-Enter inserts a newline, and a pasted newline stays one. Enter during a reply queues one follow-up. Ctrl-O expands a long tool result. A line starting with `!` runs in the background, and Ctrl-X kills it.

A permission question replaces the prompt: yes, always, skip, or deny. Up and down move. y, a, n, d, or 1–4 answer it. Esc skips. On a line-only terminal, type the answer and press Enter; a blank line asks again. "Always" for a command covers later runs of that command on its own, not a pipeline or list that starts with it.

`glyphs` is `auto` (the default), `unicode`, or `ascii`. `auto` uses box drawing when the locale is UTF-8. Set `"glyphs": "ascii"` when those characters draw two columns wide.

`-p` runs one prompt and exits. It uses auto mode unless you pass `--mode`, and it does not ask for permission.

## Tools

`read`, `write`, `edit` (exact `old_string`, must match once), `bash` (`/bin/sh -c`), `grep`, `ls`, `fetch` (http and https only), and `search` (a web query). In ask mode, `fetch` and `search` wait for approval with the other write and shell tools. Shell output stops at `output_cap` (default 32 KiB). The default has no time limit. Set `shell_timeout` in the config, or pass `timeout` on the command, and the process group is killed when the time is up. Cancel kills it too.

## Layout

`src/` is the program. `third_party/cJSON.c` is Dave Gamble's cJSON, the only vendored dependency. Providers speak the Anthropic Messages API, the OpenAI Responses API (or chat completions when `openai.wire` is `chat`), and the xAI chat API.

Windows is not a target of this version.

## License

Copyright (C) 2026 ProjectHax LLC. beige is released under the GNU General Public License, version 3. The text is in [LICENSE](LICENSE).

The coding system prompt is adapted from OpenCode's MIT-licensed gemini prompt. Reply formatting follows Grok Build, which is Apache-2.0; the renderer in `src/tui.c` is beige's own code. cJSON stays MIT. The tool list follows Pi, and ask, plan, and auto follow OpenCode and Claude Code. None of those programs' source is included, except the adapted OpenCode prompt. The notices are in [NOTICE](NOTICE).
