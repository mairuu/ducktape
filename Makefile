# the CFLAGS sentinel rule below is physically the first target in this file,
# so it would otherwise become the default goal — and since its recipe is
# entirely @-silenced, a bare `make` would print nothing, build nothing, and
# still exit 0. Name the real default explicitly.
.DEFAULT_GOAL := all

DEFS :=

TARGET  := ducktape
CC      := cc
CSTD    := -std=c23
# -Wno-overlength-strings: `build/std_data.h` embeds each std module as one
# string literal, and ISO only requires 4095 characters to be supported. Every
# compiler we build with handles more; the alternative would be chopping a std
# module up to please a minimum nobody enforces.
CFLAGS  := $(CSTD) -Wall -Wextra -Wpedantic -Wno-overlength-strings $(DEFS)
IFLAGS  := -Iinclude -Ibuild
LDFLAGS :=
LIBS    := -lm

# build type: make build=release  (default: debug)
BUILD    ?= debug
ifeq ($(BUILD),release)
    CFLAGS += -O2 -DNDEBUG
else
    CFLAGS += -g -O0 -DDEBUG
endif

# paths
SRCDIR   := src
BUILDDIR := build
DEPDIR   := $(BUILDDIR)/.deps

# sources & objects
SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
DEPS     := $(SRCS:$(SRCDIR)/%.c=$(DEPDIR)/%.d)

# the standard library: ducktape sources mirrored into the binary. The .dt
# files are the source of truth and stay directly runnable; std_data.h is a
# generated fragment src/std_src.c includes.
STDDIR   := std
# std nests, so the prerequisite list has to as well — one level of directory
# is all the library uses, and a glob that misses a file silently mirrors a
# stale copy of it into the binary.
STDSRCS  := $(wildcard $(STDDIR)/*.dt) $(wildcard $(STDDIR)/*/*.dt)
STDDATA  := $(BUILDDIR)/std_data.h

# compile_commands.json
CCJSON   := compile_commands.json
CCJSON_ENTRIES :=

# cflags sentinel: recompile everything when flags change
CFLAGS_SENTINEL := $(DEPDIR)/.cflags
CFLAGS_CURRENT  := $(CC)|$(CFLAGS)|$(IFLAGS)

# force the sentinel to be checked/updated before any compile
$(CFLAGS_SENTINEL): FORCE
	@mkdir -p $(DEPDIR)
	@printf '%s' '$(CFLAGS_CURRENT)' > $@.tmp; \
	 if ! cmp -s $@.tmp $@ 2>/dev/null; then \
	   mv $@.tmp $@; \
	   echo "flags changed -- triggering full recompile"; \
	 else \
	   rm $@.tmp; \
	 fi

.PHONY: FORCE
FORCE:

# default target
.PHONY: all
all: $(CCJSON) $(BUILDDIR)/$(TARGET)

# link
$(BUILDDIR)/$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "linked  --> $@"

# compile  (depends on sentinel so flag changes force recompile)
$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(CFLAGS_SENTINEL)
	@mkdir -p $(BUILDDIR) $(DEPDIR)
	$(CC) $(CFLAGS) $(IFLAGS) -MMD -MF $(DEPDIR)/$*.d -c $< -o $@
	@echo "compiled $<"

# regenerate the embedded std whenever a .dt changes. The script rewrites the
# header only when the content differs, so an untouched std does not cascade a
# rebuild. Stated explicitly because -MMD can only discover the dependency
# after a build in which the header already existed.
$(STDDATA): $(STDSRCS) scripts/embed_std.sh
	@mkdir -p $(BUILDDIR)
	@sh scripts/embed_std.sh $(STDDIR) $@
	@echo "embedded std --> $@"

$(BUILDDIR)/std_src.o: $(STDDATA)

-include $(DEPS)

ABS_ROOT := $(shell pwd)

$(CCJSON): $(SRCS) Makefile
	@echo "generating $@"
	@python3 scripts/gen_ccjson.py \
	    --root     "$(ABS_ROOT)"   \
	    --srcdir   "$(SRCDIR)"     \
	    --builddir "$(BUILDDIR)"   \
	    --cc       "$(CC)"         \
	    --cflags   "$(CFLAGS) $(IFLAGS)"

.PHONY: run
run: all
	./$(BUILDDIR)/$(TARGET)

.PHONY: test
test: all
	@sh scripts/run_tests.sh $(BUILDDIR)/$(TARGET)

# the suite under AddressSanitizer + UndefinedBehaviorSanitizer. Both have
# caught real bugs the plain build cannot see — a per-instantiation chunk being
# overwritten (milestone 23), and a zero-length array described by a NULL
# sentinel instead of a count (post-24) — and neither shows up as a test
# failure, so this has to be its own run rather than something `test` does.
# It rebuilds from scratch: the sanitizer flags change every object file.
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: sanitize
sanitize:
	@$(MAKE) clean
	@$(MAKE) DEFS="$(SANFLAGS)" LDFLAGS="$(SANFLAGS)"
	@sh scripts/run_tests.sh $(BUILDDIR)/$(TARGET)

.PHONY: clean
clean:
	$(RM) -r $(BUILDDIR) $(CCJSON)
	@echo "cleaned."

.PHONY: format
format:
	clang-format -i $(SRCS) $(wildcard include/*.h)

.PHONY: tidy
tidy: $(CCJSON)
	clang-tidy $(SRCS) -- $(CFLAGS) $(IFLAGS)

.PHONY: bear
bear:
	bear -- $(MAKE) clean all

.PHONY: help
help:
	@echo "targets:"
	@echo "  all     - build $(TARGET) and generate compile_commands.json (default)"
	@echo "  run     - build and run"
	@echo "  test    - build and run the test suite"
	@echo "  sanitize- rebuild under ASan+UBSan and run the suite"
	@echo "  clean   - remove build artifacts and compile_commands.json"
	@echo "  format  - run clang-format over all sources"
	@echo "  tidy    - run clang-tidy (requires compile_commands.json)"
	@echo "  bear    - use bear to regenerate compile_commands.json"
	@echo ""
	@echo "variables:"
	@echo "  BUILD=debug|release  (default: debug)"
	@echo "  CC=<compiler>        (default: cc)"
