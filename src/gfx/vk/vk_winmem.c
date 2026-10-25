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

/* The regions of a range one by one (the first `max`), with state, protection and type: for the log. */
void vk_list_regions(uintptr_t lo, uintptr_t hi, int max, void (*line)(const char *)) {
    int n = 0;
    for (uintptr_t a = lo; a < hi && n < max; n++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize; if (end > hi) end = hi;
        char t[200];
        snprintf(t, sizeof t, "  region %p..%p (%.1f MiB): state %#lx protect %#lx type %#lx allocation base %p", (void *)a, (void *)end, (end - a) / 1048576.0,
                 (unsigned long)mi.State, (unsigned long)mi.Protect, (unsigned long)mi.Type, mi.AllocationBase);
        line(t);
        a = end;
    }
}
int vk_under_wine(void) { HMODULE h = GetModuleHandleA("ntdll.dll"); return h && GetProcAddress(h, "wine_get_version") != NULL; }
