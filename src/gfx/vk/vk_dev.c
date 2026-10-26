/* Vulkan backend: device, guest memory pools, submissions and their completion, and the gpu_ops table. */
#include "gfx/vk/vk.h"
#include <SDL3/SDL_vulkan.h>
#include "gfx/vk/vk_draw.h"
#include "gfx/vk/vk_shader.h"
#include <stdatomic.h>

#include "frontend/frontend.h"
#include "nvn_dirty.h"
#include "nvn_pool_index.h"

VkInstance vk_inst;
VkPhysicalDevice vk_phys;
VkDevice vk_dev;
VkQueue vk_q;
uint32_t vk_qfam;
VmaAllocator vk_vma;
VkPhysicalDeviceProperties vk_props;
VkSurfaceKHR vk_surface;
pthread_mutex_t vk_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t vk_ticks(void) { uint64_t ns = pal_now_ns(); return ns / 625 * 12 + ns % 625 * 12 / 625; }

/* ---------- instance and device ---------- */
static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 400) VLOG("validation: %s", d->pMessage);
    return VK_FALSE;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int create_instance(int validate) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) { VLOG("no Vulkan loader (vulkan-1.dll): %s", SDL_GetError()); return -1; }
    volkInitializeCustom(gipa);
    uint32_t n = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    const char *ext[16]; uint32_t ne = 0;
    for (uint32_t i = 0; i < n && ne < 15; i++) ext[ne++] = sdl_ext[i];
    if (validate) ext[ne++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "WinTears", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = ne,
                               .ppEnabledExtensionNames = ext, .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &layer};
    VkResult r = vkCreateInstance(&ci, NULL, &vk_inst);
    if (r != VK_SUCCESS && validate) { VLOG("validation layer not available (%d): running without it", (int)r); return create_instance(0); }
    if (r != VK_SUCCESS) { VLOG("vkCreateInstance failed (%d)", (int)r); return -1; }
    volkLoadInstanceOnly(vk_inst);
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT di = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = on_debug};
        VkDebugUtilsMessengerEXT m; vkCreateDebugUtilsMessengerEXT(vk_inst, &di, NULL, &m);
        VLOG("validation layer on (TOTK_VK_VALIDATE)");
    }
    return 0;
}

static const char *const DEV_EXT[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
enum { NDEV_EXT = sizeof DEV_EXT / sizeof *DEV_EXT };

/* A device with Vulkan 1.3, the extensions above and a queue for graphics, compute and the window. Discrete first. */
static int pick_device(void) {
    uint32_t n = 0; vkEnumeratePhysicalDevices(vk_inst, &n, NULL);
    VkPhysicalDevice devs[16]; if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(vk_inst, &n, devs);
    int best = -1, best_score = -1; uint32_t best_q = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
        VkExtensionProperties *e = malloc(ne * sizeof *e); vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, e);
        int ok = p.apiVersion >= VK_API_VERSION_1_3;
        for (int k = 0; k < NDEV_EXT; k++) if (!has_ext(e, ne, DEV_EXT[k])) { ok = 0; VLOG("%s: no %s", p.deviceName, DEV_EXT[k]); }
        free(e);
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        VkQueueFamilyProperties q[16]; if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
        int qf = -1;
        for (uint32_t k = 0; k < nq && qf < 0; k++) {
            VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], k, vk_surface, &pres);
            if ((q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q[k].queueFlags & VK_QUEUE_COMPUTE_BIT) && pres) qf = (int)k;
        }
        if (qf < 0) ok = 0;
        int score = !ok ? -1 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        VLOG("GPU %u: %s (Vulkan %u.%u.%u)%s", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion), ok ? "" : " - not usable");
        if (score > best_score) { best_score = score; best = (int)i; best_q = (uint32_t)qf; }
    }
    if (best < 0) { VLOG("no usable GPU (needs Vulkan 1.3 with %s)", VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME); return -1; }
    vk_phys = devs[best]; vk_qfam = best_q;
    vkGetPhysicalDeviceProperties(vk_phys, &vk_props);
    return 0;
}

static VkDeviceSize g_host_align = 4096;
static int g_fault;   /* VK_EXT_device_fault */
static uint64_t g_import_bytes, g_import_all;   /* every import alive (pools, windows), for the 10-second line */
static int g_aw_live;   /* windows over physical (alias) memory */

/* Before dying of a lost device: what the driver says went wrong (page faults with their GPU addresses, or nothing for
   a hang) and what was given to the GPU. */
