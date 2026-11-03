CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
.PHONY: all clean test examples
all: spc
spc: compiler/spc.c
	$(CC) $(CFLAGS) -o $@ $< -lm
examples: spc
	./spc examples/sys_demo.sp --run
	./spc examples/vector_demo.sp --run
test: spc
	./spc tests/flags_test.sp --run
	./spc examples/sys_demo.sp --run
	./spc examples/vector_demo.sp --run
clean:
	rm -f spc examples/sys_demo examples/sys_demo.c examples/vector_demo examples/vector_demo.c
	rm -f spark_syscall_test.txt spark-vector-demo.svg flags_test.txt tests/flags_test tests/flags_test.c
