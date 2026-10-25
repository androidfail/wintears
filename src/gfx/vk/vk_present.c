/* The window's swapchain: each NVN present blits the game's swapchain texture into the window, scaled to fit with
   black bars (16:9 kept), in submission order after the work that drew it. Mailbox when the GPU offers it (no tearing,
   no wait: the game paces itself, nvn.c), else FIFO. */
#include <stdatomic.h>
#include "gfx/vk/vk.h"

#include "frontend/frontend.h"

static VkSwapchainKHR g_sc;
static VkImage g_img[8];
static uint32_t g_n;
static VkFormat g_fmt;
static VkExtent2D g_ext;
static VkSemaphore g_acquired[9], g_drawn[8];
static unsigned g_next;
static int g_stale;
static uint64_t g_presents;
uint64_t vk_present_count(void) { return g_presents; }

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys, vk_surface, &caps));
    int w, h; fe_drawable_size(&w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return 0;   /* minimised: try again at the next present */
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &nf, NULL);
    VkSurfaceFormatKHR fm[64]; if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &nf, fm);
    VkSurfaceFormatKHR pick = fm[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = fm[i]; break; }
    uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &nm, NULL);
    VkPresentModeKHR modes[16]; if (nm > 16) nm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &nm, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk_surface, .minImageCount = count,
        .imageFormat = pick.format, .imageColorSpace = pick.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = old};
    VKC(vkCreateSwapchainKHR(vk_dev, &ci, NULL, &g_sc));
    if (old) vkDestroySwapchainKHR(vk_dev, old, NULL);
    g_n = 8; VKC(vkGetSwapchainImagesKHR(vk_dev, g_sc, &g_n, g_img));
    g_fmt = pick.format; g_ext = ext;
    VLOG("window swapchain %ux%u, %u images, format %d, %s", ext.width, ext.height, g_n, (int)g_fmt, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return 1;
}

int vp_init(void) {
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < 9; i++) VKC(vkCreateSemaphore(vk_dev, &si, NULL, &g_acquired[i]));
    for (int i = 0; i < 8; i++) VKC(vkCreateSemaphore(vk_dev, &si, NULL, &g_drawn[i]));
    create_swapchain();
    return 0;
}

static void image_barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(c, &di);
}

/* The game's bytes are what the display shows, whatever the format says. A blit from an sRGB image would decode them
   to linear first (a dark picture), so such an image goes through a UNORM scratch image copied bit for bit. */
static VkImage g_raw; static VmaAllocation g_raw_al; static VkFormat g_raw_fmt; static uint32_t g_raw_w, g_raw_h;
typedef struct { VkImage img; VmaAllocation al; } raw_old;
static void raw_free(void *arg) { raw_old *o = arg; vmaDestroyImage(vk_vma, o->img, o->al); free(o); }
static VkImage unorm_copy(VkCommandBuffer c, const vtex *e, uint32_t w, uint32_t h) {
    VkFormat f = vt_vkformat(e) == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : vt_vkformat(e) == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
    if (!f) return vt_image(e);
    if (!g_raw || g_raw_fmt != f || g_raw_w != w || g_raw_h != h) {
        if (g_raw) { raw_old *o = malloc(sizeof *o); if (o) { *o = (raw_old){g_raw, g_raw_al}; vk_after(raw_free, o); } }
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ac = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        g_raw = VK_NULL_HANDLE;
        if (vmaCreateImage(vk_vma, &ci, &ac, &g_raw, &g_raw_al, NULL) != VK_SUCCESS) { g_raw = VK_NULL_HANDLE; return vt_image(e); }
        g_raw_fmt = f; g_raw_w = w; g_raw_h = h;
    }
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_raw, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(c, &di);   /* its old contents are not needed */
    VkImageCopy cp = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(c, vt_image(e), VK_IMAGE_LAYOUT_GENERAL, g_raw, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    vk_barrier(c);
    return g_raw;
}

typedef struct { uint8_t *px; uint32_t w, h, d; int bgra; char path[1024]; } shot_t;
int vk_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch);
const char *vk_shot_path(uint64_t n);
static void write_shot(void *arg) {   /* completion thread: the copy is done */
    shot_t *s = arg;
    if (s->bgra) for (size_t i = 0; i < (size_t)s->w * s->h; i++) { uint8_t t = s->px[4 * i]; s->px[4 * i] = s->px[4 * i + 2]; s->px[4 * i + 2] = t; }
    for (size_t i = 0; i < (size_t)s->w * s->h; i++) s->px[4 * i + 3] = 255;
    VLOG("screenshot %s (%ux%u): %s", s->path, s->w, s->h, vk_write_png(s->path, s->px, s->w, s->h, (size_t)s->w * 4) ? "FAILED" : "written");
    free(s);
}

