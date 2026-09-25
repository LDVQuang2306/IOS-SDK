#pragma once
#include <stdint.h>
#include "../mach/mach.h"
struct mach_header_64 { uint32_t magic; int32_t cputype; int32_t cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; uint32_t reserved; };
struct mach_header { uint32_t magic; int32_t cputype; int32_t cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; };
struct load_command { uint32_t cmd; uint32_t cmdsize; };
struct segment_command_64 { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t fileoff; uint64_t filesize; int32_t maxprot; int32_t initprot; uint32_t nsects; uint32_t flags; };
#define MH_MAGIC_64 0xfeedfacf
#define MH_EXECUTE 0x2
#define MH_DYLIB 0x6
#define LC_SEGMENT_64 0x19
#define LC_UUID 0x1b
#define CPU_TYPE_ARM64 ((int32_t)0x0100000c)
