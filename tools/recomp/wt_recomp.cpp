// Direct Native recompiler (vault: Wintears Direct Native Plan). Reads the game's NSOs, finds every reachable block
// with dynarmic's own A64 translator (the same IR the JIT compiles), and reports what the C back end has to cover.
// It prints totals only, never game code.
//
//   wt_recomp <TOTK_win> survey        IR opcode counts over all reachable blocks
//
// Modules are placed where src/app/main.c maps them (from 0x300000000, main, subsdk0, sdk, 2 MiB aligned), because
// the IR holds absolute addresses; the runtime checks the bases before using translated code.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

namespace {

uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

bool lz4(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned t = s[i++]; size_t lit = t >> 4;
        if (lit == 15) for (unsigned b = 255; b == 255 && i < n;) { b = s[i++]; lit += b; }
        if (lit > n - i || lit > cap - o) return false;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i == n) break;
        size_t off = s[i] | (size_t)s[i + 1] << 8; i += 2;
        if (!off || off > o) return false;
        size_t len = (t & 15) + 4;
        if ((t & 15) == 15) for (unsigned b = 255; b == 255 && i < n;) { b = s[i++]; len += b; }
        if (len > cap - o) return false;
        for (size_t k = 0; k < len; k++, o++) d[o] = d[o - off];
    }
    return o == cap;
}

struct Module {
    std::string name;
    uint64_t base = 0, text_lo = 0, text_hi = 0;
    std::vector<uint8_t> img;
    std::vector<uint64_t> seeds;   // guest addresses
};

bool load(const std::string &path, Module &m) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> nso;
    fseek(f, 0, SEEK_END); nso.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
    bool ok = fread(nso.data(), 1, nso.size(), f) == nso.size(); fclose(f);
    if (!ok || nso.size() < 0x100 || memcmp(nso.data(), "NSO0", 4)) return false;
    uint32_t flags = rd32(&nso[0xC]), fo[3], mo[3], sz[3], st[3];
    for (int k = 0; k < 3; k++) {
        fo[k] = rd32(&nso[0x10 + 16 * k]); mo[k] = rd32(&nso[0x14 + 16 * k]); sz[k] = rd32(&nso[0x18 + 16 * k]);
        st[k] = (flags >> k & 1) ? rd32(&nso[0x60 + 4 * k]) : sz[k];
    }
    m.img.assign((size_t)mo[2] + sz[2] + rd32(&nso[0x3C]), 0);
    for (int k = 0; k < 3; k++) {
        if ((size_t)fo[k] + st[k] > nso.size()) return false;
        if (flags >> k & 1) { if (!lz4(&nso[fo[k]], st[k], &m.img[mo[k]], sz[k])) return false; }
        else memcpy(&m.img[mo[k]], &nso[fo[k]], sz[k]);
    }
    m.text_lo = mo[0]; m.text_hi = (uint64_t)mo[0] + sz[0];
    uint32_t symoff = rd32(&nso[0x98]), symsize = rd32(&nso[0x9C]);
    // function starts: dynamic symbols, .eh_frame_hdr, relative relocations, BL targets (as tools/recomp/survey.py)
    const uint8_t *I = m.img.data();
    uint32_t mod0 = rd32(I + 4);
    if (memcmp(I + mod0, "MOD0", 4)) return false;
    int64_t dyn = (int64_t)mod0 + (int32_t)rd32(I + mod0 + 4);
    uint64_t eh_lo = mod0 + (int32_t)rd32(I + mod0 + 16), eh_hi = mod0 + (int32_t)rd32(I + mod0 + 20);
    std::map<int64_t, uint64_t> t;
    for (int k = 0; k < 4096; k++) { int64_t tag = (int64_t)rd64(I + dyn + 16 * k); if (!tag) break; t.emplace(tag, rd64(I + dyn + 16 * k + 8)); }
    auto seed = [&](uint64_t off) { if (off >= m.text_lo && off < m.text_hi && !(off & 3)) m.seeds.push_back(off); };
    if (t.count(6)) {
        uint64_t n = t.count(4) ? rd32(I + t[4] + 4) : symsize / 24;
        uint64_t symtab = t[6]; (void)symoff;
        for (uint64_t k = 0; k < n; k++) {
            const uint8_t *s = I + symtab + 24 * k;
            if ((s[4] & 15) == 2 && (s[6] | s[7] << 8)) seed(rd64(s + 8));
        }
    }
    if (eh_hi - eh_lo >= 12 && I[eh_lo] == 1 && I[eh_lo + 3] == 0x3B) {
        uint32_t cnt = rd32(I + eh_lo + 8);
        for (uint32_t k = 0; k < cnt; k++) seed(eh_lo + (int64_t)(int32_t)rd32(I + eh_lo + 12 + 8 * k));
    }
    if (t.count(7)) for (uint64_t k = 0; k < t[8] / 24; k++) {
        const uint8_t *r = I + t[7] + 24 * k;
        if ((rd64(r + 8) & 0xFFFFFFFF) == 1027) seed(rd64(r + 16));
    }
    if (t.count(36)) {
        uint64_t where = 0;
        for (uint64_t k = 0; k < t[35] / 8; k++) {
            uint64_t e = rd64(I + t[36] + 8 * k);
            if (!(e & 1)) { where = e; seed(rd64(I + where)); where += 8; }
            else { for (int b = 0; b < 63; b++) if (e >> (b + 1) & 1) seed(rd64(I + where + 8 * b)); where += 63 * 8; }
        }
    }
    for (uint64_t a = m.text_lo; a < m.text_hi; a += 4) {
        uint32_t w = rd32(I + a);
        if ((w & 0xFC000000) == 0x94000000) { int64_t imm = (int64_t)((w & 0x3FFFFFF) << 6) >> 4; seed(a + imm); }
    }
    std::sort(m.seeds.begin(), m.seeds.end());
    m.seeds.erase(std::unique(m.seeds.begin(), m.seeds.end()), m.seeds.end());
    return true;
}

