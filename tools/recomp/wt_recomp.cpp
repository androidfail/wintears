// Direct Native recompiler (vault: Wintears Direct Native Plan). Reads the game's NSOs, finds every reachable block
// with dynarmic's own A64 translator (the same IR the JIT compiles), and reports what the C back end has to cover.
// It prints totals only, never game code.
//
//   wt_recomp <TOTK_win> survey        IR opcode counts over all reachable blocks
//
// Modules are placed where src/app/main.c maps them (from 0x300000000, main, subsdk0, sdk, 2 MiB aligned), because
// the IR holds absolute addresses; the runtime checks the bases before using translated code.
#include <functional>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include "../../src/host/game_patch.h"
#include <vector>

#include <array>
#include <csetjmp>
#include <memory>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>

#include "wt_native.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/acc_type.h"
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
    uint64_t base = 0, text_lo = 0, text_hi = 0, rw_lo = 0;   // rw: .data and .bss, to the end of img
    std::vector<uint8_t> img;
    std::vector<uint64_t> seeds;   // guest addresses
};

// Switch jump tables (clang's AArch64 form): the case targets are reached only through "br xN", so neither symbols nor
// branches name them (45% of the blocks given to the JIT in play were right after a branch, 48% mid-code). Recognised:
//     cmp  wI, #n ; b.hi default            (n + 1 entries)
//     adrp xT, table ; add xT, xT, #lo12
//     adr  xA, anchor
//     ldrb / ldrh / ldrsw  wE, [xT, xI(, lsl #s)]
//     add  xD, xA, xE(, sxt / lsl #2)
//     br   xD
// in any order within the 24 instructions before the br; target = anchor + entry * 4 (bytes and halfwords unsigned).
template <class Seed> size_t jump_tables(const Module &m, Seed seed) {
    const uint8_t *I = m.img.data(); size_t found = 0;
    auto reg_def = [&](uint64_t from, uint64_t lo, unsigned r, uint64_t *at) {   // the last instruction before `from` writing xr
        for (uint64_t a = from; a > lo; ) { a -= 4; uint32_t w = rd32(I + a);
            if ((w & 31) == r && (w & 0x1F000000) != 0x14000000 && (w & 0x7C000000) != 0x14000000) { *at = a; return w; }   // not a branch (Rd field)
        }
        return 0u;
    };
    for (uint64_t a = m.text_lo; a < m.text_hi; a += 4) {
        uint32_t br = rd32(I + a);
        if ((br & 0xFFFFFC1F) != 0xD61F0000) continue;
        uint64_t lo = a >= m.text_lo + 4 * 24 ? a - 4 * 24 : m.text_lo, at;
        unsigned rd = br >> 5 & 31;
        uint32_t add = reg_def(a, lo, rd, &at);
        unsigned ra, re, sh;
        if ((add & 0xFFE00000) == 0x8B200000) { ra = add >> 5 & 31; re = add >> 16 & 31; sh = add >> 10 & 7; }        // add xD, xA, wE, sxtb/sxth/sxtw/uxt #s
        else if ((add & 0xFF200000) == 0x8B000000 && !(add >> 22 & 3)) { ra = add >> 5 & 31; re = add >> 16 & 31; sh = add >> 10 & 63; }   // lsl #s
        else continue;
        if (sh != 2) continue;
        uint64_t at_add = at;
        uint32_t adr = reg_def(at_add, lo, ra, &at);
        if ((adr & 0x9F000000) != 0x10000000) continue;
        int64_t aimm = (int64_t)(((adr >> 5 & 0x7FFFF) << 2 | (adr >> 29 & 3)) << 43) >> 43;
        uint64_t anchor = at + aimm;
        uint32_t ld = reg_def(at_add, lo, re, &at);
        unsigned esz; bool sgn = false;
        if ((ld & 0xFFE00C00) == 0x38600800) esz = 1;                     // ldrb wE, [xT, xI]
        else if ((ld & 0xFFE00C00) == 0x78600800) esz = 2;                // ldrh
        else if ((ld & 0xFFE00C00) == 0xB8A00800) { esz = 4; sgn = true; }   // ldrsw
        else continue;
        unsigned rt = ld >> 5 & 31, ri = ld >> 16 & 31; uint64_t at_ld = at;
        uint32_t addt = reg_def(at_ld, lo, rt, &at);
        if ((addt & 0xFF800000) != 0x91000000 || (addt >> 5 & 31) != rt) continue;   // add xT, xT, #lo12
        uint32_t adrp = reg_def(at, lo, rt, &at);
        if ((adrp & 0x9F000000) != 0x90000000) continue;
        int64_t pimm = (int64_t)(((uint64_t)(adrp >> 5 & 0x7FFFF) << 2 | (adrp >> 29 & 3)) << 43) >> 31;
        uint64_t table = ((at & ~0xFFFull) + pimm) + ((addt >> 10 & 0xFFF) << ((addt >> 22 & 1) * 12));
        int64_t n = -1;   // the bound: cmp wI / xI, #imm before the load
        for (uint64_t c = at_ld; c > lo && n < 0; ) { c -= 4; uint32_t w = rd32(I + c);
            if ((w & 0x7F80001F) == 0x7100001F && (w >> 5 & 31) == ri) n = (int64_t)((w >> 10 & 0xFFF) << ((w >> 22 & 1) * 12)) + 1; }
        if (n <= 0 || n > 4096 || table + n * esz > m.img.size()) continue;
        for (int64_t k = 0; k < n; k++) {
            const uint8_t *e = I + table + k * esz;
            int64_t v = esz == 1 ? e[0] : esz == 2 ? (int64_t)(e[0] | e[1] << 8) : sgn ? (int64_t)(int32_t)rd32(e) : (int64_t)rd32(e);
            seed(anchor + v * 4);
        }
        found++;
    }
    return found;
}

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
    m.text_lo = mo[0]; m.text_hi = (uint64_t)mo[0] + sz[0]; m.rw_lo = mo[2];
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
    jump_tables(m, seed);
    // code addresses taken in code (callbacks, lambdas passed as function pointers): adr, or adrp + add, into the text
    for (uint64_t a = m.text_lo; a < m.text_hi; a += 4) {
        uint32_t w = rd32(I + a);
        if ((w & 0x9F000000) == 0x10000000) { int64_t imm = (int64_t)(((uint64_t)(w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3)) << 43) >> 43; seed(a + imm); }
        else if ((w & 0x9F000000) == 0x90000000 && a + 4 < m.text_hi) {
            uint32_t n = rd32(I + a + 4); unsigned r = w & 31;
            if ((n & 0xFF800000) != 0x91000000 || (n >> 5 & 31) != r) continue;   // add xR', xR, #lo12 right after
            int64_t page = (int64_t)(((uint64_t)(w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3)) << 43) >> 31;
            seed(((a & ~0xFFFull) + page) + ((n >> 10 & 0xFFF) << ((n >> 22 & 1) * 12)));
        }
    }
    // linear sweep: the word after an unconditional branch (b, br, ret) is reached some other way, if at all. Play found
    // functions there that nothing above names (pointers built at run time); without the JIT each one stopped the game.
    // Zero / udf padding is left out; any other word is an instruction, so a block from padding is only a spare block.
    if (!getenv("WT_NO_SWEEP"))
        for (uint64_t a = m.text_lo + 4; a < m.text_hi; a += 4) {
            uint32_t p = rd32(I + a - 4), w = rd32(I + a);
            if (w >> 16 == 0) continue;
            if ((p & 0xFC000000) == 0x14000000 || (p & 0xFFFFFC1F) == 0xD61F0000 || p == 0xD65F03C0) seed(a);
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

// NZCV liveness. A block that sets all four flags before reading any never sees the ones it was entered with; a block
// whose every exit is a direct jump to such a block can keep its flags in a local (no store, and gcc drops the packing).
bool writes_flags_first(const IR::Block &b) {
    for (auto &in : b) {
        if (in.ReadsFromCPSR()) return false;
        if (in.WritesToCPSR()) return true;   // A64SetNZCV / A64SetNZCVRaw: all four flags
    }
    return false;   // read by the terminal (If) or left as they are
}
struct DirectExits : boost::static_visitor<bool> {   // every exit a direct jump?
    bool operator()(const IR::Term::LinkBlock &) const { return true; }
    bool operator()(const IR::Term::LinkBlockFast &) const { return true; }
    bool operator()(const IR::Term::If &t) const { return boost::apply_visitor(*this, t.then_) && boost::apply_visitor(*this, t.else_); }
    bool operator()(const IR::Term::CheckBit &t) const { return boost::apply_visitor(*this, t.then_) && boost::apply_visitor(*this, t.else_); }
    template <class T> bool operator()(const T &) const { return false; }
};
bool flags_dead_at_exit(const IR::Block &b, const std::function<bool(uint64_t)> &writes_first) {
    for (auto &in : b)   // a block that may stop in the middle (svc, exception) leaves its flags to the runtime
        if (in.GetOpcode() == IR::Opcode::A64CallSupervisor || in.GetOpcode() == IR::Opcode::A64ExceptionRaised) return false;
    DirectExits k;
    if (!boost::apply_visitor(k, b.GetTerminal())) return false;
    std::vector<uint64_t> next;
    successors(b.GetTerminal(), next);
    for (uint64_t n : next) if (!writes_first(n)) return false;
    return !next.empty();
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

// ---------------------------------------------------------------------------------------------------------------
// C back end: one function per block, semantics as dynarmic's x64 back end (src/cpu/native/wt_native.h has the ABI).

const char *ctype(IR::Type t) {
    switch (t) {
    case IR::Type::U1: return "unsigned";
    case IR::Type::U8: return "uint8_t";
    case IR::Type::U16: return "uint16_t";
    case IR::Type::U32: case IR::Type::NZCVFlags: return "uint32_t";
    case IR::Type::U64: return "uint64_t";
    case IR::Type::U128: return "wt_v128";
    default: return nullptr;
    }
}

struct Emitter {
    std::map<const IR::Inst *, int> id;
    const std::unordered_set<uint64_t> *linkable = nullptr;   // blocks this DLL defines: direct jumps chain to them
    bool flags_local = false;   // set per block: keep NZCV in a local, it is dead at every exit
    std::string body, why;   // why: the first opcode not supported
    char buf[512];

    std::string val(const IR::Value &v) {
        if (!v.IsImmediate()) {
            auto it = id.find(v.GetInst());
            return it == id.end() ? std::string("/*?*/0") : "v" + std::to_string(it->second);
        }
        switch (v.GetType()) {
        case IR::Type::U1: return v.GetU1() ? "1u" : "0u";
        case IR::Type::U8: return std::to_string(v.GetU8()) + "u";
        case IR::Type::U16: return std::to_string(v.GetU16()) + "u";
        case IR::Type::U32: return std::to_string(v.GetU32()) + "u";
        case IR::Type::U64: snprintf(buf, sizeof buf, "0x%llxull", (unsigned long long)v.GetU64()); return buf;
        case IR::Type::A64Reg: return std::to_string(static_cast<int>(v.GetA64RegRef()));
        case IR::Type::A64Vec: return std::to_string(static_cast<int>(v.GetA64VecRef()));
        case IR::Type::Cond: return std::to_string(static_cast<int>(v.GetCond()));
        default: return "/*imm?*/0";
        }
    }
    void line(const std::string &s) { body += "    " + s + "\n"; }

    // flags of an arithmetic parent for the pseudo-ops
    bool flags(const IR::Inst &p, const std::string &r, std::string &nzcv, std::string &carry, std::string &ovf) {
        using O = IR::Opcode;
        O o = p.GetOpcode();
        bool is64 = o == O::Add64 || o == O::Sub64, sub = o == O::Sub32 || o == O::Sub64;
        if (o == O::Add32 || o == O::Add64 || sub) {
            std::string a = val(p.GetArg(0)), b = val(p.GetArg(1)), ci = val(p.GetArg(2));
            if (sub) b = is64 ? "(~(uint64_t)" + b + ")" : "(~(uint32_t)" + b + ")";
            nzcv = std::string(is64 ? "wt_add_flags64(" : "wt_add_flags32(") + a + ", " + b + ", " + ci + ", " + r + ")";
            carry = "(" + nzcv + " >> 29 & 1)"; ovf = "(" + nzcv + " >> 28 & 1)";
            return true;
        }
        IR::Type t = p.GetType();
        if (t == IR::Type::U64) { nzcv = "wt_nz64(" + r + ")"; carry = ovf = "0u"; return true; }
        if (t == IR::Type::U32) { nzcv = "wt_nz32(" + r + ")"; carry = ovf = "0u"; return true; }
        return false;
    }

    bool inst(const IR::Inst &in, int n) {
        using O = IR::Opcode;
        auto a = [&](size_t i) { return val(in.GetArg(i)); };
        std::string r = "v" + std::to_string(n);
        const char *T = ctype(in.GetType());
        auto def = [&](const std::string &e) { line(std::string(T) + " " + r + " = " + e + ";"); };
        auto mem_order = [&](size_t i) {
            IR::AccType t = in.GetArg(i).GetAccType();
            return t == IR::AccType::ORDERED || t == IR::AccType::ORDEREDRW || t == IR::AccType::LIMITEDORDERED;
        };
        switch (in.GetOpcode()) {
        case O::Void: case O::Identity: case O::PushRSB: case O::A64InstructionSynchronizationBarrier: return true;
        case O::A64SetCheckBit: line("c->check_bit = " + a(0) + ";"); return true;
        case O::A64GetCFlag: def("(c->nzcv >> 29 & 1)"); return true;
        case O::A64GetNZCVRaw: def("c->nzcv"); return true;
        case O::A64SetNZCVRaw: line("c->nzcv = " + a(0) + " & 0xF0000000u;"); return true;
        case O::A64SetNZCV: line("c->nzcv = " + a(0) + ";"); return true;
        case O::A64GetW: def("(uint32_t)c->x[" + a(0) + "]"); return true;
        case O::A64GetX: def("c->x[" + a(0) + "]"); return true;
        case O::A64GetS: def("wt_v((uint32_t)c->v[" + a(0) + "].lo, 0)"); return true;
        case O::A64GetD: def("wt_v(c->v[" + a(0) + "].lo, 0)"); return true;
        case O::A64GetQ: def("c->v[" + a(0) + "]"); return true;
        case O::A64GetSP: def("c->sp"); return true;
        case O::A64GetFPCR: def("c->fpcr"); return true;
        case O::A64GetFPSR: def("c->fpsr"); return true;
        case O::A64SetW: line("c->x[" + a(0) + "] = (uint32_t)" + a(1) + ";"); return true;
        case O::A64SetX: line("c->x[" + a(0) + "] = " + a(1) + ";"); return true;
        case O::A64SetS: line("c->v[" + a(0) + "] = wt_v((uint32_t)" + a(1) + ".lo, 0);"); return true;
        case O::A64SetD: line("c->v[" + a(0) + "] = wt_v(" + a(1) + ".lo, 0);"); return true;
        case O::A64SetQ: line("c->v[" + a(0) + "] = " + a(1) + ";"); return true;
        case O::A64SetSP: line("c->sp = " + a(0) + ";"); return true;
        case O::A64SetFPCR: line("c->fpcr = " + a(0) + ";"); return true;
        case O::A64SetFPSR: line("c->fpsr = " + a(0) + ";"); return true;
        case O::A64SetPC: line("c->pc = " + a(0) + ";"); return true;
        case O::A64CallSupervisor: line("c->svc = " + a(0) + "; c->halt = WT_HALT_SVC;"); return true;
        case O::A64ExceptionRaised: {
            auto e = static_cast<A64::Exception>(in.GetArg(1).GetU64());
            if (e == A64::Exception::Yield || e == A64::Exception::SendEvent || e == A64::Exception::SendEventLocal) return true;
            line("c->exc_pc = " + a(0) + "; c->exc_kind = " + std::to_string(static_cast<int>(e)) + "u; c->halt = WT_HALT_EXCEPTION;");
            return true;
        }
        case O::A64DataSynchronizationBarrier: case O::A64DataMemoryBarrier: line("__atomic_thread_fence(__ATOMIC_SEQ_CST);"); return true;
        case O::A64GetCNTFRQ: def("19200000u"); return true;
        case O::A64GetCNTPCT: def("c->cntpct(c->cntpct_user)"); return true;
        case O::A64GetCTR: def("0x8444c004u"); return true;
        case O::A64GetDCZID: def("4u"); return true;
        case O::A64GetTPIDR: def("c->tpidr"); return true;
        case O::A64GetTPIDRRO: def("c->tpidrro"); return true;
        case O::A64SetTPIDR: line("c->tpidr = " + a(0) + ";"); return true;
        case O::A64ClearExclusive: line("c->excl_valid = 0;"); return true;
#define LD(OP, TY, UT) case O::OP: \
            if (mem_order(2)) def("__atomic_load_n((" TY " *)(uintptr_t)" + a(1) + ", __ATOMIC_SEQ_CST)"); \
            else def("WT_LD(" UT ", " + a(1) + ")"); return true;
        LD(A64ReadMemory8, "uint8_t", "wt_u8u") LD(A64ReadMemory16, "uint16_t", "wt_u16u")
        LD(A64ReadMemory32, "uint32_t", "wt_u32u") LD(A64ReadMemory64, "uint64_t", "wt_u64u")
#undef LD
        case O::A64ReadMemory128: def("wt_ld128(" + a(1) + ")"); return true;
#define ST(OP, TY, UT) case O::OP: \
            if (mem_order(3)) line("__atomic_store_n((" TY " *)(uintptr_t)" + a(1) + ", " + a(2) + ", __ATOMIC_SEQ_CST);"); \
            else line("WT_ST(" UT ", " + a(1) + ", " + a(2) + ");"); return true;
        ST(A64WriteMemory8, "uint8_t", "wt_u8u") ST(A64WriteMemory16, "uint16_t", "wt_u16u")
        ST(A64WriteMemory32, "uint32_t", "wt_u32u") ST(A64WriteMemory64, "uint64_t", "wt_u64u")
#undef ST
        case O::A64WriteMemory128: line("wt_st128(" + a(1) + ", " + a(2) + ");"); return true;
#define XR(OP, TY) case O::OP: line(std::string(T) + " " + r + "; WT_EXCL_READ(c, " TY ", " + a(1) + ", " + r + ");"); return true;
        XR(A64ExclusiveReadMemory8, "uint8_t") XR(A64ExclusiveReadMemory16, "uint16_t")
        XR(A64ExclusiveReadMemory32, "uint32_t") XR(A64ExclusiveReadMemory64, "uint64_t")
#undef XR
#define XW(OP, TY) case O::OP: line(std::string(T) + " " + r + "; WT_EXCL_WRITE(c, " TY ", " + a(1) + ", " + a(2) + ", " + r + ");"); return true;
        XW(A64ExclusiveWriteMemory8, "uint8_t") XW(A64ExclusiveWriteMemory16, "uint16_t")
        XW(A64ExclusiveWriteMemory32, "uint32_t") XW(A64ExclusiveWriteMemory64, "uint64_t")
#undef XW
        case O::A64ExclusiveReadMemory128: line("wt_v128 " + r + "; WT_EXCL_READ128(c, " + a(1) + ", " + r + ");"); return true;
        case O::A64ExclusiveWriteMemory128: line("uint32_t " + r + "; WT_EXCL_WRITE128(c, " + a(1) + ", " + a(2) + ", " + r + ");"); return true;
        case O::A64InstructionCacheOperationRaised: return true;   // IC IVAU: the recompiled code never changes
        case O::GetCarryFromOp: case O::GetOverflowFromOp: case O::GetNZCVFromOp: {
            if (in.GetArg(0).IsImmediate()) {   // parent folded to a constant: N and Z from it, C and V clear (as the x64 back end)
                if (in.GetOpcode() != O::GetNZCVFromOp) { why = "flag of a folded constant"; return false; }
                def(in.GetArg(0).GetType() == IR::Type::U64 ? "wt_nz64(" + a(0) + ")" : "wt_nz32((uint32_t)" + a(0) + ")");
                return true;
            }
            const IR::Inst &p = *in.GetArg(0).GetInst();
            std::string nz, cf, of;
            if (!flags(p, val(in.GetArg(0)), nz, cf, of)) return false;
            def(in.GetOpcode() == O::GetNZCVFromOp ? nz : in.GetOpcode() == O::GetCarryFromOp ? cf : of);
            return true;
        }
        case O::GetCFlagFromNZCV: def("(" + a(0) + " >> 29 & 1)"); return true;
        case O::NZCVFromPackedFlags: def("(" + a(0) + " & 0xF0000000u)"); return true;
        case O::Pack2x32To1x64: def("((uint64_t)" + a(1) + " << 32 | " + a(0) + ")"); return true;
        case O::Pack2x64To1x128: def("wt_v(" + a(0) + ", " + a(1) + ")"); return true;
        case O::LeastSignificantWord: def("(uint32_t)" + a(0)); return true;
        case O::LeastSignificantHalf: def("(uint16_t)" + a(0)); return true;
        case O::LeastSignificantByte: def("(uint8_t)" + a(0)); return true;
        case O::MostSignificantWord: def("(uint32_t)(" + a(0) + " >> 32)"); return true;
        case O::MostSignificantBit: def("(" + a(0) + " >> 31)"); return true;
        case O::IsZero32: case O::IsZero64: def("(" + a(0) + " == 0)"); return true;
        case O::TestBit: def("(unsigned)(" + a(0) + " >> " + a(1) + " & 1)"); return true;
        case O::ConditionalSelect32: case O::ConditionalSelect64: case O::ConditionalSelectNZCV:
            def("wt_cond(c->nzcv, " + a(0) + ") ? " + a(1) + " : " + a(2)); return true;
        case O::LogicalShiftLeft32: def("wt_lsl32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::LogicalShiftLeft64: def("wt_lsl64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::LogicalShiftRight32: def("wt_lsr32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::LogicalShiftRight64: def("wt_lsr64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::ArithmeticShiftRight32: def("wt_asr32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::ArithmeticShiftRight64: def("wt_asr64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::RotateRight32: def("wt_ror32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::RotateRight64: def("wt_ror64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::LogicalShiftLeftMasked32: def("(" + a(0) + " << (" + a(1) + " & 31))"); return true;
        case O::LogicalShiftLeftMasked64: def("(" + a(0) + " << (" + a(1) + " & 63))"); return true;
        case O::LogicalShiftRightMasked32: def("(" + a(0) + " >> (" + a(1) + " & 31))"); return true;
        case O::LogicalShiftRightMasked64: def("(" + a(0) + " >> (" + a(1) + " & 63))"); return true;
        case O::ArithmeticShiftRightMasked32: def("(uint32_t)((int32_t)" + a(0) + " >> (" + a(1) + " & 31))"); return true;
        case O::ArithmeticShiftRightMasked64: def("(uint64_t)((int64_t)" + a(0) + " >> (" + a(1) + " & 63))"); return true;
        case O::RotateRightMasked32: def("wt_ror32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::RotateRightMasked64: def("wt_ror64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::Add32: def("(uint32_t)(" + a(0) + " + " + a(1) + " + " + a(2) + ")"); return true;
        case O::Add64: def(a(0) + " + " + a(1) + " + " + a(2)); return true;
        case O::Sub32: def("(uint32_t)(" + a(0) + " + ~(uint32_t)" + a(1) + " + " + a(2) + ")"); return true;
        case O::Sub64: def(a(0) + " + ~(uint64_t)" + a(1) + " + " + a(2)); return true;
        case O::Mul32: def("(uint32_t)(" + a(0) + " * " + a(1) + ")"); return true;
        case O::Mul64: def(a(0) + " * " + a(1)); return true;
        case O::SignedMultiplyHigh64: def("(uint64_t)(((__int128)(int64_t)" + a(0) + " * (int64_t)" + a(1) + ") >> 64)"); return true;
        case O::UnsignedMultiplyHigh64: def("(uint64_t)(((unsigned __int128)" + a(0) + " * " + a(1) + ") >> 64)"); return true;
        case O::UnsignedDiv32: def("wt_udiv32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::UnsignedDiv64: def("wt_udiv64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::SignedDiv32: def("wt_sdiv32(" + a(0) + ", " + a(1) + ")"); return true;
        case O::SignedDiv64: def("wt_sdiv64(" + a(0) + ", " + a(1) + ")"); return true;
        case O::And32: case O::And64: def(a(0) + " & " + a(1)); return true;
        case O::AndNot32: case O::AndNot64: def(a(0) + " & ~" + a(1)); return true;
        case O::Eor32: case O::Eor64: def(a(0) + " ^ " + a(1)); return true;
        case O::Or32: case O::Or64: def(a(0) + " | " + a(1)); return true;
        case O::Not32: case O::Not64: def("~" + a(0)); return true;
        case O::SignExtendByteToWord: def("(uint32_t)(int32_t)(int8_t)" + a(0)); return true;
        case O::SignExtendHalfToWord: def("(uint32_t)(int32_t)(int16_t)" + a(0)); return true;
        case O::SignExtendByteToLong: def("(uint64_t)(int64_t)(int8_t)" + a(0)); return true;
        case O::SignExtendHalfToLong: def("(uint64_t)(int64_t)(int16_t)" + a(0)); return true;
        case O::SignExtendWordToLong: def("(uint64_t)(int64_t)(int32_t)" + a(0)); return true;
        case O::ZeroExtendByteToWord: case O::ZeroExtendHalfToWord: def("(uint32_t)" + a(0)); return true;
        case O::ZeroExtendByteToLong: case O::ZeroExtendHalfToLong: case O::ZeroExtendWordToLong: def("(uint64_t)" + a(0)); return true;
        case O::ZeroExtendLongToQuad: def("wt_v(" + a(0) + ", 0)"); return true;
        case O::ByteReverseWord: def("__builtin_bswap32(" + a(0) + ")"); return true;
        case O::ByteReverseHalf: def("__builtin_bswap16(" + a(0) + ")"); return true;
        case O::ByteReverseDual: def("__builtin_bswap64(" + a(0) + ")"); return true;
        case O::CountLeadingZeros32: def("wt_clz32(" + a(0) + ")"); return true;
        case O::CountLeadingZeros64: def("wt_clz64(" + a(0) + ")"); return true;
        case O::ExtractRegister32: def("wt_extr32(" + a(1) + ", " + a(0) + ", " + a(2) + ")"); return true;   // IR: (low, high, lsb)
        case O::ExtractRegister64: def("wt_extr64(" + a(1) + ", " + a(0) + ", " + a(2) + ")"); return true;   // IR: (low, high, lsb)
        case O::ReplicateBit32: def("(uint32_t)-(int32_t)(" + a(0) + " >> " + a(1) + " & 1)"); return true;
        case O::ReplicateBit64: def("(uint64_t)-(int64_t)(" + a(0) + " >> " + a(1) + " & 1)"); return true;
        case O::MaxSigned32: def("((int32_t)" + a(0) + " > (int32_t)" + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::MaxSigned64: def("((int64_t)" + a(0) + " > (int64_t)" + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::MinSigned32: def("((int32_t)" + a(0) + " < (int32_t)" + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::MinSigned64: def("((int64_t)" + a(0) + " < (int64_t)" + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::MaxUnsigned32: case O::MaxUnsigned64: def("(" + a(0) + " > " + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::MinUnsigned32: case O::MinUnsigned64: def("(" + a(0) + " < " + a(1) + " ? " + a(0) + " : " + a(1) + ")"); return true;
        case O::VectorGetElement8: def("(uint8_t)((" + a(1) + " < 8 ? " + a(0) + ".lo >> (8 * " + a(1) + ") : " + a(0) + ".hi >> (8 * (" + a(1) + " - 8))))"); return true;
        case O::VectorGetElement16: def("(uint16_t)((" + a(1) + " < 4 ? " + a(0) + ".lo >> (16 * " + a(1) + ") : " + a(0) + ".hi >> (16 * (" + a(1) + " - 4))))"); return true;
        case O::VectorGetElement32: def("(uint32_t)((" + a(1) + " < 2 ? " + a(0) + ".lo >> (32 * " + a(1) + ") : " + a(0) + ".hi >> (32 * (" + a(1) + " - 2))))"); return true;
        case O::VectorGetElement64: def("(" + a(1) + " ? " + a(0) + ".hi : " + a(0) + ".lo)"); return true;
        case O::VectorZeroUpper: def("wt_v(" + a(0) + ".lo, 0)"); return true;
        case O::VectorTable: {   // TBL tables: up to four registers, unused arguments are Void
            std::string t = "wt_tbl " + r + " = {{";
            unsigned k = 0;
            for (size_t i = 0; i < in.NumArgs(); i++) {
                if (in.GetArg(i).IsEmpty()) continue;
                t += (k ? ", " : "") + a(i); k++;
            }
            line(t + "}, " + std::to_string(k) + "u};");
            return true;
        }
        default: break;
        }
        static const std::unordered_set<std::string> helpers = {
#include "wt_ops_list.inc"
        };
        std::string name = IR::GetNameOf(in.GetOpcode());
        if (!T || !helpers.count(name)) { why = name; return false; }
        // conversions with a rounding argument: the helpers round to nearest only, which is what FPCR 0 gives
        if ((name.rfind("FPFixed", 0) == 0 || name == "FPDoubleToSingle" || name == "FPSingleToHalf" || name == "FPDoubleToHalf") && val(in.GetArg(in.NumArgs() - 1)) != "0u") {
            why = name + " (rounding mode)"; return false;
        }
        std::string call = "wt_op_" + name + "(";
        for (size_t i = 0; i < in.NumArgs(); i++) call += (i ? ", " : "") + a(i);
        def(call + ")");
        return true;
    }

    void term(const IR::Terminal &t, int depth) {
        std::string ind(4 * depth, ' ');
        struct V : boost::static_visitor<void> {
            Emitter &e; int d; std::string ind;
            V(Emitter &e, int d, std::string ind) : e(e), d(d), ind(std::move(ind)) {}
            void pc(const IR::LocationDescriptor &l) {
                snprintf(e.buf, sizeof e.buf, "%sc->pc = 0x%llxull; return;\n", ind.c_str(), (unsigned long long)A64::LocationDescriptor{l}.PC());
                e.body += e.buf;
            }
            void operator()(const IR::Term::Invalid &) { e.body += ind + "c->halt = WT_HALT_UNSUPPORTED; return;\n"; }
            void operator()(const IR::Term::Interpret &t) {
                snprintf(e.buf, sizeof e.buf, "%sc->pc = 0x%llxull; c->halt = WT_HALT_UNSUPPORTED; return;\n", ind.c_str(), (unsigned long long)A64::LocationDescriptor{t.next}.PC());
                e.body += e.buf;
            }
            void operator()(const IR::Term::ReturnToDispatch &) { e.body += ind + "return;\n"; }
            void link(const IR::LocationDescriptor &l) {   // chain: a tail call to the next block unless a halt is asked for
                uint64_t to = A64::LocationDescriptor{l}.PC();
                if (!e.linkable || !e.linkable->count(to)) { pc(l); return; }
                // c->pc is only read once a block returns (jit.cpp), so a chained block leaves it for the next one to set
                snprintf(e.buf, sizeof e.buf, "%sif (c->halt_now) { c->pc = 0x%llxull; return; } { extern void B_%llx(wt_ctx *); __attribute__((musttail)) return B_%llx(c); }\n",
                         ind.c_str(), (unsigned long long)to, (unsigned long long)to, (unsigned long long)to);
                e.body += e.buf;
            }
            void operator()(const IR::Term::LinkBlock &t) { link(t.next); }
            void operator()(const IR::Term::LinkBlockFast &t) { link(t.next); }
            void operator()(const IR::Term::PopRSBHint &) { e.body += ind + "WT_DISPATCH(c);\n"; }        // a return
            void operator()(const IR::Term::FastDispatchHint &) { e.body += ind + "WT_DISPATCH(c);\n"; }  // an indirect branch
            void sub(const IR::Terminal &x) { V v{e, d + 1, ind + "    "}; boost::apply_visitor(v, x); }
            void operator()(const IR::Term::If &t) {
                e.body += ind + "if (wt_cond(c->nzcv, " + std::to_string(static_cast<int>(t.if_)) + ")) {\n"; sub(t.then_);
                e.body += ind + "} else {\n"; sub(t.else_); e.body += ind + "}\n";
            }
            void operator()(const IR::Term::CheckBit &t) {
                e.body += ind + "if (c->check_bit) {\n"; sub(t.then_); e.body += ind + "} else {\n"; sub(t.else_); e.body += ind + "}\n";
            }
            void operator()(const IR::Term::CheckHalt &t) {
                e.body += ind + "if (c->halt || c->halt_now) return;\n"; boost::apply_visitor(*this, t.else_);
            }
        } v{*this, depth, ind};
        boost::apply_visitor(v, t);
    }

    bool block(const IR::Block &b, uint64_t pc, std::string &out) {
        id.clear(); body.clear(); why.clear();
        int n = 0;
        for (auto &in : b) {
            id[&in] = n;
            if (!inst(in, n)) return false;
            n++;
        }
        term(b.GetTerminal(), 1);
        snprintf(buf, sizeof buf, "void B_%llx(wt_ctx *c) {\n", (unsigned long long)pc);
        out += buf;
        if (flags_local) {   // no successor reads the flags this block leaves (flags_dead_at_exit)
            for (size_t i = 0; (i = body.find("c->nzcv", i)) != std::string::npos; i += 3) body.replace(i, 7, "nz_");
            out += "    uint32_t nz_ = c->nzcv;\n";
        }
        out += body; out += "}\n";
        return true;
    }
};

// What kind of address the run-time misses are (TOTK_NATIVE_MISSES file): a known function start, the instruction after a
// call (a return), after an unconditional branch / return (code reached only by an indirect jump: jump tables), or in
// the middle of straight code. Totals only, weighted by how often each missed.
int classify(Program &p, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    const char *kinds[] = {"function start", "after a call (return)", "after a branch / return (indirect jump target)", "mid-block", "outside the modules"};
    uint64_t sites[5] = {}, hits[5] = {}; char line[128];
    while (fgets(line, sizeof line, f)) {
        unsigned long long pc, n; if (sscanf(line, "%llx %llu", &pc, &n) != 2) continue;
        int k = 4;
        for (auto &m : p.mods) {
            if (pc < m.base + m.text_lo || pc >= m.base + m.text_hi) continue;
            uint64_t off = pc - m.base;
            if (std::binary_search(m.seeds.begin(), m.seeds.end(), off)) { k = 0; break; }
            uint32_t prev = off >= 4 ? rd32(&m.img[off - 4]) : 0;
            if ((prev & 0xFC000000) == 0x94000000 || (prev & 0xFFFFFC1F) == 0xD63F0000) k = 1;            // BL, BLR
            else if ((prev & 0xFC000000) == 0x14000000 || (prev & 0xFFFFFC1F) == 0xD61F0000 || prev == 0xD65F03C0) k = 2;   // B, BR, RET
            else k = 3;
            break;
        }
        sites[k]++; hits[k] += n;
    }
    fclose(f);
    uint64_t th = 0; for (auto h : hits) th += h;
    for (int k = 0; k < 5; k++) printf("%-48s %6llu sites, %5.1f%% of misses\n", kinds[k], (unsigned long long)sites[k], th ? 100.0 * hits[k] / th : 0.0);
    return 0;
}
int emit(Program &p, const std::string &dir, const std::string &only, size_t per_file, const char *seeds_file) {
    A64::UserConfig uc;
    uc.define_unpredictable_behaviour = true;
    uc.wall_clock_cntpct = true;
    uc.dczid_el0 = 4;
    std::vector<uint64_t> work;
    for (auto &m : p.mods) if (only == "all" || m.name == only) for (uint64_t s : m.seeds) work.push_back(m.base + s);
    if (seeds_file) {   // block starts seen at run time (TOTK_NATIVE_MISSES: "<hex pc> <count>" lines)
        FILE *f = fopen(seeds_file, "r");
        unsigned long long pc, n;
        size_t k = 0;
        while (f && fscanf(f, "%llx %llu", &pc, &n) == 2) { work.push_back(pc); k++; }
        if (f) fclose(f);
        printf("%zu block starts from %s\n", k, seeds_file);
    }
    auto in_scope = [&](uint64_t pc) {
        for (auto &m : p.mods) if (pc >= m.base + m.text_lo && pc < m.base + m.text_hi) return only == "all" || m.name == only;
        return false;
    };
    // pass 1: every reachable block; pass 2: emit them in address order, one file per 2^per_file bytes of guest code.
    // Files are named by address and only change when their blocks do, so build_native.py's object cache can reuse the
    // compiled ones across rebuilds.
    std::unordered_set<uint64_t> seen, wfirst;   // wfirst: blocks that set NZCV before reading it
    std::vector<uint64_t> next;
    while (!work.empty()) {
        uint64_t pc = work.back(); work.pop_back();
        if (!in_scope(pc) || !seen.insert(pc).second) continue;
        IR::Block b = translate(p, pc, uc);
        if (writes_flags_first(b)) wfirst.insert(pc);
        for (auto &inst : b)
            if (inst.GetOpcode() == IR::Opcode::PushRSB) work.push_back(A64::LocationDescriptor{IR::LocationDescriptor{inst.GetArg(0).GetU64()}}.PC());
        next.clear(); successors(b.GetTerminal(), next);
        for (uint64_t n : next) work.push_back(n);
    }
    std::vector<uint64_t> all(seen.begin(), seen.end());
    std::sort(all.begin(), all.end());
    std::vector<uint64_t> done;
    std::map<std::string, uint64_t> missing;
    Emitter em;
    em.linkable = &seen;
    std::string cur;
    size_t files = 0;
    uint64_t total = all.size(), chunk = ~0ull, n_local = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        char name[64]; snprintf(name, sizeof name, "/blocks_%llx.c", (unsigned long long)chunk);
        FILE *f = fopen((dir + name).c_str(), "wb");
        fputs("#include \"wt_native.h\"\n", f); fputs(cur.c_str(), f); fclose(f);
        cur.clear(); files++;
    };
    for (uint64_t pc : all) {
        if (pc >> per_file != chunk) { flush(); chunk = pc >> per_file; }
        IR::Block b = translate(p, pc, uc);
        em.flags_local = flags_dead_at_exit(b, [&](uint64_t n) { return wfirst.count(n) != 0; });
        n_local += em.flags_local;
        if (!em.block(b, pc, cur)) {   // left to dynarmic: a stub that hands the pc back, for blocks that chain to it
            missing[em.why]++;
            snprintf(em.buf, sizeof em.buf, "void B_%llx(wt_ctx *c) { c->pc = 0x%llxull; }\n", (unsigned long long)pc, (unsigned long long)pc);
            cur += em.buf;
            continue;
        }
        done.push_back(pc);
    }
    flush();
    FILE *f = fopen((dir + "/table.c").c_str(), "wb");
    fputs("#include \"wt_native.h\"\n", f);
    for (uint64_t pc : done) fprintf(f, "void B_%llx(wt_ctx *);\n", (unsigned long long)pc);
    fprintf(f, "__declspec(dllexport) const unsigned long long wt_count = %zu;\n", done.size());
    fprintf(f, "__declspec(dllexport) const unsigned long long wt_bases[3] = {0x%llxull, 0x%llxull, 0x%llxull};\n",
            (unsigned long long)p.mods[0].base, (unsigned long long)p.mods[1].base, (unsigned long long)p.mods[2].base);
    fputs("__declspec(dllexport) const wt_entry wt_table[] = {\n", f);
    for (uint64_t pc : done) fprintf(f, "    {0x%llxull, B_%llx},\n", (unsigned long long)pc, (unsigned long long)pc);
    fputs("};\n", f);
    fclose(f);
    printf("%llu blocks, %zu emitted (%.1f%%) in %zu files; flags kept local in %llu\n", (unsigned long long)total, done.size(),
           100.0 * done.size() / total, files, (unsigned long long)n_local);
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto &[k, n] : missing) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; i++) printf("  left out for %-40s %llu blocks\n", v[i].second.c_str(), (unsigned long long)v[i].first);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// difftest: recompiled blocks against dynarmic, one block at a time from the same random state. The modules are
// mapped at their bases (unrelocated, the same for both sides), registers point into a scratch area whose words point
// back into it, vectors hold random floats. Compared: x0-x30, sp, pc, NZCV, vectors (lanes that are NaN on both
// sides count as equal: NaNs are not bit-exact), svc / exception, and a hash of scratch and module .data/.bss.

struct DiffCb final : A64::UserCallbacks {
    uint64_t tpidr = 0, tpidrro = 0; uint32_t svc = 0; int halted = 0; uint64_t exc_pc = 0; int exc = -1;
    std::unique_ptr<A64::Jit> jit;
    template<typename T> static T ld(uint64_t a) { T v; memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
    template<typename T> static void st(uint64_t a, T v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }
    std::uint8_t MemoryRead8(A64::VAddr a) override { return ld<uint8_t>(a); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return ld<uint16_t>(a); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return ld<uint32_t>(a); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return ld<uint64_t>(a); }
    A64::Vector MemoryRead128(A64::VAddr a) override { return {ld<uint64_t>(a), ld<uint64_t>(a + 8)}; }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { st(a, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { st(a, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { st(a, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { st(a, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override { st(a, v[0]); st(a + 8, v[1]); }
    template<typename T> static bool cas(uint64_t a, T v, T e) { return __atomic_compare_exchange_n((T *)(uintptr_t)a, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {   // as the runtime (jit.cpp): cmpxchg16b
        unsigned __int128 nv = (unsigned __int128)v[1] << 64 | v[0], ne = (unsigned __int128)e[1] << 64 | e[0];
        return __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128 *>(a), ne, nv);
    }
    void InterpreterFallback(A64::VAddr pc, size_t) override { exc_pc = pc; exc = -2; jit->HaltExecution(HaltReason::UserDefined3); }
    void CallSVC(std::uint32_t imm) override { svc = imm; halted = 1; jit->HaltExecution(HaltReason::UserDefined1); }
    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        if (e == A64::Exception::Yield || e == A64::Exception::SendEvent || e == A64::Exception::SendEventLocal) return;
        exc_pc = pc; exc = static_cast<int>(e); jit->HaltExecution(HaltReason::UserDefined3);
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1; }
    std::uint64_t GetCNTPCT() override { return 123456789; }
};

uint64_t fixed_cntpct(void *) { return 123456789; }

void *g_fault_jmp[5];   // __builtin_setjmp: no unwinding, which cannot cross the exception handler
volatile int g_in_test;
LONG CALLBACK fault_handler(EXCEPTION_POINTERS *e) {
    if (g_in_test && e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) __builtin_longjmp(g_fault_jmp, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

uint64_t hash_mem(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; } return h; }

int difftest(Program &p, const std::string &dll, size_t samples, uint64_t seed) {
    // guest memory: modules at their bases, a 64 MiB scratch area
    struct Region { uint8_t *at; size_t n; std::vector<uint8_t> snap; };
    std::vector<Region> rw;
    for (auto &m : p.mods) {
        size_t n = (m.img.size() + 0xFFFF) & ~(size_t)0xFFFF;
        auto *at = static_cast<uint8_t *>(VirtualAlloc((void *)(uintptr_t)m.base, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (at != (uint8_t *)(uintptr_t)m.base) { fprintf(stderr, "cannot map %s at %#llx\n", m.name.c_str(), (unsigned long long)m.base); return 1; }
        memcpy(at, m.img.data(), m.img.size());
        rw.push_back({at + m.rw_lo, m.img.size() - m.rw_lo, {}});
    }
    const uint64_t scratch = 0x2000000000ull; const size_t scratch_n = 8u << 20;
    auto *sc = static_cast<uint8_t *>(VirtualAlloc((void *)(uintptr_t)scratch, scratch_n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!sc) { fprintf(stderr, "cannot map scratch\n"); return 1; }
    uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 1;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (size_t i = 0; i < scratch_n; i += 8) { uint64_t v = scratch + (scratch_n / 4) + (rnd() % (scratch_n / 2) & ~7ull); memcpy(sc + i, &v, 8); }
    rw.push_back({sc, scratch_n, {}});
    for (auto &r : rw) r.snap.assign(r.at, r.at + r.n);
    auto restore = [&]() { for (auto &r : rw) memcpy(r.at, r.snap.data(), r.n); };
    auto mem_hash = [&]() { uint64_t h = 0; for (auto &r : rw) h = h * 31 + hash_mem(r.at, r.n); return h; };

    HMODULE lib = LoadLibraryA(dll.c_str());
    if (!lib) { fprintf(stderr, "cannot load %s\n", dll.c_str()); return 1; }
    auto *cnt = (const unsigned long long *)GetProcAddress(lib, "wt_count");
    auto *tab = (const wt_entry *)GetProcAddress(lib, "wt_table");
    if (!cnt || !tab) { fprintf(stderr, "%s: no table\n", dll.c_str()); return 1; }
    AddVectoredExceptionHandler(1, fault_handler);

    DiffCb cb;
    A64::UserConfig uc;
    ExclusiveMonitor monitor{1};
    uc.callbacks = &cb; uc.tpidr_el0 = &cb.tpidr; uc.tpidrro_el0 = &cb.tpidrro; uc.global_monitor = &monitor;
    uc.cntfrq_el0 = 19200000; uc.dczid_el0 = 4; uc.ctr_el0 = 0x8444c004;
    uc.define_unpredictable_behaviour = true; uc.wall_clock_cntpct = true;
    uc.enable_cycle_counting = true;
    uc.unsafe_optimizations = true; uc.optimizations |= OptimizationFlag::Unsafe_InaccurateNaN;
    // exactly one block per Run: no linking, return stack or fast dispatch past the cycle check
    uc.optimizations &= ~(OptimizationFlag::BlockLinking | OptimizationFlag::ReturnStackBuffer | OptimizationFlag::FastDispatch);
    cb.jit = std::make_unique<A64::Jit>(uc);
    A64::UserConfig tc; tc.define_unpredictable_behaviour = true; tc.wall_clock_cntpct = true;

    std::map<std::string, uint64_t> tested_ops, bad_ops;
    size_t ok = 0, bad = 0, faulted = 0, shown = 0;
    volatile int no_halt = 1;   // chained blocks stop after the first: one block, as on the dynarmic side
    for (size_t s = 0; s < samples; s++) {
        const wt_entry &e = tab[rnd() % *cnt];
        // random state
        wt_ctx n{};
        for (int i = 0; i < 31; i++) n.x[i] = (rnd() & 3) ? scratch + scratch_n / 4 + (rnd() % (scratch_n / 2) & ~7ull) : rnd() % 4096;
        n.sp = scratch + scratch_n / 2;
        static const float picks[] = {0.0f, -0.0f, 1.0f, -1.5f, 0.5f, 3.25f, 1e-40f, 1e30f, -7.0f, 100.0f};
        for (int i = 0; i < 32; i++) {
            float f[4]; for (int k = 0; k < 4; k++) f[k] = (rnd() & 1) ? picks[rnd() % 10] : (float)(int32_t)(rnd() % 20001 - 10000) / 64.0f;
            memcpy(&n.v[i], f, 16);
            if (rnd() % 4 == 0) { double d[2] = {(double)(int32_t)(rnd() % 20001 - 10000) / 64.0, (double)picks[rnd() % 10]}; memcpy(&n.v[i], d, 16); }
        }
        n.nzcv = (uint32_t)(rnd() & 0xF) << 28; n.pc = e.pc; n.tpidr = scratch + 4096; n.tpidrro = scratch + 8192;
        n.halt_request = (volatile int *)&no_halt; n.cntpct = fixed_cntpct;
        n.fpcr = (rnd() & 1) ? 0x01000000u : 0;   // half with FZ, as TOTK's threads run
        wt_ctx start = n;
        restore();
        g_in_test = 1;
        static unsigned mx_host = _mm_getcsr();
        if (__builtin_setjmp(g_fault_jmp)) { g_in_test = 0; _mm_setcsr(mx_host); faulted++; continue; }
        unsigned mx = _mm_getcsr();
        if (n.fpcr) _mm_setcsr(mx | 0x8040u);   // FTZ + DAZ, as the runtime does under FZ
        e.fn(&n);
        _mm_setcsr(mx);
        g_in_test = 0;
        uint64_t h_native = mem_hash();
        // dynarmic, one block
        restore();
        auto &j = *cb.jit;
        std::array<uint64_t, 31> regs; memcpy(regs.data(), start.x, sizeof regs); j.SetRegisters(regs);
        std::array<A64::Vector, 32> vec; for (int i = 0; i < 32; i++) vec[i] = {start.v[i].lo, start.v[i].hi}; j.SetVectors(vec);
        j.SetSP(start.sp); j.SetPC(start.pc); j.SetFpcr(start.fpcr); j.SetFpsr(0); j.SetPstate(start.nzcv); j.ClearExclusiveState();
        cb.tpidr = start.tpidr; cb.tpidrro = start.tpidrro; cb.svc = 0; cb.halted = 0; cb.exc = -1;
        g_in_test = 1;
        if (__builtin_setjmp(g_fault_jmp)) { g_in_test = 0; faulted++; cb.jit = std::make_unique<A64::Jit>(uc); continue; }
        j.Run();
        g_in_test = 0;
        j.ClearHalt(HaltReason::UserDefined1); j.ClearHalt(HaltReason::UserDefined3);
        uint64_t h_jit = mem_hash();
        // compare
        std::string diff;
        auto regs2 = j.GetRegisters();
        for (int i = 0; i < 31; i++) if (regs2[i] != n.x[i]) { diff += " x" + std::to_string(i); }
        if (j.GetSP() != n.sp) diff += " sp";
        if (j.GetPC() != n.pc && n.halt != WT_HALT_EXCEPTION && cb.exc == -1) diff += " pc";
        auto wf = [&](uint64_t x) { return p.code(x) && writes_flags_first(translate(p, x, tc)); };
        if ((j.GetPstate() & 0xF0000000u) != n.nzcv && !flags_dead_at_exit(translate(p, e.pc, tc), wf)) diff += " nzcv";   // dead flags are left as they were
        auto vec2 = j.GetVectors();
        for (int i = 0; i < 32; i++) {
            uint64_t a[2] = {vec2[i][0], vec2[i][1]}, b[2] = {n.v[i].lo, n.v[i].hi};
            if (!memcmp(a, b, 16)) continue;
            bool nan_only = true;
            for (int k = 0; k < 4 && nan_only; k++) {
                uint32_t x = (uint32_t)(a[k / 2] >> (32 * (k & 1))), y = (uint32_t)(b[k / 2] >> (32 * (k & 1)));
                if (x != y && !((x & 0x7FFFFFFF) > 0x7F800000 && (y & 0x7FFFFFFF) > 0x7F800000)) nan_only = false;
            }
            if (!nan_only) {   // or 64-bit lanes NaN on both sides
                nan_only = true;
                for (int k = 0; k < 2; k++) if (a[k] != b[k] && !(((a[k] << 1) > (0x7FFull << 53)) && ((b[k] << 1) > (0x7FFull << 53)))) nan_only = false;
            }
            if (!nan_only) diff += " v" + std::to_string(i);
        }
        if ((n.halt == WT_HALT_SVC) != (cb.halted == 1) || (cb.halted && cb.svc != n.svc)) diff += " svc";
        if (h_native != h_jit) diff += " memory";
        IR::Block b = translate(p, e.pc, tc);
        std::unordered_set<std::string> ops;
        for (auto &in : b) ops.insert(IR::GetNameOf(in.GetOpcode()));
        for (auto &o : ops) tested_ops[o]++;
        if (diff.empty()) { ok++; continue; }
        bad++;
        for (auto &o : ops) bad_ops[o]++;
        if (shown++ < 12) {
            std::string list; for (auto &o : ops) list += o + " ";
            printf("mismatch at block %llu of %llu:%s | ops: %s\n", (unsigned long long)(&e - tab), (unsigned long long)*cnt, diff.c_str(), list.c_str());
        }
    }
    printf("%zu blocks: %zu equal, %zu different, %zu faulted (wild addresses, skipped)\n", samples, ok, bad, faulted);
    std::vector<std::pair<double, std::string>> v;
    for (auto &[o, nb] : bad_ops) v.emplace_back((double)nb / tested_ops[o], o);
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 25; i++)
        printf("  %-45s in %5.1f%% of the different blocks that use it (%llu of %llu)\n", v[i].second.c_str(), 100 * v[i].first,
               (unsigned long long)bad_ops[v[i].second], (unsigned long long)tested_ops[v[i].second]);
    return bad ? 3 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: wt_recomp <TOTK_win> survey | emit <dir> <module|all> [log2 bytes of guest code per file [seeds file]] | difftest <dll> <samples> [seed]\n"); return 2; }
    Program p;
    uint64_t at = 0x300000000ull;
    for (const char *n : {"main", "subsdk0", "sdk"}) {
        Module m; m.name = n;
        if (!load(std::string(argv[1]) + "/data/v0/exefs/" + n, m)) { fprintf(stderr, "cannot load %s\n", n); return 1; }
        m.base = at;
        if (!strcmp(n, "main")) printf("real-time frame step: %d instructions patched\n", gp_apply_realtime(m.img.data(), m.base, m.text_hi));
        at = (at + ((m.img.size() + 0xfff) & ~0xfffull) + 0x1fffff) & ~0x1fffffull;
        printf("%s at 0x%llx, %zu function starts\n", n, (unsigned long long)m.base, m.seeds.size());
        p.mods.push_back(std::move(m));
    }
    if (!strcmp(argv[2], "survey")) return survey(p);
    if (!strcmp(argv[2], "classify") && argc >= 4) return classify(p, argv[3]);
    if (!strcmp(argv[2], "emit") && argc >= 5) return emit(p, argv[3], argv[4], argc > 5 ? (size_t)atoll(argv[5]) : 14, argc > 6 ? argv[6] : nullptr);   // 16 KiB of guest code a file
    if (!strcmp(argv[2], "difftest") && argc >= 5) return difftest(p, argv[3], (size_t)atoll(argv[4]), argc > 5 ? (uint64_t)atoll(argv[5]) : 1);
    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
