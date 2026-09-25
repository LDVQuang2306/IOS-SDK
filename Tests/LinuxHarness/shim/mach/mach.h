#pragma once
/* Minimal Mach shim so the dumper engine can run on Linux against a synthetic process image. */
#include <stdint.h>
#include <stddef.h>
typedef int kern_return_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef unsigned int mach_port_t;
typedef int vm_prot_t;
typedef unsigned int mach_msg_type_number_t;
typedef mach_port_t memory_object_name_t;
typedef int* vm_region_info_t;
typedef int vm_region_flavor_t;
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_PROTECTION_FAILURE 2
#define KERN_INVALID_ARGUMENT 4
#define KERN_MEMORY_FAILURE 9
#define KERN_MEMORY_ERROR 10
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
typedef struct { vm_prot_t protection; vm_prot_t max_protection; unsigned int inheritance; int shared; int reserved; unsigned long long offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
#define VM_REGION_BASIC_INFO_64 9
#ifdef __cplusplus
extern "C" {
#endif
mach_port_t mach_task_self(void);
kern_return_t vm_read_overwrite(mach_port_t, vm_address_t, vm_size_t, vm_address_t, vm_size_t*);
kern_return_t vm_region_64(mach_port_t, vm_address_t*, vm_size_t*, vm_region_flavor_t, vm_region_info_t, mach_msg_type_number_t*, memory_object_name_t*);
#ifdef __cplusplus
}
#endif
