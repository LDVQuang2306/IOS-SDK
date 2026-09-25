#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

extern "C" mach_port_t mach_task_self(void) { return 1; }

extern "C" kern_return_t vm_read_overwrite(mach_port_t, vm_address_t Address, vm_size_t Size, vm_address_t Out, vm_size_t* OutSize)
{
    struct iovec Local{ reinterpret_cast<void*>(Out), Size };
    struct iovec Remote{ reinterpret_cast<void*>(Address), Size };
    const ssize_t Read = process_vm_readv(getpid(), &Local, 1, &Remote, 1, 0);
    if (Read != static_cast<ssize_t>(Size)) return KERN_INVALID_ADDRESS;
    if (OutSize) *OutSize = Size;
    return KERN_SUCCESS;
}

extern "C" kern_return_t vm_region_64(mach_port_t, vm_address_t* Address, vm_size_t* Size, vm_region_flavor_t, vm_region_info_t Info, mach_msg_type_number_t*, memory_object_name_t*)
{
    FILE* Maps = fopen("/proc/self/maps", "r");
    char Line[512];
    while (fgets(Line, sizeof(Line), Maps))
    {
        unsigned long Begin, End; char Perms[8];
        sscanf(Line, "%lx-%lx %7s", &Begin, &End, Perms);
        if (End > *Address)
        {
            if (Begin > *Address) *Address = Begin;
            *Size = End - *Address;
            auto* Basic = reinterpret_cast<vm_region_basic_info_data_64_t*>(Info);
            Basic->protection = (Perms[0] == 'r' ? VM_PROT_READ : 0) | (Perms[1] == 'w' ? VM_PROT_WRITE : 0) | (Perms[2] == 'x' ? VM_PROT_EXECUTE : 0);
            fclose(Maps);
            return KERN_SUCCESS;
        }
    }
    fclose(Maps);
    return KERN_INVALID_ADDRESS;
}

struct FFakeImage { const mach_header* Header; std::string Name; intptr_t Slide; };
std::vector<FFakeImage> GFakeImages;

extern "C" uint32_t _dyld_image_count(void) { return static_cast<uint32_t>(GFakeImages.size()); }
extern "C" const struct mach_header* _dyld_get_image_header(uint32_t i) { return i < GFakeImages.size() ? GFakeImages[i].Header : nullptr; }
extern "C" const char* _dyld_get_image_name(uint32_t i) { return i < GFakeImages.size() ? GFakeImages[i].Name.c_str() : nullptr; }
extern "C" intptr_t _dyld_get_image_vmaddr_slide(uint32_t i) { return i < GFakeImages.size() ? GFakeImages[i].Slide : 0; }
