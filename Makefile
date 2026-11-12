# Makefile - builds the MapReduce library, the example applications and the tests.
#
#   make              build libmr.a, all examples and all tests
#   make test         run every test program and every example (self-checking)
#   make asan         rebuild with AddressSanitizer + UBSan and run everything
#   make tsan         rebuild with ThreadSanitizer and run the thread tests
#   make valgrind     run the tests under Valgrind (leak check)
#   make analysis     run the worker analysis (examples/benchmark)
#   make sim          build the separate simulation (see simulation/README.md)
#   make sim-test     build and test the simulation
#   make clean        remove everything that was built (also in simulation/, if it exists)
#
# Folder programs (need a folder argument, so they are not part of "make test"):
#   make examples/word_count_folder examples/word_frequency_folder
#   ./examples/word_count_folder <folder> [max-mappers] [reducers] [-w word] [-o file]
#   ./examples/word_frequency_folder <folder> [max-mappers] [reducers] [-w word] [-o file]

CC      ?= gcc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -pedantic -Iinclude
LDLIBS   = -pthread

LIB_OBJS = src/mr_error.o src/mr_kv.o src/mr_split.o src/mr_partition.o \
           src/mr_pool.o src/mr_map.o src/mr_shuffle.o src/mr_reduce.o src/mr_job.o

EXAMPLES = examples/word_count examples/inverted_index examples/word_frequency \
           examples/max_per_key examples/matrix_multiply examples/benchmark \
           examples/word_count_folder examples/word_frequency_folder

TESTS    = tests/test_kv tests/test_split tests/test_partition tests/test_job \
           tests/test_threads tests/test_errors tests/test_memory tests/test_large \
           tests/test_alloc_fail

.PHONY: all test test-units test-examples asan tsan valgrind analysis sim sim-test clean

all: $(EXAMPLES) $(TESTS)

# ---- the library: nine source files packed into one archive ---------------
libmr.a: $(LIB_OBJS)
	ar rcs $@ $^

src/%.o: src/%.c include/mapreduce.h src/mr_internal.h
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

# ---- examples and tests: one .c file each, linked with the library --------
examples/%: examples/%.c examples/app_util.h libmr.a
	$(CC) $(CFLAGS) $< -L. -lmr $(LDLIBS) -lm -o $@

tests/%: tests/%.c tests/test_util.h tests/test_common.h libmr.a
	$(CC) $(CFLAGS) $< -L. -lmr $(LDLIBS) -o $@

# test_alloc_fail makes allocations fail on purpose: it needs the linker's --wrap option.
tests/test_alloc_fail: tests/test_alloc_fail.c tests/test_util.h tests/test_common.h libmr.a
	$(CC) $(CFLAGS) $< -L. -lmr $(LDLIBS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -o $@

# ---- running ------------------------------------------------------------------
test: test-units test-examples

test-units: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t"; ./$$t || exit 1; done
	@echo; echo "ALL TEST PROGRAMS PASSED"

# The examples check their own answer and return 1 if it is wrong.
test-examples: $(EXAMPLES)
	@for e in word_count inverted_index word_frequency max_per_key; do \
	    echo "=== examples/$$e"; ./examples/$$e > /dev/null || exit 1; \
	    ./examples/$$e - 4 3 > /dev/null || exit 1; done
	./examples/matrix_multiply > /dev/null
	./examples/matrix_multiply 7 5 6 4 3 > /dev/null
	./examples/matrix_multiply 30 20 25 4 4 > /dev/null
	./examples/benchmark 100000 1 > /dev/null
	@echo; echo "ALL EXAMPLES PASSED"

# Sanitizers: rebuild everything with the sanitizer, run, clean up again.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test
	$(MAKE) clean

tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=thread" LDLIBS="-pthread -fsanitize=thread" \
	    tests/test_job tests/test_threads tests/test_errors tests/test_memory examples/word_count examples/matrix_multiply
	@for t in test_job test_threads test_errors test_memory; do echo "=== $$t"; ./tests/$$t > /dev/null || exit 1; done
	./examples/word_count - 4 4 > /dev/null
	./examples/matrix_multiply 12 8 10 4 4 > /dev/null
	@echo; echo "THREAD SANITIZER: NO RACES REPORTED"
	$(MAKE) clean

valgrind: $(TESTS)
	@for t in test_kv test_split test_partition test_job test_errors test_memory test_alloc_fail; do \
	    echo "=== valgrind $$t"; \
	    valgrind -q --leak-check=full --error-exitcode=9 ./tests/$$t > /dev/null || exit 1; done
	@echo; echo "VALGRIND: NO ERRORS, NO LEAKS"

analysis: examples/benchmark
	./examples/benchmark

# ---- the separate simulation (own Makefile, own header, own library) --------
sim:
	$(MAKE) -C simulation
sim-test:
	$(MAKE) -C simulation test

# "make clean" no longer stops when the simulation/ folder is missing.
clean:
	rm -f $(LIB_OBJS) libmr.a $(EXAMPLES) $(TESTS)
	@if [ -d simulation ]; then $(MAKE) -C simulation clean; fi