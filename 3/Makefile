GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant
GCC_DEBUG_FLAGS = -ggdb

all: test

test: userfs.c test.c
	gcc $(GCC_FLAGS) userfs.c test.c ../utils/unit.c -I ../utils -o test

debug:
	gcc $(GCC_FLAGS) $(GCC_DEBUG_FLAGS) userfs.c test.c ../utils/unit.c -I ../utils -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

memcheck:
	gcc $(GCC_FLAGS_MEM) userfs.c custom_test.c ../utils/unit.c ../utils/heap_help/heap_help.c -I ../utils -o memcheck -ldl -rdynamic

clean:
	rm -f test memcheck