void vk_lost(int r) {
    if (r != VK_ERROR_DEVICE_LOST) return;
    static _Atomic int once; if (once++) return;
    VLOG("device lost: %llu imports alive (%.0f MiB), %d windows over physical memory", (unsigned long long)g_import_all, g_import_bytes / 1048576.0, g_aw_live);
    if (!g_fault) { VLOG("device lost: no fault report (VK_EXT_device_fault unavailable)"); return; }
    VkDeviceFaultCountsEXT c = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult q = vkGetDeviceFaultInfoEXT(vk_dev, &c, NULL);
    if (q != VK_SUCCESS && q != VK_INCOMPLETE) { VLOG("device lost: fault report failed (%d)", (int)q); return; }
    c.vendorBinarySize = 0;
    VkDeviceFaultAddressInfoEXT *ai = calloc(c.addressInfoCount + 1, sizeof *ai);
    VkDeviceFaultVendorInfoEXT *vi = calloc(c.vendorInfoCount + 1, sizeof *vi);
    VkDeviceFaultInfoEXT fi = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = ai, .pVendorInfos = vi};
    q = vkGetDeviceFaultInfoEXT(vk_dev, &c, &fi);
    VLOG("device lost: %s (%d addresses, %d vendor records; result %d)", fi.description, (int)c.addressInfoCount, (int)c.vendorInfoCount, (int)q);
    static const char *const kind[] = {"none", "read", "write", "execute", "instruction pointer unknown", "instruction pointer invalid", "instruction pointer fault"};
    for (uint32_t i = 0; ai && i < c.addressInfoCount; i++)
        VLOG("device lost: %s at GPU address %#llx (+-%#llx)", ai[i].addressType < 7 ? kind[ai[i].addressType] : "?",
             (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
    for (uint32_t i = 0; vi && i < c.vendorInfoCount; i++)
        VLOG("device lost: %s (code %#llx, data %#llx)", vi[i].description, (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
    free(ai); free(vi);
}

static int create_device(void) {
    /* Every feature the GPU has, except the robustness ones (bounds checks the game does not need, and they cost). */
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    /* instance divisors above 1 (an extension on a 1.3 device): enabled when the GPU has it */
    uint32_t nex = 0; vkEnumerateDeviceExtensionProperties(vk_phys, NULL, &nex, NULL);
    VkExtensionProperties *ex = malloc((nex ? nex : 1) * sizeof *ex); vkEnumerateDeviceExtensionProperties(vk_phys, NULL, &nex, ex);
    int has_div = has_ext(ex, nex, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME), has_budget = has_ext(ex, nex, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    int has_fault = has_ext(ex, nex, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    free(ex);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT dvf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (has_div) f13.pNext = &dvf;
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    VkPhysicalDeviceFaultFeaturesEXT flt = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (has_fault) { flt.pNext = f.pNext; f.pNext = &flt; }
    vkGetPhysicalDeviceFeatures2(vk_phys, &f);
    flt.deviceFaultVendorBinary = VK_FALSE;
    g_fault = has_fault && flt.deviceFault;
    vk_divisor = has_div && dvf.vertexAttributeInstanceRateDivisor;
    const char *dev_ext[NDEV_EXT + 3]; memcpy(dev_ext, DEV_EXT, sizeof DEV_EXT);
    uint32_t ndev_ext = NDEV_EXT;
    if (vk_divisor) dev_ext[ndev_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    if (has_budget) dev_ext[ndev_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    if (has_fault) dev_ext[ndev_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;   /* why the device was lost (vk_lost) */
    VLOG("device fault reports: %s", g_fault ? "on" : has_fault ? "the driver has the extension but not the feature" : "the driver lacks VK_EXT_device_fault");   /* the 10-second memory line: what the driver says is used and available */
    /* TOTK_VK_ROBUST=1 keeps the GPU's bounds checks (diagnosis: AMD turns an out-of-range access into a lost device) */
    { const char *e = pal_getenv("TOTK_VK_ROBUST"); int rob = e && *e == '1';
      if (!rob) { f.features.robustBufferAccess = VK_FALSE; f13.robustImageAccess = VK_FALSE; }
      VLOG("bounds checks: %s (TOTK_VK_ROBUST)", rob ? "on" : "off"); }
    /* Off although the GPU has them, as nothing here uses them: with buffer device addresses or update-after-bind
       descriptors RADV puts every allocation in every submission, and the kernel then checks each imported range at
       each submission (the Steam Deck's imports are one per pool: replay was 22 ms per submission against 2 on the PC). */
    f12.bufferDeviceAddress = f12.bufferDeviceAddressCaptureReplay = f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f12.descriptorBindingUniformBufferUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
    f12.descriptorBindingStorageImageUpdateAfterBind = f12.descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
    f12.descriptorBindingUniformTexelBufferUpdateAfterBind = f12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
    f13.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_FALSE;
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) { VLOG("the GPU lacks dynamic rendering / synchronization2 / timeline semaphores"); return -1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk_qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = ndev_ext, .ppEnabledExtensionNames = dev_ext};
    VkResult r = vkCreateDevice(vk_phys, &ci, NULL, &vk_dev);
    if (r != VK_SUCCESS) { VLOG("vkCreateDevice failed (%d)", (int)r); return -1; }
    volkLoadDevice(vk_dev);
    vkGetDeviceQueue(vk_dev, vk_qfam, 0, &vk_q);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &hp};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
    vkGetPhysicalDeviceProperties2(vk_phys, &p2);
    vk_texel_align[0] = (uint32_t)p13.uniformTexelBufferOffsetAlignmentBytes; vk_texel_single[0] = p13.uniformTexelBufferOffsetSingleTexelAlignment;
    vk_texel_align[1] = (uint32_t)p13.storageTexelBufferOffsetAlignmentBytes; vk_texel_single[1] = p13.storageTexelBufferOffsetSingleTexelAlignment;
    for (int i = 0; i < 2; i++) if (!vk_texel_align[i]) vk_texel_align[i] = (uint32_t)vk_props.limits.minTexelBufferOffsetAlignment;
    g_host_align = hp.minImportedHostPointerAlignment;
    vk_sb_align = vk_props.limits.minStorageBufferOffsetAlignment > 16 ? (uint32_t)vk_props.limits.minStorageBufferOffsetAlignment : 16;
    VLOG("device %s, driver %#x, imported memory alignment %llu, at most %u allocations, BC %s, ASTC %s",
         vk_props.deviceName, vk_props.driverVersion, (unsigned long long)g_host_align, vk_props.limits.maxMemoryAllocationCount,
         f.features.textureCompressionBC ? "yes" : "no", f.features.textureCompressionASTC_LDR ? "yes" : "no (decoded on upload)");
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo ai = {.physicalDevice = vk_phys, .device = vk_dev, .instance = vk_inst, .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .pVulkanFunctions = &vf, .flags = has_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
    VKC(vmaCreateAllocator(&ai, &vk_vma));
    /* GPUs that share system memory (the Steam Deck, laptops' integrated GPUs) or have little of their own: textures
       there cost RAM, so large uploaded textures leave out their top mip level by default (vk_tex.c, TOTK_MIP_DROP). */
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > vk_local_heap) vk_local_heap = mp.memoryHeaps[i].size;
    vk_integrated = vk_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    VLOG("memory: %s GPU, largest device-local heap %.0f MiB, budget reporting %s", vk_integrated ? "integrated (shares system RAM)" : "discrete",
         vk_local_heap / 1048576.0, has_budget ? "on" : "off");
    return 0;
}

/* ---------- guest memory pools ----------
   Each GPU-visible pool becomes a VkBuffer over the game's memory (imported host memory, no copy). The game keeps a few
   thousand pools alive, more than the allocation limit of some drivers (4096), so pools inside the guest heap share one
   import of the whole heap; other pools (the alias region) get their own. */
typedef struct { uintptr_t lo, hi; VkDeviceMemory mem; VkBuffer buf; int refs, windowed; } import_t;   /* windowed: see vk_buf */
void host_on_heap_shrink(void (*fn)(uint64_t lo, uint64_t hi));
void host_alias_region(uint64_t *base, uint64_t *size);
int host_alias_committed(uint64_t lo, uint64_t hi);
void host_on_alias_unmap(void (*fn)(uint64_t lo, uint64_t hi));
static pidx g_imports;               /* sorted by address, key = import_t * */
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pool_n, g_import_n;

void vk_describe_range(uintptr_t lo, uintptr_t hi, char *out, size_t cap);   /* vk_winmem.c */
static struct { uintptr_t lo, hi; } g_imp_log[64]; static int g_nimp_log;   /* the first imports (diagnostics) */
/* Before the first submission: what each imported range is made of now. Linux GPU drivers check every page of an
   imported range at each submission; a page that is inaccessible or not committed fails it (the Steam Deck). */
void vk_list_regions(uintptr_t lo, uintptr_t hi, int max, void (*line)(const char *));
int vk_under_wine(void);
static void region_line(const char *t) { VLOG("%s", t); }
static void imports_describe(void) {
    VLOG("running under Wine / Proton: %s", vk_under_wine() ? "yes" : "no");
    for (int i = 0; i < g_nimp_log; i++) { char what[200]; vk_describe_range(g_imp_log[i].lo, g_imp_log[i].hi, what, sizeof what);
        VLOG("before the first submission, imported %p..%p (%.1f MiB): %s", (void *)g_imp_log[i].lo, (void *)g_imp_log[i].hi, (g_imp_log[i].hi - g_imp_log[i].lo) / 1048576.0, what);
        if (i < 4) vk_list_regions(g_imp_log[i].lo, g_imp_log[i].hi, 12, region_line); }
}
static import_t *import_fail(uintptr_t lo, uintptr_t hi, const char *step, int r) {   /* why a range cannot be imported, for the first few */
    static int n; char what[200];
    if (n++ < 4) { vk_describe_range(lo, hi, what, sizeof what); VLOG("import of %p..%p (%.1f MiB) failed at %s (%d): %s", (void *)lo, (void *)hi, (hi - lo) / 1048576.0, step, r, what); }
    return NULL;
}
static import_t *import_range_in(uintptr_t lo, uintptr_t hi, int listed) {
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult hr = vkGetMemoryHostPointerPropertiesEXT(vk_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo, &hp);
    if (hr != VK_SUCCESS || !hp.memoryTypeBits) return import_fail(lo, hi, "host pointer properties", (int)hr);
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = hi - lo, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VkBuffer buf;
    VkResult br = vkCreateBuffer(vk_dev, &bi, NULL, &buf);
    if (br != VK_SUCCESS) return import_fail(lo, hi, "buffer", (int)br);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(vk_dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    uint32_t type = UINT32_MAX, bits = hp.memoryTypeBits & req.memoryTypeBits;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)   /* host coherent first: CPU and GPU see each other's writes */
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) type = i;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++) if (bits >> i & 1) type = i;
    VkImportMemoryHostPointerInfoEXT im = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)lo};
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = hi - lo, .memoryTypeIndex = type};
    VkDeviceMemory mem;
    VkResult ar = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(vk_dev, &mi, NULL, &mem);
    if (ar != VK_SUCCESS) { vkDestroyBuffer(vk_dev, buf, NULL); return import_fail(lo, hi, type == UINT32_MAX ? "memory type" : "allocate", (int)ar); }
    VkResult bind = vkBindBufferMemory(vk_dev, buf, mem, 0);
    if (bind != VK_SUCCESS) { vkFreeMemory(vk_dev, mem, NULL); vkDestroyBuffer(vk_dev, buf, NULL); return import_fail(lo, hi, "bind", (int)bind); }
    import_t *im_ = calloc(1, sizeof *im_);
    *im_ = (import_t){lo, hi, mem, buf, 0};
    __atomic_add_fetch(&g_import_bytes, hi - lo, __ATOMIC_RELAXED); g_import_all++;
    if (g_nimp_log < 64) g_imp_log[g_nimp_log++] = (typeof(g_imp_log[0])){lo, hi};
    if (!listed) return im_;
    { static int n; if (n++ < 24) VLOG("imported %p..%p (%.1f MiB) for the GPU", (void *)lo, (void *)hi, (hi - lo) / 1048576.0); }
    pidx_ent e = {lo, hi, lo, im_, 0, im_, 0};
    if (!pidx_insert(&g_imports, &e)) die("Vulkan: pool table");
    g_import_n++;
    return im_;
}
static import_t *import_range(uintptr_t lo, uintptr_t hi) { return import_range_in(lo, hi, 1); }

/* Windowed pools (Wine / Proton). The Linux kernel re-reads the page tables of every imported range a submission
   names, at each submission: with whole pools that was most of TOTK's 2 GiB heap and 40 ms per submission on Great
   Sky Island (the submission thread at 90 % of a core, the frame rate capped by it). A large pool is therefore
   imported in pieces on demand: the 1 MiB-aligned window around each range the GPU is given (vk_buf), clipped to
   the pool, so a submission names only the windows its draws use. Windows of one pool may overlap (they pin the same
   pages; the GPU sees one memory). */
#define WIN_GRAIN (1u << 20)
#define WIN_POOL_MIN (8u << 20)   /* smaller pools are imported whole */
#include "nvn_hmap.h"
static nvn_hmap g_win = {.klen = 16};   /* {lo, hi} -> import_t *, g_pool_mu */
static uint64_t g_win_n, g_win_bytes;
static import_t *window_in(uintptr_t rlo, uintptr_t rhi, uintptr_t a, uintptr_t b, uintptr_t grain) {   /* g_pool_mu held */
    uintptr_t k[2] = {a & ~(grain - 1), (b + grain - 1) & ~(grain - 1)};
    if (k[0] < rlo) k[0] = rlo;
    if (k[1] > rhi) k[1] = rhi;
    import_t *w = nvn_hm_get(&g_win, k);
    if (w) return w;
    w = import_range_in(k[0], k[1], 0);
    if (!w) return NULL;
    nvn_hm_put(&g_win, k, w);
    g_win_n++; g_win_bytes += k[1] - k[0];
    { static int n; if (n++ < 4) VLOG("memory %p..%p (%.0f MiB) given to the GPU in windows: first %p..%p", (void *)rlo, (void *)rhi,
                                      (rhi - rlo) / 1048576.0, (void *)k[0], (void *)k[1]); }
    return w;
}
static import_t *window_of(const import_t *pool, uintptr_t a, uintptr_t b) { return window_in(pool->lo, pool->hi, a, b, WIN_GRAIN); }

/* The guest heap under Wine (TOTK_IMPORT_HEAP=0). RADV names every allocation in every submission and the kernel does
   a fixed amount of work per imported range at each one: the game keeps thousands of small pools in its heap, one
   import each, and submissions took 70-100 ms on the Steam Deck even with large pools windowed. Pools in the heap are
   not imported at all now; the heap is one committed, never protected range, and the GPU is given 16 MiB-aligned
   windows of it on demand (at most about 130), shared by all pools in them. They go when the heap shrinks below them
   (host_on_heap_shrink), else with the process. Creating or finalizing a pool in the heap costs nothing then. */
#define HEAP_GRAIN (16u << 20)
/* The alias region (MapPhysicalMemory) under Wine, likewise: the game keeps hundreds of small pools there. memory.c
   commits it in 16 MiB grains from its start, each kept while a page in it is mapped; the GPU is given the grains
   around each range. Windows go before their grains are decommitted (host_on_alias_unmap). */
static import_t *alias_window(uint64_t a, uint64_t b) {   /* g_pool_mu held */
    uint64_t base, size; host_alias_region(&base, &size);
    if (a < base || b > base + size || b <= a) return NULL;
    uint64_t k[2] = {base + ((a - base) & ~(uint64_t)(HEAP_GRAIN - 1)), base + ((b - base + HEAP_GRAIN - 1) & ~(uint64_t)(HEAP_GRAIN - 1))};
    if (k[1] > base + size) k[1] = base + size;
    import_t *w = nvn_hm_get(&g_win, k);
    if (w) return w;
    if (!host_alias_committed(k[0], k[1])) return NULL;
    if (!(w = import_range_in((uintptr_t)k[0], (uintptr_t)k[1], 0))) return NULL;
    nvn_hm_put(&g_win, k, w);
    g_win_n++; g_win_bytes += k[1] - k[0]; g_aw_live++;
    { static int n; if (n++ < 4) VLOG("alias memory given to the GPU in windows: %p..%p", (void *)k[0], (void *)k[1]); }
    return w;
}
static int in_alias(uint64_t a, uint64_t b) { uint64_t base, size; host_alias_region(&base, &size); return a >= base && b <= base + size; }
static int heap_whole(void) {
    static int whole = -1;
    if (whole < 0) { const char *e = pal_getenv("TOTK_IMPORT_HEAP"); whole = e ? *e != '0' : !pal_under_wine();
                     if (!whole) VLOG("the heap's pools are given to the GPU in 16 MiB windows of the heap (%s)", e ? "TOTK_IMPORT_HEAP=0" : "Wine / Proton"); }
    return whole;
}
static void windows_drop(uintptr_t lo, uintptr_t hi, import_t **out, int *n, int cap, int overlapping) {   /* g_pool_mu held: take windows out */
    for (uint32_t i = 0; i < g_win.cap && *n < cap; i++) {
        import_t *w = g_win.vals ? g_win.vals[i] : NULL;
        if (!w || (overlapping ? (w->hi <= lo || w->lo >= hi) : (w->lo < lo || w->hi > hi))) continue;
        uintptr_t k[2] = {w->lo, w->hi}; nvn_hm_del(&g_win, k);
        g_win_n--; g_win_bytes -= w->hi - w->lo; if (in_alias(w->lo, w->hi)) g_aw_live--;
        out[(*n)++] = w; i = (uint32_t)-1;   /* deletion shifts entries: rescan */
    }
}

static void pool_add(const void *mem, uint64_t size) {
    if (!size) return;
    uintptr_t a = (uintptr_t)mem, b = a + size, lo = a & ~(uintptr_t)(g_host_align - 1), hi = (b + g_host_align - 1) & ~(uintptr_t)(g_host_align - 1);
    pthread_mutex_lock(&g_pool_mu);
    pidx_ent *e = pidx_find_mut(&g_imports, a, size);
    import_t *im = e ? e->buf : NULL;
    if (!im) {
        uint64_t hb, hs; host_heap_range(&hb, &hs);
        /* TOTK_IMPORT_HEAP=0: each pool is imported on its own instead of the whole heap at once (diagnosis: Linux GPU
           drivers want an imported range inside one memory mapping; protection changes split the heap's) */
        int whole = heap_whole(), in_heap = a >= hb && b <= hb + hs;
        if (whole && in_heap) im = import_range((uintptr_t)hb, (uintptr_t)(hb + hs));
        if (!whole && (in_heap || in_alias(a, b))) { g_pool_n++; pthread_mutex_unlock(&g_pool_mu); nvn_dirty_add_range(mem, (size_t)size); return; }   /* heap / alias windows (vk_buf) */
        if (!im && !whole && hi - lo >= WIN_POOL_MIN) {   /* windowed: imported in pieces by vk_buf */
            im = calloc(1, sizeof *im);
            if (!im) die("Vulkan: pool table");
            *im = (import_t){lo, hi, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 1};
            pidx_ent pe = {lo, hi, lo, im, 0, im, 0};
            if (!pidx_insert(&g_imports, &pe)) die("Vulkan: pool table");
            g_import_n++;
        }
        if (!im) im = import_range(lo, hi);
        if (!im) VK_ONCE("memory pool %p+%#llx cannot be imported: the GPU does not see it (first case)", mem, (unsigned long long)size);
    }
    if (im) im->refs++;
    g_pool_n++;
    pthread_mutex_unlock(&g_pool_mu);
    nvn_dirty_add_range(mem, (size_t)size);
}

int runtime_core_away(void);
void runtime_core_back(int *away);
static void free_import(void *arg);
/* SetHeapSize smaller / alias grains decommitted: the windows over [lo, hi) go first */
static void heap_shrinking(uint64_t lo, uint64_t hi) {
    enum { MAXW = 4096 }; import_t **w = malloc(MAXW * sizeof *w); int n = 0;
    if (!w) die("Vulkan: heap windows");
    pthread_mutex_lock(&g_pool_mu); windows_drop((uintptr_t)lo, (uintptr_t)hi, w, &n, MAXW, 1); pthread_mutex_unlock(&g_pool_mu);
    if (n) {
        pthread_mutex_lock(&vk_mu); uint64_t f = vk_flush(NULL, NULL); pthread_mutex_unlock(&vk_mu);
        int away = runtime_core_away(); vk_wait_done(f); runtime_core_back(&away);
        for (int i = 0; i < n; i++) free_import(w[i]);
    }
    free(w);
}
static void free_import(void *arg) { import_t *im = arg; __atomic_sub_fetch(&g_import_bytes, im->hi - im->lo, __ATOMIC_RELAXED); __atomic_sub_fetch(&g_import_all, 1, __ATOMIC_RELAXED);
                                     vkDestroyBuffer(vk_dev, im->buf, NULL); vkFreeMemory(vk_dev, im->mem, NULL); free(im); }
static void pool_del(const void *mem) {
    nvn_dirty_remove_range(mem);
    pthread_mutex_lock(&g_pool_mu);
    pidx_ent *e = pidx_find_mut(&g_imports, (uintptr_t)mem, 1);
    import_t *im = e ? e->buf : NULL;
    uintptr_t lo = 0, hi = 0;
    if (im && !--im->refs) {
        uint64_t hb, hs; host_heap_range(&hb, &hs);
        if (im->lo != (uintptr_t)hb) {   /* the heap import stays */
            lo = im->lo; hi = im->hi;
            pidx_remove(&g_imports, im, NULL);
            g_import_n--;
        } else im = NULL;
    } else im = NULL;
    g_pool_n--;
    pthread_mutex_unlock(&g_pool_mu);
    if (hi > lo) {
        pthread_mutex_lock(&vk_mu);
        vt_pool_gone(lo, hi);
        uint64_t f = vk_flush(NULL, NULL);
        pthread_mutex_unlock(&vk_mu);
        enum { MAXW = 4096 }; import_t **wins = im->windowed ? malloc(MAXW * sizeof *wins) : NULL; int nw = 0;
        if (im->windowed && !wins) die("Vulkan: pool windows");
        if (im->windowed) { pthread_mutex_lock(&g_pool_mu); windows_drop(lo, hi, wins, &nw, MAXW, 0); pthread_mutex_unlock(&g_pool_mu); }
        /* The import goes before this returns, once the GPU is done with it: the game frees the memory as soon as
           Finalize returns, and RADV puts every allocation in every submission, so a submission in between named
           user pages that were gone and the Steam Deck lost the device (while loading, when pools come and go). */
        int away = runtime_core_away();
        vk_wait_done(f);
        runtime_core_back(&away);
        for (int i = 0; i < nw; i++) free_import(wins[i]);
        free(wins);
        if (im->windowed) free(im); else free_import(im);
    }
}

/* GPU-only stand-in for a physical pool the driver cannot import. TOTK's physical pools are CPU_NO_ACCESS page-file
   sections that virtual pools map views of; Linux GPU drivers import only private memory, so on the Steam Deck the
   virtual pool (the menus' render targets and buffers) failed to import and its buffers were bound as zeros. As the CPU
   never touches a CPU_NO_ACCESS pool, device memory of the same size, addressed by physical offset, is the same memory
   to the GPU: virtual addresses are translated page by page (totk_vpool_physical), so aliases still meet.
   shortcut: a stand-in lives as long as the process (pools are few and long-lived); free it at pool finalize if
   physical pools ever come and go. */
uint64_t totk_vpool_physical(const void *mem, uint64_t size);
int totk_vpool_phys_pool(uint64_t phys, uint64_t *lo, uint64_t *span);
static import_t *standin_make(uintptr_t lo, uintptr_t hi) {   /* g_pool_mu held */
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(vk_dev, &bi, NULL, &buf) != VK_SUCCESS) return NULL;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(vk_dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VkDeviceMemory mem;
    if (type == UINT32_MAX || vkAllocateMemory(vk_dev, &mi, NULL, &mem) != VK_SUCCESS) { vkDestroyBuffer(vk_dev, buf, NULL); return NULL; }
    if (vkBindBufferMemory(vk_dev, buf, mem, 0) != VK_SUCCESS) { vkFreeMemory(vk_dev, mem, NULL); vkDestroyBuffer(vk_dev, buf, NULL); return NULL; }
    import_t *im = calloc(1, sizeof *im);
    *im = (import_t){lo, hi, mem, buf, 1};
    pidx_ent e = {lo, hi, lo, im, 0, im, 0};
    if (!pidx_insert(&g_imports, &e)) die("Vulkan: pool table");
    VLOG("physical pool %p..%p (%.1f MiB) is not importable: the GPU uses device memory in its place (CPU_NO_ACCESS)", (void *)lo, (void *)hi, (hi - lo) / 1048576.0);
    return im;
}
int vk_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    pthread_mutex_lock(&g_pool_mu);
    const pidx_ent *e = pidx_find(&g_imports, (uintptr_t)addr, len ? len : 1);
    const import_t *im0 = e ? e->buf : NULL;
    if (im0 && im0->windowed) im0 = window_of(im0, (uintptr_t)addr, (uintptr_t)(addr + (len ? len : 1)));
    if (!e && !heap_whole()) {   /* the heap's windows */
        uint64_t hb, hs; host_heap_range(&hb, &hs);
        if (addr >= hb && addr + (len ? len : 1) <= hb + hs) im0 = window_in((uintptr_t)hb, (uintptr_t)(hb + hs), (uintptr_t)addr, (uintptr_t)(addr + (len ? len : 1)), HEAP_GRAIN);
        else im0 = alias_window(addr, addr + (len ? len : 1));
        if (im0) { *buf = im0->buf; *off = addr - im0->lo; pthread_mutex_unlock(&g_pool_mu); return 1; }
    }
    if (im0) { *buf = im0->buf; *off = addr - im0->lo; }
    pthread_mutex_unlock(&g_pool_mu);
    if (e) return im0 != NULL;
    /* not imported: a virtual pool's pages, through their physical pool (its stand-in when that is not importable) */
    uint64_t ph = totk_vpool_physical((const void *)(uintptr_t)addr, len ? len : 1), plo, pspan;
    if (!ph || !totk_vpool_phys_pool(ph, &plo, &pspan)) return 0;
    pthread_mutex_lock(&g_pool_mu);
    e = pidx_find(&g_imports, (uintptr_t)ph, len ? len : 1);
    import_t *im = e ? e->buf : standin_make((uintptr_t)plo, (uintptr_t)(plo + pspan));
    if (im) { *buf = im->buf; *off = ph - im->lo; }
    pthread_mutex_unlock(&g_pool_mu);
    return im != NULL;
}

