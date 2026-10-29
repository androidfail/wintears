// ASTC to RGBA8 on upload (desktop GPUs do not sample ASTC; Apple GPUs, upstream's, do): ARM's astc-encoder
// (externals/astc-encoder, decompressor build). Big images are decoded on several threads.
// shortcut: CPU decode at upload; upgrade to a GPU compute decoder if loading stutters.
#include <astcenc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct ctx_entry { unsigned bw, bh; int srgb; astcenc_context *ctx; };
std::vector<ctx_entry> g_ctx;
std::mutex g_mu;
unsigned g_threads;

astcenc_context *get(unsigned bw, unsigned bh, int srgb) {
    for (auto &e : g_ctx) if (e.bw == bw && e.bh == bh && e.srgb == srgb) return e.ctx;
    if (!g_threads) g_threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    astcenc_config cfg;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, bw, bh, 1, 0.0f, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS) return nullptr;
    astcenc_context *c = nullptr;
    if (astcenc_context_alloc(&cfg, g_threads, &c, nullptr) != ASTCENC_SUCCESS) return nullptr;
    g_ctx.push_back({bw, bh, srgb, c});
    return c;
}

/* Helper threads made once (starting threads is slow under Wine: big textures used to start up to 8 each): job k of n
   runs on helper k - 1, job 0 on the caller. */
std::mutex g_pool_mu;
std::condition_variable g_pool_cv, g_pool_done;
std::function<void(unsigned)> g_job;
unsigned g_job_gen, g_job_n, g_job_left;
std::vector<std::thread> g_helpers;
void helper(unsigned k, unsigned seen) {   /* seen: the job number when it was made (it waits for the next) */
    for (;;) {
        std::function<void(unsigned)> job;
        {
            std::unique_lock<std::mutex> l(g_pool_mu);
            g_pool_cv.wait(l, [&] { return g_job_gen != seen; });
            seen = g_job_gen;
            if (k >= g_job_n) continue;
            job = g_job;
        }
        job(k);
        std::lock_guard<std::mutex> l(g_pool_mu);
        if (--g_job_left == 0) g_pool_done.notify_all();
    }
}
void run_on(unsigned n, const std::function<void(unsigned)> &f) {
    while (g_helpers.size() + 1 < n) {
        unsigned k = (unsigned)g_helpers.size() + 1, gen;
        { std::lock_guard<std::mutex> l(g_pool_mu); gen = g_job_gen; }
        g_helpers.emplace_back(helper, k, gen); g_helpers.back().detach();
    }
    {
        std::lock_guard<std::mutex> l(g_pool_mu);
        g_job = f; g_job_n = n; g_job_left = n - 1; g_job_gen++;
    }
    g_pool_cv.notify_all();
    f(0);
    std::unique_lock<std::mutex> l(g_pool_mu);
    g_pool_done.wait(l, [] { return g_job_left == 0; });
}
}

// src: the blocks of one 2D slice (row by row), dst: w x h RGBA8 texels. 0 = decoded.
extern "C" int vk_astc_decode(const uint8_t *src, size_t n, unsigned bw, unsigned bh, unsigned w, unsigned h, int srgb, uint8_t *dst) {
    std::lock_guard<std::mutex> lock(g_mu);
    astcenc_context *c = get(bw, bh, srgb);
    if (!c) return -1;
    void *slices[1] = {dst};
    astcenc_image img{};
    img.dim_x = w; img.dim_y = h; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = slices;
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    unsigned nt = (size_t)w * h >= 512u * 512u ? g_threads : 1;
    astcenc_error err = ASTCENC_SUCCESS;
    if (nt == 1) err = astcenc_decompress_image(c, src, n, &img, &swz, 0);
    else {
        std::vector<astcenc_error> e(nt, ASTCENC_SUCCESS);
        run_on(nt, [&](unsigned i) { e[i] = astcenc_decompress_image(c, src, n, &img, &swz, i); });
        for (auto x : e) if (x != ASTCENC_SUCCESS) err = x;
    }
    astcenc_decompress_reset(c);
    return err == ASTCENC_SUCCESS ? 0 : -1;
}

/* BC3 (DXT5) from RGBA8, for GPUs without ASTC: a quarter of RGBA8's memory and bandwidth. Endpoints are the block's
   bounding box inset by a sixteenth (J.M.P. van Waveren, "Real-Time DXT Compression", 2006): fast, a little blockier
   than ASTC, which is already lossy. The colour part of BC3 always has four colours, so endpoint order is free. */
