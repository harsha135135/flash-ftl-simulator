CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes -Wconversion -Wno-sign-conversion -Werror
UCFLAGS := -std=gnu11 $(WARN) -Isrc $(CFLAGS)
SAN     := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
# Per-OS output directory: the tree is shared between macOS and the Linux VM.
OS      := $(shell uname -s | tr A-Z a-z)
B       := build/$(OS)

CORE    := src/nand.c src/ftl.c src/simalloc.c src/workload.c src/refmodel.c
HDRS    := $(wildcard src/*.h) tests/testutil.h
TESTS   := test_nand test_ftl test_diff

all: $(B)/ftlsim $(B)/gc_trace $(addprefix $(B)/,$(TESTS))

$(B) $(B)/asan:
	mkdir -p $@

$(B)/ftlsim: src/ftlsim.c $(CORE) $(HDRS) | $(B)
	$(CC) $(UCFLAGS) -o $@ src/ftlsim.c $(CORE) -lm

$(B)/gc_trace: tools/gc_trace.c $(CORE) $(HDRS) | $(B)
	$(CC) $(UCFLAGS) -o $@ tools/gc_trace.c $(CORE) -lm

$(B)/test_%: tests/test_%.c $(CORE) $(HDRS) | $(B)
	$(CC) $(UCFLAGS) -o $@ $< $(CORE) -lm

$(B)/asan/test_%: tests/test_%.c $(CORE) $(HDRS) | $(B)/asan
	$(CC) $(UCFLAGS) $(SAN) -o $@ $< $(CORE) -lm

$(B)/asan/ftlsim: src/ftlsim.c $(CORE) $(HDRS) | $(B)/asan
	$(CC) $(UCFLAGS) $(SAN) -o $@ src/ftlsim.c $(CORE) -lm

check: $(addprefix $(B)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(B)/$$t || exit 1; done

# Same tests under AddressSanitizer + UndefinedBehaviorSanitizer (+ LeakSanitizer
# where the platform supports it), plus a short sanitized ftlsim run.
check-asan: $(addprefix $(B)/asan/,$(TESTS)) $(B)/asan/ftlsim
	@for t in test_nand test_ftl; do echo "== asan $$t"; $(B)/asan/$$t || exit 1; done
	@echo "== asan test_diff (10 seeds x 2000 ops; full matrix runs in make check)"; $(B)/asan/test_diff 10 2000
	@echo "== asan ftlsim"; $(B)/asan/ftlsim --blocks 64 --ppb 16 --page-size 512 \
		--spare-blocks 6 --workload hotcold --warmup-x 1 --measure-x 2 >/dev/null

clean:
	rm -rf $(B)

.PHONY: all check check-asan clean
