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
        case O::A64GetCNTPCT: def("c->cntpct()"); return true;
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
        case O::ExtractRegister32: def("wt_extr32(" + a(0) + ", " + a(1) + ", " + a(2) + ")"); return true;
        case O::ExtractRegister64: def("wt_extr64(" + a(0) + ", " + a(1) + ", " + a(2) + ")"); return true;
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
        default:
            why = IR::GetNameOf(in.GetOpcode());
            return false;
        }
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
            void operator()(const IR::Term::LinkBlock &t) { pc(t.next); }
            void operator()(const IR::Term::LinkBlockFast &t) { pc(t.next); }
            void operator()(const IR::Term::PopRSBHint &) { e.body += ind + "return;\n"; }
            void operator()(const IR::Term::FastDispatchHint &) { e.body += ind + "return;\n"; }
            void sub(const IR::Terminal &x) { V v{e, d + 1, ind + "    "}; boost::apply_visitor(v, x); }
            void operator()(const IR::Term::If &t) {
                e.body += ind + "if (wt_cond(c->nzcv, " + std::to_string(static_cast<int>(t.if_)) + ")) {\n"; sub(t.then_);
                e.body += ind + "} else {\n"; sub(t.else_); e.body += ind + "}\n";
            }
            void operator()(const IR::Term::CheckBit &t) {
                e.body += ind + "if (c->check_bit) {\n"; sub(t.then_); e.body += ind + "} else {\n"; sub(t.else_); e.body += ind + "}\n";
            }
            void operator()(const IR::Term::CheckHalt &t) {
                e.body += ind + "if (c->halt || *c->halt_request) return;\n"; boost::apply_visitor(*this, t.else_);
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
        out += buf; out += body; out += "}\n";
        return true;
    }
};

int emit(Program &p, const std::string &dir, const std::string &only, size_t per_file) {
    A64::UserConfig uc;
    uc.define_unpredictable_behaviour = true;
    uc.wall_clock_cntpct = true;
    uc.dczid_el0 = 4;
    std::vector<uint64_t> work;
    for (auto &m : p.mods) if (only == "all" || m.name == only) for (uint64_t s : m.seeds) work.push_back(m.base + s);
    auto in_scope = [&](uint64_t pc) {
        for (auto &m : p.mods) if (pc >= m.base + m.text_lo && pc < m.base + m.text_hi) return only == "all" || m.name == only;
        return false;
    };
    std::unordered_set<uint64_t> seen;
    std::vector<uint64_t> done;
    std::map<std::string, uint64_t> missing;
    Emitter em;
    std::string cur;
    size_t in_file = 0, files = 0;
    uint64_t total = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        std::string path = dir + "/blocks_" + std::to_string(files++) + ".c";
        FILE *f = fopen(path.c_str(), "wb");
        fputs("#include \"wt_native.h\"\n", f); fputs(cur.c_str(), f); fclose(f);
        cur.clear(); in_file = 0;
    };
    std::vector<uint64_t> next;
    while (!work.empty()) {
        uint64_t pc = work.back(); work.pop_back();
        if (!in_scope(pc) || !seen.insert(pc).second) continue;
        IR::Block b = translate(p, pc, uc);
        total++;
        for (auto &inst : b)
            if (inst.GetOpcode() == IR::Opcode::PushRSB) work.push_back(A64::LocationDescriptor{IR::LocationDescriptor{inst.GetArg(0).GetU64()}}.PC());
        next.clear(); successors(b.GetTerminal(), next);
        for (uint64_t n : next) work.push_back(n);
        if (!em.block(b, pc, cur)) { missing[em.why]++; continue; }
        done.push_back(pc);
        if (++in_file == per_file) flush();
    }
    flush();
    std::sort(done.begin(), done.end());
    FILE *f = fopen((dir + "/table.c").c_str(), "wb");
    fputs("#include \"wt_native.h\"\n", f);
    for (uint64_t pc : done) fprintf(f, "void B_%llx(wt_ctx *);\n", (unsigned long long)pc);
    fprintf(f, "__declspec(dllexport) const unsigned long long wt_count = %zu;\n", done.size());
    fputs("__declspec(dllexport) const wt_entry wt_table[] = {\n", f);
    for (uint64_t pc : done) fprintf(f, "    {0x%llxull, B_%llx},\n", (unsigned long long)pc, (unsigned long long)pc);
    fputs("};\n", f);
    fclose(f);
    printf("%llu blocks, %zu emitted (%.1f%%) in %zu files\n", (unsigned long long)total, done.size(), 100.0 * done.size() / total, files);
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto &[k, n] : missing) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; i++) printf("  left out for %-40s %llu blocks\n", v[i].second.c_str(), (unsigned long long)v[i].first);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wt_recomp <TOTK_win> survey | emit <dir> <module|all> [blocks per file]\n"); return 2; }
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
    if (!strcmp(argv[2], "emit") && argc >= 5) return emit(p, argv[3], argv[4], argc > 5 ? (size_t)atoll(argv[5]) : 2000);
    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