/* ---------- submissions ---------- */
typedef struct chunk { VkBuffer buf; VmaAllocation al; uint8_t *map; VkDeviceSize size, used; struct chunk *next; } chunk;
typedef struct { void (*fn)(void *); void *arg; } act_t;
typedef struct sub {
    VkCommandPool pool; VkCommandBuffer pre, main;
    uint64_t fence, t_submit; chunk *chunks; act_t *acts; int nact, cact; struct sub *next;
} sub;
#define CHUNK_SIZE (16ull << 20)   /* staging: larger requests get a chunk of their own size */
enum { MAX_IN_FLIGHT = 6 };
static VkSemaphore g_tl;
static sub *g_cur, *g_free_subs, *g_fly_head, *g_fly_tail;
static chunk *g_free_chunks;
static int g_in_flight, g_free_chunk_n;
static _Atomic uint64_t g_chunk_bytes;   /* staging memory held (the memory line) */
static uint64_t g_fence, g_done, g_serial;
static _Atomic uint64_t g_game_fence;
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;

uint64_t vk_open_fence(void) { return g_fence + 1; }
uint64_t vk_gpu_done(void) { uint64_t v = 0; vkGetSemaphoreCounterValue(vk_dev, g_tl, &v); return v; }
void vk_barrier(VkCommandBuffer c) {
    vk_n_barrier++;
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(c, &di);
}

