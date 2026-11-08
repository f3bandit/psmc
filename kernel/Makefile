# MC/PS kernel: our own PocketStation BIOS (16 KiB).
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS = -mcpu=arm7tdmi -marm -Os -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -fno-common -fno-pic -fno-delete-null-pointer-checks --param=min-pagesize=0
OBJS = start.o kernel.o gui.o font.o

mcps-kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c %s $@); if [ $$size -gt 16384 ]; then echo "too big: $$size"; exit 1; fi; \
	 truncate -s 16384 $@; echo "mcps-kernel.bin: $$size bytes used of 16384"

kernel.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) -T link.ld $(OBJS) -lgcc -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c hw.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o kernel.elf mcps-kernel.bin
