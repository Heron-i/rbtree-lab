CC := gcc
CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude
SRC := src/rbtree.c
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
all: $(BIN) $(DBIN) $(CRBIN) $(DEBIN) $(FIBIN) $(FEBIN) $(INBIN) $(SIBIN) $(FUZZBIN)
$(BIN): $(SRC) $(TSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@
$(DBIN): $(SRC) $(DTSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(DTSRC) -o $@
$(CRBIN): $(SRC) $(CRSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(CRSRC) -o $@
$(DEBIN): $(SRC) $(DESRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(DESRC) -o $@
$(FIBIN): $(SRC) $(FISRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(FISRC) -o $@
$(FEBIN): $(SRC) $(FESRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(FESRC) -o $@
$(INBIN): $(SRC) $(INSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(INSRC) -o $@
$(SIBIN): $(SRC) $(SISRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(SISRC) -o $@
$(FUZZBIN): $(SRC) tests/fuzz.c include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c -o $@
test: $(BIN) $(DBIN) $(CRBIN) $(DEBIN) $(FIBIN) $(FEBIN) $(INBIN) $(SIBIN) $(FUZZBIN)
	./$(BIN) && ./$(DBIN) && ./$(CRBIN) && ./$(DEBIN) && ./$(FIBIN) && ./$(FEBIN) && ./$(INBIN) && ./$(SIBIN) && ./$(FUZZBIN) 100000
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
clean:
	rm -rf build
.PHONY: all test asan memcheck clean
