# Changelog

A release's GitHub page shows its section of this file. CI fails when the version in `src/main.c` has no section here, so add one before tagging.

## 0.2.0 - 2026-10-26

- `.deb` and `.rpm` packages of the x86_64 and i486 builds, for Debian 3.1 and CentOS 5 or later. They have no dependencies and install `/usr/bin/beige` with a current CA bundle in `/usr/share/beige`.
- beige finds its CA bundle without setup: after `~/.beige/cacert.pem` it looks beside the executable, then in `/usr/local/share/beige` and `/usr/share/beige`. An unpacked release tarball works where it is, and `install.sh` installs are found too.
- `beige status` and `/status` show the CA bundle in use.
- Release builds download curl's current copy of Mozilla's CA store and check it against the published SHA-256.

## 0.1.0 - 2026-10-26

First release.

- A terminal coding agent in C99 for POSIX systems from the early 2000s: GCC 3.4, POSIX.1-2001, and an 80×24 terminal.
- Providers: the Anthropic Messages API, the OpenAI Responses API or chat completions, and xAI. API keys, or device-code login for a SuperGrok or ChatGPT subscription.
- Tools: read, write, edit, bash, grep, ls, fetch, and search.
- Permission modes ask, plan, and auto. The sandbox keeps the file tools inside the working directory.
- Sessions saved as JSONL, `--continue` to reopen the last one, and `/compact` to summarize older turns.
- `AGENTS.md` (or `CLAUDE.md`) loaded from `~/.beige` and from the working directory up to the git root. `/init` drafts one.
- Static binaries for x86_64, i686-sse2, i686 (no SSE), and i486 (no SSE, MMX, or CMOV) that need only Linux 2.6 or later.
- `install.sh` builds OpenSSL and curl static on an old system, then builds and installs beige.