static sub *take_sub(void) {
    pthread_mutex_lock(&g_done_mu);
    while (g_in_flight >= MAX_IN_FLIGHT) pthread_cond_wait(&g_done_cv, &g_done_mu);
    sub *s = g_free_subs; if (s) g_free_subs = s->next;
    pthread_mutex_unlock(&g_done_mu);
    if (s) { VKC(vkResetCommandPool(vk_dev, s->pool, 0)); return s; }
    s = calloc(1, sizeof *s);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = vk_qfam};
    VKC(vkCreateCommandPool(vk_dev, &pi, NULL, &s->pool));
    VkCommandBuffer cb[2];
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VKC(vkAllocateCommandBuffers(vk_dev, &ai, cb));
    s->pre = cb[0]; s->main = cb[1];
    return s;
}

void vk_open(void) {
    if (g_cur) return;
    sub *s = take_sub();
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(vkBeginCommandBuffer(s->pre, &bi)); VKC(vkBeginCommandBuffer(s->main, &bi));
    vk_barrier(s->pre);    /* after everything submitted earlier */
    vk_barrier(s->main);   /* after this submission's uploads */
    g_cur = s; g_serial++;
}
VkCommandBuffer vk_cmd(void) { vk_open(); return g_cur->main; }
VkCommandBuffer vk_pre(void) { vk_open(); return g_cur->pre; }
uint64_t vk_sub_serial(void) { return g_serial; }

