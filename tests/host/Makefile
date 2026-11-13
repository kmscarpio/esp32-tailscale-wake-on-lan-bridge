CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

test: test_wol_core
	./test_wol_core

test_wol_core: test_wol_core.c ../../main/wol_core.c ../../main/wol_core.h
	$(CC) $(CFLAGS) -o $@ test_wol_core.c ../../main/wol_core.c

clean:
	rm -f test_wol_core
