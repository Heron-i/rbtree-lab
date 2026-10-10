CC := gcc-14
BASE_CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude -Itests
# test builds route rb_malloc/rb_free through the fault injector
CFLAGS := $(BASE_CFLAGS) -DRB_FAULT_INJECT
SRC := src/rbtree.c tests/fault_alloc.c
TSRC := tests/test_rbtree.c
DTSRC := tests/test_delete.c
CRSRC := tests/test_create.c
DESRC := tests/test_destroy.c
FISRC := tests/test_find.c
FESRC := tests/test_foreach.c
INSRC := tests/test_insert.c
SISRC := tests/test_size.c
BIN := build/test_rbtree
DBIN := build/test_delete
CRBIN := build/test_create
DEBIN := build/test_destroy
FIBIN := build/test_find
FEBIN := build/test_foreach
INBIN := build/test_insert
SIBIN := build/test_size
FUZZBIN := build/fuzz
SWEEPBIN := build/fault_sweep
PRODOBJ := build/rbtree_prod.o
# pool: tested on its own until the tree is wired to it
POOLSRC := src/pool.c tests/fault_alloc.c tests/test_pool_geom.c
POOLBIN := build/test_pool_geom
PLSRC := src/pool.c tests/fault_alloc.c tests/test_pool.c
PLBIN := build/test_pool
POOLPRODOBJ := build/pool_prod.o
all: $(BIN) $(DBIN) $(CRBIN) $(DEBIN) $(FIBIN) $(FEBIN) $(INBIN) $(SIBIN) $(FUZZBIN) $(SWEEPBIN) $(POOLBIN) $(PLBIN) $(PRODOBJ) $(POOLPRODOBJ)
# compile-only: keeps the production (malloc-forwarding) seam -Werror clean
$(PRODOBJ): src/rbtree.c include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(BASE_CFLAGS) -c src/rbtree.c -o $@
$(POOLPRODOBJ): src/pool.c src/pool.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(BASE_CFLAGS) -c src/pool.c -o $@
$(POOLBIN): $(POOLSRC) src/pool.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(POOLSRC) -o $@
$(PLBIN): $(PLSRC) src/pool.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(PLSRC) -o $@
$(BIN): $(SRC) $(TSRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@
$(DBIN): $(SRC) $(DTSRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(DTSRC) -o $@
$(CRBIN): $(SRC) $(CRSRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(CRSRC) -o $@
$(DEBIN): $(SRC) $(DESRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(DESRC) -o $@
$(FIBIN): $(SRC) $(FISRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(FISRC) -o $@
$(FEBIN): $(SRC) $(FESRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(FESRC) -o $@
$(INBIN): $(SRC) $(INSRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(INSRC) -o $@
$(SIBIN): $(SRC) $(SISRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(SISRC) -o $@
$(FUZZBIN): $(SRC) tests/fuzz.c include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c -o $@
$(SWEEPBIN): $(SRC) tests/fault_sweep.c include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fault_sweep.c -o $@
test: $(BIN) $(DBIN) $(CRBIN) $(DEBIN) $(FIBIN) $(FEBIN) $(INBIN) $(SIBIN) $(FUZZBIN) $(SWEEPBIN) $(POOLBIN) $(PLBIN)
	./$(BIN) && ./$(DBIN) && ./$(CRBIN) && ./$(DEBIN) && ./$(FIBIN) && ./$(FEBIN) && ./$(INBIN) && ./$(SIBIN) && ./$(FUZZBIN) 100000 && ./$(SWEEPBIN) && ./$(POOLBIN) && ./$(PLBIN)
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test
memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(DBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(CRBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(DEBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FIBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FEBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(INBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(SIBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FUZZBIN) 20000
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(SWEEPBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(POOLBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(PLBIN)
clean:
	rm -rf build
.PHONY: all test asan memcheck clean