void vk_after(void (*fn)(void *), void *arg) {
    vk_open();
    sub *s = g_cur;
    if (s->nact == s->cact) { s->cact = s->cact ? s->cact * 2 : 64; s->acts = realloc(s->acts, (size_t)s->cact * sizeof *s->acts); if (!s->acts) die("Vulkan: completion list"); }
    s->acts[s->nact++] = (act_t){fn, arg};
}

/* A write the GPU cannot make (memory it does not see): done by the CPU once the open submission has completed, so it
   lands after the GPU work recorded before it, as on the GPU timeline. */
typedef struct { void *dst; const void *src; size_t n; uint8_t data[]; } cpu_wr;
static void do_cpu_write(void *arg) {
    cpu_wr *w = arg;
    dm_guest_write_prepare(w->dst, w->n);
    memmove(w->dst, w->src ? w->src : w->data, w->n);
    free(w);
}
void vk_after_write(uint64_t dst, const void *src, size_t n, int snapshot) {
    cpu_wr *w = malloc(sizeof *w + (snapshot ? n : 0));
    if (!w) die("Vulkan: deferred write of %zu bytes", n);
    w->dst = (void *)(uintptr_t)dst; w->n = n; w->src = snapshot ? NULL : src;
    if (snapshot) memcpy(w->data, src, n);
    vk_after(do_cpu_write, w);
}

