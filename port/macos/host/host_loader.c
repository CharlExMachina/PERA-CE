/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code and rebased onto the window, tools/macos_build.py) whose
segments are copied to the guest addresses it was linked at, in the window.
The image starts with a struct halo_guest_header naming its import table,
which is filled with the host functions of the same names (the import stubs
branch to these 64-bit addresses directly).

As on Android (port/android/host/host_loader.c), the code is then made
read-only and executable. This process does not use the hardened runtime,
under which its own memory could not become executable.
*/

#include "host.h"

#include <libkern/OSCacheControl.h>
#include <string.h>
#include <sys/mman.h>

/* the ELF definitions used here (macOS has no <elf.h>) */
#define ELF_MAGIC "\177ELF"
#define ELF_CLASS_64 2
#define ELF_MACHINE_AARCH64 183
#define ELF_TYPE_EXECUTABLE 2
#define ELF_SEGMENT_LOAD 1
#define ELF_SEGMENT_EXECUTE 1

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

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

int host_load_image(const void *file, size_t size)
{
	const struct elf_header *elf = file;
	const struct elf_segment *segments;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->identification, ELF_MAGIC, 4) || elf->identification[4] != ELF_CLASS_64 ||
		elf->machine != ELF_MACHINE_AARCH64 || elf->type != ELF_TYPE_EXECUTABLE ||
		elf->segment_offset + (uint64_t)elf->segment_count * sizeof(struct elf_segment) > size)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const struct elf_segment *)((const char *)file + elf->segment_offset);
	for (index = 0; index < elf->segment_count; index++)
	{
		if (segments[index].type != ELF_SEGMENT_LOAD)
			continue;
		if (segments[index].address < low)
			low = segments[index].address;
		if (segments[index].address + segments[index].memory_size > high)
			high = segments[index].address + segments[index].memory_size;
	}
	low &= ~(HOST_PAGE - 1);
	high = (high + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
	if (low != HALO_GUEST_IMAGE_BASE || high > (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;
	for (index = 0; index < elf->segment_count; index++)
	{
		const struct elf_segment *segment = &segments[index];

		if (segment->type != ELF_SEGMENT_LOAD)
			continue;
		if (segment->offset + segment->file_size > size)
			return -1;
		memcpy(host_window + segment->address, (const char *)file + segment->offset, segment->file_size);
	}

	header = GUEST_TO_HOST(const struct halo_guest_header *, low);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	table = GUEST_TO_HOST(uint64_t *, header->import_table);
	name = GUEST_TO_HOST(const char *, header->import_names);
	count = *GUEST_TO_HOST(const uint32_t *, header->import_count);
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < elf->segment_count; index++)
	{
		const struct elf_segment *segment = &segments[index];
		uint64_t start = segment->address & ~(HOST_PAGE - 1);
		uint64_t end = (segment->address + segment->memory_size + HOST_PAGE - 1) & ~(HOST_PAGE - 1);

		if (segment->type == ELF_SEGMENT_LOAD && (segment->flags & ELF_SEGMENT_EXECUTE))
		{
			if (mprotect(host_window + start, end - start, PROT_READ | PROT_EXEC) != 0)
			{
				host_logf(HOST_LOG_ERROR, "cannot make the guest's code executable");
				return -1;
			}
			sys_icache_invalidate(host_window + start, end - start);
		}
	}
	return 0;
}
