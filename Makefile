
opts :=
#opts := -D SUPPORT_SINGLE_CORE

ipc-bench: main.c
	gcc -O2 -lm -lpthread main.c -o ipc-bench

.PHONY: clean
clean:
	rm -f ipc-bench