struct Program {
    std::vector<Module> mods;
    std::optional<uint32_t> code(uint64_t va) const {
        for (auto &m : mods)
            if (va >= m.base + m.text_lo && va + 4 <= m.base + m.text_hi) return rd32(&m.img[va - m.base]);
        return std::nullopt;
    }
};

// Successor locations a terminal names directly (indirect exits name none).
void successors(const IR::Terminal &term, std::vector<uint64_t> &out) {
    struct V : boost::static_visitor<void> {
        std::vector<uint64_t> &o;
        explicit V(std::vector<uint64_t> &o) : o(o) {}
        void add(const IR::LocationDescriptor &l) { o.push_back(A64::LocationDescriptor{l}.PC()); }
        void operator()(const IR::Term::Invalid &) {}
        void operator()(const IR::Term::Interpret &t) { add(t.next); }
        void operator()(const IR::Term::ReturnToDispatch &) {}
        void operator()(const IR::Term::LinkBlock &t) { add(t.next); }
        void operator()(const IR::Term::LinkBlockFast &t) { add(t.next); }
        void operator()(const IR::Term::PopRSBHint &) {}
        void operator()(const IR::Term::FastDispatchHint &) {}
        void operator()(const IR::Term::If &t) { boost::apply_visitor(*this, t.then_); boost::apply_visitor(*this, t.else_); }
        void operator()(const IR::Term::CheckBit &t) { boost::apply_visitor(*this, t.then_); boost::apply_visitor(*this, t.else_); }
        void operator()(const IR::Term::CheckHalt &t) { boost::apply_visitor(*this, t.else_); }
    } v{out};
    boost::apply_visitor(v, term);
}

IR::Block translate(const Program &p, uint64_t pc, const A64::UserConfig &uc) {
    auto get = [&](uint64_t va) { return p.code(va); };
    IR::Block b = A64::Translate(A64::LocationDescriptor{pc, FP::FPCR{0}}, get, {uc.define_unpredictable_behaviour, uc.wall_clock_cntpct});
    Optimization::PolyfillPass(b, {});
    Optimization::A64CallbackConfigPass(b, uc);
    Optimization::A64GetSetElimination(b);
    Optimization::DeadCodeElimination(b);
    Optimization::ConstantPropagation(b);
    Optimization::DeadCodeElimination(b);
    Optimization::IdentityRemovalPass(b);
    return b;
}

int survey(Program &p) {
    A64::UserConfig uc;
    uc.define_unpredictable_behaviour = true;
    uc.wall_clock_cntpct = true;
    std::vector<uint64_t> work;
    for (auto &m : p.mods) for (uint64_t s : m.seeds) work.push_back(m.base + s);
    std::unordered_set<uint64_t> seen;
    std::map<std::string, uint64_t> ops;
    uint64_t blocks = 0, insts = 0, guest_insts = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint64_t> next;
    while (!work.empty()) {
        uint64_t pc = work.back(); work.pop_back();
        if (!seen.insert(pc).second || !p.code(pc)) continue;
        IR::Block b = translate(p, pc, uc);
        blocks++;
        guest_insts += (A64::LocationDescriptor{b.EndLocation()}.PC() - pc) / 4;
        for (auto &inst : b) {
            insts++;
            ops[IR::GetNameOf(inst.GetOpcode())]++;
            if (inst.GetOpcode() == IR::Opcode::PushRSB) work.push_back(A64::LocationDescriptor{IR::LocationDescriptor{inst.GetArg(0).GetU64()}}.PC());
        }
        next.clear(); successors(b.GetTerminal(), next);
        for (uint64_t n : next) work.push_back(n);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%llu blocks, %llu guest instructions covered, %llu IR instructions, %zu distinct opcodes, %.1f s\n",
           (unsigned long long)blocks, (unsigned long long)guest_insts, (unsigned long long)insts, ops.size(), s);
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto &[k, n] : ops) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    uint64_t acc = 0;
    for (size_t i = 0; i < v.size(); i++) {
        acc += v[i].first;
        printf("%6zu %-40s %12llu  %6.2f%%  cum %6.2f%%\n", i + 1, v[i].second.c_str(), (unsigned long long)v[i].first,
               100.0 * v[i].first / insts, 100.0 * acc / insts);
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wt_recomp <TOTK_win> survey\n"); return 2; }
    Program p;
    uint64_t at = 0x300000000ull;
    for (const char *n : {"main", "subsdk0", "sdk"}) {
        Module m; m.name = n;
        if (!load(std::string(argv[1]) + "/data/v0/exefs/" + n, m)) { fprintf(stderr, "cannot load %s\n", n); return 1; }
        m.base = at;
        at = (at + ((m.img.size() + 0xfff) & ~0xfffull) + 0x1fffff) & ~0x1fffffull;
        printf("%s at 0x%llx, %zu function starts\n", n, (unsigned long long)m.base, m.seeds.size());
        p.mods.push_back(std::move(m));
    }
    if (!strcmp(argv[2], "survey")) return survey(p);
    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
