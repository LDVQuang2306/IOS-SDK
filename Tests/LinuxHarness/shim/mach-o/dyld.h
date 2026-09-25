#pragma once
#include <stdint.h>
#include "loader.h"
#ifdef __cplusplus
extern "C" {
#endif
uint32_t _dyld_image_count(void);
const struct mach_header* _dyld_get_image_header(uint32_t);
const char* _dyld_get_image_name(uint32_t);
intptr_t _dyld_get_image_vmaddr_slide(uint32_t);
#ifdef __cplusplus
}
#endif
