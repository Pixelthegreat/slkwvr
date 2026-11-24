DEBUG ?= y
ASAN ?= n
CC ?= cc
CFLAGS += -c -std=c99
LDFLAGS +=

ifeq ($(DEBUG), y)
CFLAGS += -g
LDFLAGS += -g
endif
ifeq ($(ASAN), y)
CFLAGS += -fsanitize=address -g3
LDFLAGS += -fsanitize=address -g3
endif

TESTS += condition float global loop slkwvr
.PHONY: all test $(TESTS:%=test_%) example

all: slkwvr

slkwvr: slkwvr.o
	@echo LD $@
	@$(CC) -o $@ $< $(LDFLAGS)

slkwvr.o: slkwvr.c
	@echo CC $<
	@$(CC) $(CFLAGS) $<

test: $(TESTS:%=test_%)

$(TESTS:%=test_%): %:
	@mkdir -pv build
	@echo TEST $(@:test_%=%)
	@./slkwvr test/$(@:test_%=%).sw build/test-$(@:test_%=%).qbe
	@qbe -o build/test-$(@:test_%=%).s build/test-$(@:test_%=%).qbe
	@cc -o build/test-$(@:test_%=%) build/test-$(@:test_%=%).s -g
	@./build/test-$(@:test_%=%)

example:
	@mkdir -pv build
	@echo EXAMPLE $(name)
	@./slkwvr examples/$(name).sw build/$(name).qbe
	@qbe -o build/$(name).s build/$(name).qbe
	@cc -o build/$(name) build/$(name).s
	@./build/$(name)
