CC := gcc
CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude
SRC := src/rbtree.c
TSRC := tests/test_rbtree.c
DTSRC := tests/test_delete.c
BIN := build/test_rbtree
DBIN := build/test_delete
FUZZBIN := build/fuzz
all: $(BIN) $(DBIN) $(FUZZBIN)
$(BIN): $(SRC) $(TSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@
$(DBIN): $(SRC) $(DTSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(DTSRC) -o $@
$(FUZZBIN): $(SRC) tests/fuzz.c include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c -o $@
test: $(BIN) $(DBIN) $(FUZZBIN)
	./$(BIN) && ./$(DBIN) && ./$(FUZZBIN) 100000
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test
memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(DBIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FUZZBIN) 20000
clean:
	rm -rf build
.PHONY: all test asan memcheck clean