static chunk *new_chunk(VkDeviceSize size) {
    pthread_mutex_lock(&g_done_mu);
    chunk *c = size <= CHUNK_SIZE ? g_free_chunks : NULL;
    if (c) { g_free_chunks = c->next; g_free_chunk_n--; }
    pthread_mutex_unlock(&g_done_mu);
    if (c) { c->used = 0; return c; }
    c = calloc(1, sizeof *c);
    c->size = size > CHUNK_SIZE ? size : CHUNK_SIZE;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->size,   /* index: quads, 8-bit indices; texel: buffer textures copied (vk_tex.c) */
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ac = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    VkResult r = vmaCreateBuffer(vk_vma, &bi, &ac, &c->buf, &c->al, &info);
    if (r != VK_SUCCESS) {   /* out of memory: let every earlier submission finish and hand its chunks back, drop the spare ones, try again */
        VLOG("staging: %llu MiB not available (%d): waiting for the GPU and freeing spare staging memory", (unsigned long long)(c->size >> 20), (int)r);
        vk_wait_done(g_fence);
        pthread_mutex_lock(&g_done_mu);
        for (chunk *f = g_free_chunks, *nx; f; f = nx) { nx = f->next; vmaDestroyBuffer(vk_vma, f->buf, f->al); atomic_fetch_sub(&g_chunk_bytes, f->size); free(f); }
        g_free_chunks = NULL; g_free_chunk_n = 0;
        pthread_mutex_unlock(&g_done_mu);
        r = vmaCreateBuffer(vk_vma, &bi, &ac, &c->buf, &c->al, &info);
        if (r != VK_SUCCESS) { ac.usage = VMA_MEMORY_USAGE_AUTO; ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; r = vmaCreateBuffer(vk_vma, &bi, &ac, &c->buf, &c->al, &info); }
        if (r != VK_SUCCESS) { vk_log_memory(); die("Vulkan: out of memory for %llu MiB of staging (%d); the memory line above says where it went", (unsigned long long)(c->size >> 20), (int)r); }
    }
    atomic_fetch_add(&g_chunk_bytes, c->size);
    c->map = info.pMappedData;
    return c;
}
void *vk_stage(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    vk_open();
    if (align < 16) align = 16;
    chunk *c = g_cur->chunks;
    VkDeviceSize at = c ? (c->used + align - 1) & ~(align - 1) : 0;
    if (!c || at + n > c->size) { c = new_chunk(n); c->next = g_cur->chunks; g_cur->chunks = c; at = 0; }
    c->used = at + n; *buf = c->buf; *off = at;
    return c->map + at;
}

/* ---------- the submission thread ----------
   vkQueueSubmit2 and vkQueuePresentKHR run here, in the order they were asked for. On Linux the kernel checks every
   page of every imported range a submission uses (amdgpu user pointers), about 10 ms a submission on the Steam Deck,
   and the game's render thread used to wait for that twice a frame (its work, then the present). Fences are timeline
   values given out at vk_flush, so waiting for one works whether it was submitted yet or not. */
typedef struct qitem { struct qitem *next; int present; VkCommandBuffer cb[2]; VkSemaphoreSubmitInfo wait, sig[2]; int has_wait, nsig; } qitem;
static qitem *g_q_head, *g_q_tail;
static int g_q_busy;
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cv = PTHREAD_COND_INITIALIZER;
_Atomic uint64_t vk_ns_qsubmit;   /* time in vkQueueSubmit2 (the 10-second line) */
static void q_push(qitem *it) {
    pthread_mutex_lock(&g_q_mu);
    if (g_q_tail) g_q_tail->next = it; else g_q_head = it;
    g_q_tail = it;
    pthread_cond_broadcast(&g_q_cv);
    pthread_mutex_unlock(&g_q_mu);
}
void vk_submit_drain(void) {   /* everything asked for so far handed to the driver */
    pthread_mutex_lock(&g_q_mu);
    while (g_q_head || g_q_busy) pthread_cond_wait(&g_q_cv, &g_q_mu);
    pthread_mutex_unlock(&g_q_mu);
}
void vk_queue_present(void) { qitem *it = calloc(1, sizeof *it); if (!it) die("Vulkan: present"); it->present = 1; q_push(it); }
void vp_do_present(void);   /* vk_present.c: acquire, copy, present (this thread) */
static void *submit_thread(void *unused) {
    (void)unused;
    pal_thread_set_name("vk submit");
    for (uint64_t n = 0;; ) {
        pthread_mutex_lock(&g_q_mu);
        while (!g_q_head) pthread_cond_wait(&g_q_cv, &g_q_mu);
        qitem *it = g_q_head; g_q_head = it->next; if (!g_q_head) g_q_tail = NULL;
        g_q_busy = 1;
        pthread_mutex_unlock(&g_q_mu);
        if (it->present) vp_do_present();
        else {
            VkCommandBufferSubmitInfo cb[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = it->cb[0]},
                                               {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = it->cb[1]}};
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = it->has_wait ? 1 : 0, .pWaitSemaphoreInfos = &it->wait,
                                .commandBufferInfoCount = 2, .pCommandBufferInfos = cb, .signalSemaphoreInfoCount = (uint32_t)it->nsig, .pSignalSemaphoreInfos = it->sig};
            uint64_t tq = pal_now_ns();
            VkResult r = vkQueueSubmit2(vk_q, 1, &si, VK_NULL_HANDLE);
            atomic_fetch_add(&vk_ns_qsubmit, pal_now_ns() - tq);
            if (n++ < 3) VLOG("submission %llu: %s", (unsigned long long)it->sig[0].value, r == VK_SUCCESS ? "accepted" : "refused");
            if (r != VK_SUCCESS) vk_lost(r), die("Vulkan: vkQueueSubmit2 failed (%d)%s", (int)r, r == VK_ERROR_DEVICE_LOST ? ": the GPU stopped (device lost)" : "");
        }
        free(it);
        pthread_mutex_lock(&g_q_mu);
        g_q_busy = 0;
        pthread_cond_broadcast(&g_q_cv);
        pthread_mutex_unlock(&g_q_mu);
    }
    return NULL;
}

