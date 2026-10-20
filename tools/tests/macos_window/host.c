/*
HOST.C

The smallest host for a guest built as the macOS port's is
(tools/test_macos_asm_rebase.py): reserves a window of 4 GB at a 4 GB
boundary, copies the image there, fills its one import and runs its entry
on a stack in the window, with the window's base in x28.
*/

#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct elf_header
{
	unsigned char identification[16];
	uint16_t type, machine;
	uint32_t version;
	uint64_t entry, segment_offset, section_offset;
	uint32_t flags;
	uint16_t header_size, segment_entry_size, segment_count, section_entry_size, section_count, section_names;
};

struct elf_segment
{
	uint32_t type, flags;
	uint64_t offset, address, physical_address, file_size, memory_size, alignment;
};

static uint8_t *window;

/* the import: the guest's pointer, made a host one by its stub */
static void host_print(const char *text, int value)
{
	printf("%s=%d\n", text, value);
}

uint32_t guest_call(uint8_t *window, uint64_t function, uint64_t stack);
__asm__(
	".globl _guest_call\n_guest_call:\n"
	"	stp x29, x30, [sp, #-48]!\n	mov x29, sp\n	stp x27, x28, [sp, #16]\n	str x19, [sp, #32]\n"
	"	mov x19, sp\n	mov x28, x0\n	add x27, x28, w1, uxtw\n	mov sp, x2\n	blr x27\n	mov sp, x19\n"
	"	ldr x19, [sp, #32]\n	ldp x27, x28, [sp, #16]\n	ldp x29, x30, [sp], #48\n	ret\n");

int main(int argc, char **argv)
{
	static uint8_t file[1 << 22];
	FILE *input = fopen(argv[1], "rb");
	size_t size = input ? fread(file, 1, sizeof(file), input) : 0;
	const struct elf_header *elf = (const struct elf_header *)file;
	const struct elf_segment *segments = (const struct elf_segment *)(file + elf->segment_offset);
	uint8_t *reservation = mmap(NULL, 12ULL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	uint64_t table = strtoull(argv[2], NULL, 16);
	int index;

	if (argc < 3 || !size || reservation == MAP_FAILED)
		return 2;
	window = (uint8_t *)(((uintptr_t)reservation + (4ULL << 30) + 0xffffffffULL) & ~0xffffffffULL);
	if (mmap(window + 0x88000000u, 0x400000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) ==
		MAP_FAILED || mmap(window + 0x90000000u, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
		-1, 0) == MAP_FAILED)
		return 2;
	(void)argc;
	for (index = 0; index < elf->segment_count; index++)
	{
		if (segments[index].type == 1)
			memcpy(window + segments[index].address, file + segments[index].offset, segments[index].file_size);
	}
	*(uint64_t *)(window + table) = (uint64_t)(uintptr_t)host_print;
	for (index = 0; index < elf->segment_count; index++)
	{
		if (segments[index].type == 1 && (segments[index].flags & 1))
		{
			uint64_t start = segments[index].address & ~0x3fffULL;
			uint64_t end = (segments[index].address + segments[index].memory_size + 0x3fff) & ~0x3fffULL;

			if (mprotect(window + start, end - start, PROT_READ | PROT_EXEC))
				return 2;
			sys_icache_invalidate(window + start, end - start);
		}
	}
	printf("result=%u\n", guest_call(window, (uint32_t)elf->entry, (uint64_t)(uintptr_t)(window + 0x90100000u)));
	return 0;
}