namespace {
inline unsigned rgb565(int r, int g, int b) { return (unsigned)((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255); }
void bc3_block(const uint8_t px[16][4], uint8_t out[16]) {
    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { lo[c] = std::min(lo[c], (int)px[i][c]); hi[c] = std::max(hi[c], (int)px[i][c]); }
    /* alpha: a0 = max, a1 = min, eight levels; index 0 = a0, 1 = a1, 2..7 = (8-i)/7 of the way from a1 to a0 */
    int a0 = hi[3], a1 = lo[3];
    out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    uint64_t ai = 0;
    if (a0 > a1)
        for (int i = 0; i < 16; i++) {
            int l = ((px[i][3] - a1) * 14 + (a0 - a1)) / (2 * (a0 - a1));   /* nearest of the 8 levels, 0 = a1 .. 7 = a0 */
            ai |= (uint64_t)(l == 7 ? 0 : l == 0 ? 1 : 8 - l) << (3 * i);
        }
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(ai >> (8 * k));
    /* colour: inset box, quantised to 5:6:5 and expanded back for the index search */
    int e0[3], e1[3];
    for (int c = 0; c < 3; c++) { int in = (hi[c] - lo[c]) >> 4; e0[c] = std::min(255, hi[c] - in); e1[c] = std::max(0, lo[c] + in); }
    unsigned c0 = rgb565(e0[0], e0[1], e0[2]), c1 = rgb565(e1[0], e1[1], e1[2]);
    int p0[3] = {(int)(c0 >> 11) * 255 / 31, (int)(c0 >> 5 & 63) * 255 / 63, (int)(c0 & 31) * 255 / 31};
    int p1[3] = {(int)(c1 >> 11) * 255 / 31, (int)(c1 >> 5 & 63) * 255 / 63, (int)(c1 & 31) * 255 / 31};
    int d[3] = {p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2]}, dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    uint32_t ci = 0;
    if (dd)
        for (int i = 0; i < 16; i++) {   /* t: where the texel projects on c1 -> c0, in sixths; palette 0 = c0, 1 = c1, 2 = 2/3 c0, 3 = 1/3 c0 */
            int t = ((px[i][0] - p1[0]) * d[0] + (px[i][1] - p1[1]) * d[1] + (px[i][2] - p1[2]) * d[2]) * 6;
            unsigned idx = t >= 5 * dd ? 0 : t >= 3 * dd ? 2 : t >= dd ? 3 : 1;
            ci |= idx << (2 * i);
        }
    out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8); out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
    for (int k = 0; k < 4; k++) out[12 + k] = (uint8_t)(ci >> (8 * k));
}
void bc3_rows(const uint8_t *rgba, unsigned w, unsigned h, uint8_t *dst, unsigned by0, unsigned by1) {
    unsigned bx_n = (w + 3) / 4;
    uint8_t px[16][4];
    for (unsigned by = by0; by < by1; by++)
        for (unsigned bx = 0; bx < bx_n; bx++) {
            for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) {   /* edge blocks repeat the last row / column */
                unsigned sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                memcpy(px[y * 4 + x], rgba + ((size_t)sy * w + sx) * 4, 4);
            }
            bc3_block(px, dst + ((size_t)by * bx_n + bx) * 16);
        }
}
}

// As vk_astc_decode, then BC3 blocks into dst (ceil(w/4) x ceil(h/4) blocks of 16 bytes). 0 = done.
extern "C" int vk_astc_decode_bc3(const uint8_t *src, size_t n, unsigned bw, unsigned bh, unsigned w, unsigned h, int srgb, uint8_t *dst) {
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    int r = vk_astc_decode(src, n, bw, bh, w, h, srgb, rgba.data());
    unsigned rows = (h + 3) / 4;
    std::lock_guard<std::mutex> lock(g_mu);   /* the helper threads are shared with the decoder */
    unsigned nt = (size_t)w * h >= 256u * 256u ? std::max(1u, g_threads) : 1;
    if (nt == 1) bc3_rows(rgba.data(), w, h, dst, 0, rows);
    else run_on(nt, [&](unsigned i) { bc3_rows(rgba.data(), w, h, dst, rows * i / nt, rows * (i + 1) / nt); });
    return r;
}