uint64_t vk_flush(const VkSemaphoreSubmitInfo *wait, const VkSemaphoreSubmitInfo *signal) {
    vk_open();
    sub *s = g_cur;
    VkMemoryBarrier2 hb ={VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo hd = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &hb};
    vkCmdPipelineBarrier2(s->main, &hd);   /* the CPU reads what the GPU wrote into pool memory once the fence is done */
    VKC(vkEndCommandBuffer(s->pre)); VKC(vkEndCommandBuffer(s->main));
    if (!g_fence) { pthread_mutex_lock(&g_pool_mu); imports_describe(); pthread_mutex_unlock(&g_pool_mu); }
    qitem *it = calloc(1, sizeof *it);
    if (!it) die("Vulkan: submission");
    it->cb[0] = s->pre; it->cb[1] = s->main;
    it->sig[0] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_tl, .value = g_fence + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    it->nsig = 1; if (signal) it->sig[it->nsig++] = *signal;
    if (wait) { it->wait = *wait; it->has_wait = 1; }
    q_push(it);
    s->fence = ++g_fence; s->next = NULL; s->t_submit = pal_now_ns();
    g_cur = NULL;
    pthread_mutex_lock(&g_done_mu);
    if (g_fly_tail) g_fly_tail->next = s; else g_fly_head = s;
    g_fly_tail = s; g_in_flight++;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
    return s->fence;
}

static void lat_note(uint64_t ns);
static void *done_thread(void *unused) {
    (void)unused;
    pal_thread_set_name("vk completion");
    for (;;) {
        pthread_mutex_lock(&g_done_mu);
        while (!g_fly_head) pthread_cond_wait(&g_done_cv, &g_done_mu);
        sub *s = g_fly_head;
        pthread_mutex_unlock(&g_done_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_tl, .pValues = &s->fence};
        VkResult r = vkWaitSemaphores(vk_dev, &wi, UINT64_MAX);
        if (r != VK_SUCCESS) vk_lost(r), die("Vulkan: waiting for the GPU failed (%d)%s", (int)r, r == VK_ERROR_DEVICE_LOST ? ": the GPU stopped (device lost)" : "");
        lat_note(pal_now_ns() - s->t_submit);
        for (int i = 0; i < s->nact; i++) s->acts[i].fn(s->acts[i].arg);
        s->nact = 0;
        pthread_mutex_lock(&g_done_mu);
        for (chunk *c = s->chunks, *nx; c; c = nx) {
            nx = c->next;
            if (c->size == CHUNK_SIZE && g_free_chunk_n < 4) { c->next = g_free_chunks; g_free_chunks = c; g_free_chunk_n++; }
            else { vmaDestroyBuffer(vk_vma, c->buf, c->al); atomic_fetch_sub(&g_chunk_bytes, c->size); free(c); }
        }
        s->chunks = NULL;
        g_fly_head = s->next; if (!g_fly_head) g_fly_tail = NULL;
        g_done = s->fence; g_in_flight--;
        s->next = g_free_subs; g_free_subs = s;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&g_done_mu);
    }
    return NULL;
}

void vk_wait_done(uint64_t fence) {
    pthread_mutex_lock(&g_done_mu);
    while (g_done < fence) pthread_cond_wait(&g_done_cv, &g_done_mu);
    pthread_mutex_unlock(&g_done_mu);
}

/* ---------- gpu_ops ---------- */
static int init(void) {
    if (!fe_window()) return -1;
    const char *v = pal_getenv("TOTK_VK_VALIDATE");
    if (create_instance(v && *v == '1')) return -1;
    if (!SDL_Vulkan_CreateSurface(fe_window(), vk_inst, NULL, &vk_surface)) { VLOG("window surface: %s", SDL_GetError()); return -1; }
    if (pick_device() || create_device()) return -1;
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    VKC(vkCreateSemaphore(vk_dev, &si, NULL, &g_tl));
    pthread_t t;
    if (pthread_create(&t, NULL, done_thread, NULL)) die("Vulkan: completion thread");
    pthread_detach(t);
    if (pthread_create(&t, NULL, submit_thread, NULL)) die("Vulkan: submission thread");
    host_on_heap_shrink(heap_shrinking);
    host_on_alias_unmap(heap_shrinking);
    pthread_detach(t);
    vt_init(); vs_init(); vd_init();
    if (vp_init()) return -1;
    return 0;
}

static void pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    vt_pool_invalidate(p, off, n);   /* imported memory is coherent for buffers; rendered textures are in images and are written back */
}
static void set_tex_pool_n(int n) { vt_set_pool_n(n); }
static void reg_tex(int id, const tex_t *t, const view_t *v) { vt_reg_tex(id, t, v); }
static void reg_samp(int id, const samp_par *p) { vt_reg_samp(id, p); }
static void tex_release(tex_t *t) { if (t->serial) vt_release_serial(t->serial); }
static void prog_release(prog_t *p) { if (!p->serial) return; pthread_mutex_lock(&vk_mu); vs_prog_release(p->serial); pthread_mutex_unlock(&vk_mu); }
static void window_tex(uint64_t serial) { (void)serial; }

