DEFS :=

TARGET  := ducktape 
CC      := cc
CSTD    := -std=c23
CFLAGS  := $(CSTD) -Wall -Wextra -Wpedantic $(DEFS)
IFLAGS  := -Iinclude
LDFLAGS :=
LIBS    :=

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
	@echo "  clean   - remove build artifacts and compile_commands.json"
	@echo "  format  - run clang-format over all sources"
	@echo "  tidy    - run clang-tidy (requires compile_commands.json)"
	@echo "  bear    - use bear to regenerate compile_commands.json"
	@echo ""
	@echo "variables:"
	@echo "  BUILD=debug|release  (default: debug)"
	@echo "  CC=<compiler>        (default: cc)"
