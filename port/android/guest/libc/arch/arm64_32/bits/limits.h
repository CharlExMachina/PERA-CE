#ifdef HALO_MACOS
/* Apple silicon's pages (port/macos/README.md): mallocng maps and unmaps
whole ones */
#define PAGESIZE 16384
#else
#define PAGESIZE 4096
#endif
#define LONG_BIT 32
