kilo: kilo.c abuf.c
	$(CC) kilo.c abuf.c -o kilo -g -Wall -Wextra -pedantic -std=c17

gdb: kilo .gdbinit
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)