uint32_t vk_texel_align[2] = {16, 16}, vk_texel_single[2];
int vk_divisor;
uint32_t vk_sb_align = 16;
uint64_t vk_ns_xlate, vk_ns_pipe, vk_ns_tex, vk_n_pass, vk_n_ubo, vk_n_ubflush, vk_n_barrier;
static uint64_t g_sub_max;
static _Atomic uint64_t g_sub_ns, g_sub_n, g_gpu_lat_ns, g_gpu_lat_n, g_wait_ns;
/* Where memory goes (every 10 s, and before an out-of-memory failure). */
int vk_integrated; uint64_t vk_local_heap;
void vk_log_memory(void) {
    uint64_t res = 0, com = 0, hb = 0, hs = 0; pal_process_memory(&res, &com); host_heap_range(&hb, &hs);
    uint64_t img[4]; vt_memory(img);
    char heaps[512] = ""; size_t at = 0;
    VmaBudget b[VK_MAX_MEMORY_HEAPS]; vmaGetHeapBudgets(vk_vma, b);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount && at < sizeof heaps - 80; i++)
        at += (size_t)snprintf(heaps + at, sizeof heaps - at, "%s heap %u%s %.0f/%.0f MiB", i ? "," : "", i, mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (GPU)" : "",
                               b[i].usage / 1048576.0, b[i].budget / 1048576.0);
    VLOG("memory: process %.0f MiB resident, %.0f MiB committed (game heap %.0f MiB); images %.0f MiB (decoded ASTC %.0f, other uploaded %.0f, GPU-written %.0f; %.0f MiB saved by mip drop); staging %.0f MiB; driver:%s",
         res / 1048576.0, com / 1048576.0, hs / 1048576.0, (img[0] + img[1] + img[2]) / 1048576.0, img[0] / 1048576.0, img[1] / 1048576.0, img[2] / 1048576.0, img[3] / 1048576.0,
         atomic_load(&g_chunk_bytes) / 1048576.0, heaps);
}
static uint64_t submit(const uint64_t *handles, int n) {
    uint64_t t0 = pal_now_ns();
    {   /* the sampler profiles this thread while it replays (threads.c, the [replay] line) */
        extern void *volatile runtime_replay_thread; static unsigned long id; static void *h;
        if (pal_thread_id() != id) { id = (unsigned long)pal_thread_id(); h = pal_thread_handle_self(); }
        runtime_replay_thread = h;
    }
    pthread_mutex_lock(&vk_mu);
    vk_open();
    for (int i = 0; i < n; i++) vx_run((const uint8_t *)(uintptr_t)handles[i]);
    vx_submit_done();
    vx_end_pass();
    uint64_t f = vk_flush(NULL, NULL);
    atomic_store(&g_game_fence, f);
    vt_end_submission();
    pthread_mutex_unlock(&vk_mu);
    { extern void *volatile runtime_replay_thread; runtime_replay_thread = NULL; }
    uint64_t dt = pal_now_ns() - t0; if (dt > g_sub_max) g_sub_max = dt;
    atomic_fetch_add(&g_sub_ns, dt); atomic_fetch_add(&g_sub_n, 1);
    static uint64_t last; uint64_t now = pal_now_ns();
    if (!last) last = now;
    if (now - last >= 10000000000ull) {
        last = now;
        uint64_t sn = atomic_exchange(&g_sub_n, 0), sns = atomic_exchange(&g_sub_ns, 0), ln = atomic_exchange(&g_gpu_lat_n, 0), lns = atomic_exchange(&g_gpu_lat_ns, 0), w = atomic_exchange(&g_wait_ns, 0);
        { extern uint64_t vk_ns_hash; VLOG("of the replay: texture content checks (no write protection) %.0f ms, handing work to the driver (vkQueueSubmit2) %.0f ms; %llu imports alive (%.0f MiB), of them %llu windows (%.0f MiB, %d over physical memory)", vk_ns_hash / 1e6, atomic_exchange(&vk_ns_qsubmit, 0) / 1e6,
                                     (unsigned long long)g_import_all, g_import_bytes / 1048576.0, (unsigned long long)g_win_n, g_win_bytes / 1048576.0, g_aw_live); vk_ns_hash = 0; }
        VLOG("last 10 s: %llu submissions, replay %.0f ms in all (longest %.0f ms; shader translation %.0f, pipelines %.0f, textures %.0f; %llu passes, %llu uniform updates (%llu flushes), %llu barriers); submit-to-done %.2f ms on average; game waited %.0f ms for the GPU; %.1f MiB copied to video memory",
             (unsigned long long)sn, sns / 1e6, g_sub_max / 1e6, vk_ns_xlate / 1e6, vk_ns_pipe / 1e6, vk_ns_tex / 1e6, (unsigned long long)vk_n_pass, (unsigned long long)vk_n_ubo, (unsigned long long)vk_n_ubflush, (unsigned long long)vk_n_barrier, ln ? lns / 1e6 / ln : 0.0, w / 1e6, g_sh_bytes / 1048576.0);
        vk_log_memory();
        static int diag = -1; if (diag < 0) { const char *d = pal_getenv("TOTK_DIAG"); diag = d && *d == '1'; }
        if (diag) { static uint64_t lastf; uint64_t f = pal_fault_total(); char cpu[600]; pal_thread_cpu_report(cpu, sizeof cpu);
          VLOG("cpu (last 10 s, %% of one core): %s; %llu memory faults", cpu, (unsigned long long)(f - lastf)); lastf = f;
          void runtime_wait_report(char *out, size_t cap); char w[400]; runtime_wait_report(w, sizeof w);
          extern _Atomic uint64_t vp_ns_acquire, vp_ns_present, vp_n_present; extern _Atomic unsigned long long vi_n_vsync;
          VLOG("waits (last 10 s): %s; %llu presents, acquiring images %.0f ms, presenting %.0f ms; %llu vsync signals", w,
               (unsigned long long)atomic_exchange(&vp_n_present, 0), atomic_exchange(&vp_ns_acquire, 0) / 1e6, atomic_exchange(&vp_ns_present, 0) / 1e6,
               (unsigned long long)atomic_exchange(&vi_n_vsync, 0)); }
        vd_pcache_save();
        g_sh_bytes = 0; g_sub_max = vk_ns_xlate = vk_ns_pipe = vk_ns_tex = vk_n_pass = vk_n_ubo = vk_n_ubflush = vk_n_barrier = 0;
    }
    static int said; if (!said++) VLOG("first submission (%d command lists)", n);
    return f;
}
static void lat_note(uint64_t ns) { atomic_fetch_add(&g_gpu_lat_ns, ns); atomic_fetch_add(&g_gpu_lat_n, 1); }
static uint64_t last_fence(void) { return atomic_load(&g_game_fence); }
static int wait_fence(uint64_t fence, uint64_t timeout_ns) {
    uint64_t t0 = pal_now_ns();
    pthread_mutex_lock(&g_done_mu);
    if (g_done < fence && timeout_ns) {
        uint64_t end = pal_wall_time_ns() + (timeout_ns > (1ull << 62) ? (1ull << 62) : timeout_ns);
        struct timespec ts = {(time_t)(end / 1000000000ull), (long)(end % 1000000000ull)};
        while (g_done < fence && pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts) == 0) {}
    }
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_done_mu);
    if (timeout_ns) atomic_fetch_add(&g_wait_ns, pal_now_ns() - t0);
    return ok;
}
static void finish(void) {
    pthread_mutex_lock(&vk_mu); uint64_t f = g_fence; pthread_mutex_unlock(&vk_mu);
    vk_wait_done(f);
}
static uint64_t present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    pthread_mutex_lock(&vk_mu);
    uint64_t f = vp_present(t, crop);
    pthread_mutex_unlock(&vk_mu);
    return f;
}
static void present_wait(uint64_t ticket) { (void)ticket; }   /* a present is submitted when gpu_present returns */
static int display_hz(void) { return 60; }   /* the game's clock: its interval is in 60 Hz ticks (nvn.c present_period) */

const gpu_ops gpu_vk_ops = {"vulkan", init, pool_add, pool_del, pool_invalidate, set_tex_pool_n, reg_tex, reg_samp, tex_release,
                            prog_release, window_tex, vt_read_texels, vt_write_texels, vx_pin_refs, submit, last_fence, wait_fence,
                            finish, present, present_wait, display_hz};
