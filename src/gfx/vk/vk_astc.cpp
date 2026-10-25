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