_Atomic uint64_t vp_ns_acquire, vp_ns_present, vp_n_present;   /* the 10-second log */
uint64_t vp_present(const tex_t *t, const int crop[4]) {
    if (g_stale || !g_sc) { vkDeviceWaitIdle(vk_dev); g_stale = 0; if (!create_swapchain()) return 0; }
    VkSemaphore acq = g_acquired[g_next++ % 9];
    uint32_t idx;
    uint64_t ta = pal_now_ns();
    VkResult r = vkAcquireNextImageKHR(vk_dev, g_sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    atomic_fetch_add(&vp_ns_acquire, pal_now_ns() - ta); atomic_fetch_add(&vp_n_present, 1);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g_stale = 1; return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) die("Vulkan: vkAcquireNextImageKHR failed (%d)", (int)r);
    vtex *e = vt_get(t);
    VkCommandBuffer c = vk_cmd();
    image_barrier(c, g_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    if (e && !vt_depth(e)) {
        uint32_t tw, th, td; vt_size(e, 0, &tw, &th, &td);
        int32_t sx = 0, sy = 0, sw = (int32_t)tw, sh = (int32_t)th;
        if (crop && crop[2] > 0 && crop[3] > 0) { sx = crop[0]; sy = crop[1]; sw = crop[2]; sh = crop[3]; }
        /* fit sw x sh into the window, centred */
        double k = (double)g_ext.width / sw < (double)g_ext.height / sh ? (double)g_ext.width / sw : (double)g_ext.height / sh;
        int32_t dw = (int32_t)(sw * k + 0.5), dh = (int32_t)(sh * k + 0.5), dx = ((int32_t)g_ext.width - dw) / 2, dy = ((int32_t)g_ext.height - dh) / 2;
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        vk_barrier(c);
        vkCmdBlitImage(c, unorm_copy(c, e, tw, th), VK_IMAGE_LAYOUT_GENERAL, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    const char *shot = e ? vk_shot_path(g_presents + 1) : NULL;
    VkFormat ef = e ? vt_vkformat(e) : VK_FORMAT_UNDEFINED;
    if (shot && (ef == VK_FORMAT_R8G8B8A8_UNORM || ef == VK_FORMAT_R8G8B8A8_SRGB || ef == VK_FORMAT_B8G8R8A8_UNORM || ef == VK_FORMAT_B8G8R8A8_SRGB)) {
        shot_t *sh = malloc(sizeof *sh);
        vt_size(e, 0, &sh->w, &sh->h, &sh->d);
        VkBuffer sb; VkDeviceSize so;
        sh->px = vk_stage((VkDeviceSize)sh->w * sh->h * 4, 16, &sb, &so);
        sh->bgra = ef == VK_FORMAT_B8G8R8A8_UNORM || ef == VK_FORMAT_B8G8R8A8_SRGB;
        snprintf(sh->path, sizeof sh->path, "%s", shot);
        VkBufferImageCopy r = {.bufferOffset = so, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {sh->w, sh->h, 1}};
        vkCmdCopyImageToBuffer(c, vt_image(e), VK_IMAGE_LAYOUT_GENERAL, sb, 1, &r);
        vk_after(write_shot, sh);
    }
    image_barrier(c, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = acq, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_drawn[idx], .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    uint64_t f = vk_flush(&w, &s);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_drawn[idx],
                           .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    uint64_t tp = pal_now_ns();
    r = vkQueuePresentKHR(vk_q, &pi);
    atomic_fetch_add(&vp_ns_present, pal_now_ns() - tp);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_stale = 1;
    else if (r != VK_SUCCESS) die("Vulkan: vkQueuePresentKHR failed (%d)", (int)r);
    if (!g_presents++) VLOG("first frame in the window (game texture %s)", e ? "found" : "not drawn yet");
    return f;
}
