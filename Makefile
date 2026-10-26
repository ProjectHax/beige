CC ?= gcc
CFLAGS ?= -Wall -Wextra
# Added to any CFLAGS from the environment or the command line; beige needs them to build.
BEIGE_CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
CPPFLAGS += -Ithird_party -Isrc
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS ?= -lcurl

SRCS = \
	src/util.c \
	src/perm.c \
	src/tools.c \
	src/transcript.c \
	src/http.c \
	src/config.c \
	src/context.c \
	src/session.c \
	src/provider.c \
	src/provider_anthropic.c \
	src/provider_openai.c \
	src/oauth.c \
	src/models.c \
	src/signals.c \
	src/glyph.c \
	src/agent.c \
	src/tui.c \
	src/main.c

OBJS = $(SRCS:.c=.o)

# Every object depends on every header: a header change rebuilds all, which is quick at this size.
HDRS = $(wildcard src/*.h) third_party/cJSON.h

# Sources linked into each test, used as both prerequisites and link inputs so the two cannot drift.
NET_SRCS = src/tools.c src/util.c src/http.c src/signals.c
PROVIDER_SRCS = src/provider.c src/provider_anthropic.c src/provider_openai.c src/transcript.c
TEST_PERM_SRCS = tests/test_perm.c src/perm.c src/util.c
TEST_PATH_SRCS = tests/test_path.c src/util.c
TEST_EDIT_SRCS = tests/test_edit.c $(NET_SRCS)
TEST_BASH_SRCS = tests/test_bash.c $(NET_SRCS)
TEST_CONTEXT_SRCS = tests/test_context.c src/context.c src/util.c
TEST_SSE_SRCS = tests/test_sse.c $(PROVIDER_SRCS) $(NET_SRCS)
TEST_MODELS_SRCS = tests/test_models.c src/models.c $(PROVIDER_SRCS) $(NET_SRCS)
TEST_TEXT_SRCS = tests/test_text.c src/glyph.c src/util.c

TESTS = tests/test_perm tests/test_path tests/test_edit tests/test_bash tests/test_context tests/test_sse \
	tests/test_models tests/test_text

COMPILE = $(CC) $(BEIGE_CFLAGS) $(CFLAGS) $(CPPFLAGS)

.PHONY: all test clean

all: beige

beige: $(OBJS) third_party/cJSON.o
	$(CC) $(BEIGE_CFLAGS) $(CFLAGS) -o $@ $(OBJS) third_party/cJSON.o $(CURL_LIBS)

$(OBJS): $(HDRS)

src/%.o: src/%.c
	$(COMPILE) $(CURL_CFLAGS) -c -o $@ $<

third_party/cJSON.o: third_party/cJSON.c third_party/cJSON.h
	$(CC) -std=c99 -Wall -Wno-unused-function -O2 -c -o $@ $<

$(TESTS): $(HDRS) tests/check.h

tests/test_perm: $(TEST_PERM_SRCS)
	$(COMPILE) -o $@ $(TEST_PERM_SRCS)

tests/test_path: $(TEST_PATH_SRCS)
	$(COMPILE) -o $@ $(TEST_PATH_SRCS)

tests/test_edit: $(TEST_EDIT_SRCS) third_party/cJSON.o
	$(COMPILE) $(CURL_CFLAGS) -o $@ $(TEST_EDIT_SRCS) third_party/cJSON.o $(CURL_LIBS)

tests/test_bash: $(TEST_BASH_SRCS) third_party/cJSON.o
	$(COMPILE) $(CURL_CFLAGS) -o $@ $(TEST_BASH_SRCS) third_party/cJSON.o $(CURL_LIBS)

tests/test_context: $(TEST_CONTEXT_SRCS)
	$(COMPILE) -o $@ $(TEST_CONTEXT_SRCS)

tests/test_sse: $(TEST_SSE_SRCS) third_party/cJSON.o
	$(COMPILE) $(CURL_CFLAGS) -o $@ $(TEST_SSE_SRCS) third_party/cJSON.o $(CURL_LIBS)

tests/test_models: $(TEST_MODELS_SRCS) third_party/cJSON.o
	$(COMPILE) $(CURL_CFLAGS) -o $@ $(TEST_MODELS_SRCS) third_party/cJSON.o $(CURL_LIBS)

tests/test_text: $(TEST_TEXT_SRCS)
	$(COMPILE) -o $@ $(TEST_TEXT_SRCS)

test: beige $(TESTS)
	./tests/test_perm
	./tests/test_path
	./tests/test_edit
	./tests/test_bash
	./tests/test_context
	./tests/test_sse
	./tests/test_models
	./tests/test_text
	BEIGE_BIN="$(CURDIR)/beige" sh tests/integration.sh
	BEIGE_BIN="$(CURDIR)/beige" sh tests/test_slash.sh

clean:
	rm -f beige src/*.o third_party/cJSON.o $(TESTS)
