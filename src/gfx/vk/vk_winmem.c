/* What the memory under an address range is, for the log when the GPU cannot import it as host memory. */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

void vk_describe_range(uintptr_t lo, uintptr_t hi, char *out, size_t cap) {
    uint64_t pages[5] = {0};   /* private read/write, mapped view, image, committed but no access / guard, reserved or free */
    uint32_t regions = 0;
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize; if (end > hi) end = hi;
        uint64_t n = (end - a) >> 12;
        int k = mi.State != MEM_COMMIT ? 4 : (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ? 3 : mi.Type == MEM_MAPPED ? 1 : mi.Type == MEM_IMAGE ? 2 : 0;
        pages[k] += n; regions++;
        a = end;
    }
    snprintf(out, cap, "%u regions: %llu pages private, %llu mapped view, %llu image, %llu no-access, %llu reserved/free",
             regions, (unsigned long long)pages[0], (unsigned long long)pages[1], (unsigned long long)pages[2], (unsigned long long)pages[3], (unsigned long long)pages[4]);
}
