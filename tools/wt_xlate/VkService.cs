// WinTears: the runtime shader translation service for Vulkan (wt_xlate --service --out <dir>), in place of upstream's
// tools/totk_xlate/Service.cs (which turns the SPIR-V into Metal with SPIRV-Cross). The translator itself is upstream's
// Program.cs (SwshXlate.Translate, compiled unchanged) on the vendored Ryujinx shader translator with upstream's patches.
//
// Request (one JSON line, as upstream's): {"variant", "stage": "vs|ps|cs", "code": path, "c1": hex, "tex": {slot: type},
//   "img": {slot: {type, fmt}}, "attr": {loc: float|sint|uint}, "fout": {idx: ...}, "local": [x, y, z], "depth_minus_w": bool}
// Reply: {"ok": true} or {"ok": false, "error": "..."}; outputs <out>/<variant>.spv and <variant>.wtr.
//
// The SPIR-V's resources are moved to descriptor set 0 (vertex, compute) or 1 (fragment), so a pipeline's two stages
// never share a binding number. <variant>.wtr says what the replay binds at each binding, one line per item:
//   stage vs|ps|cs              local X Y Z            in LOC...             out LOC...
//   dep tex|img|attr|fout SLOT VALUE                  (the answers the translation depended on)
//   bind BINDING TYPE COUNT KIND REG SREG OFF RW DIM
//     TYPE ubo ssbo cis (combined image sampler) si (sampled image) samp utb (uniform texel buffer) simg stb
//     KIND support c1 drv0 drv2 cbuf (user uniform REG) sbuf (storage slot REG) ubo_ptr (compute uniform REG as
//          storage) sbuf_ubo (address at OFF in user uniform REG) tex (texture slot REG, sampler slot SREG or -1 =
//          the combined handle's) samp (sampler slot REG) img (image slot REG) none (unknown: bind a dummy)
//     DIM 1d 2d 3d cube buffer, with a / ms / s suffix letters for array / multisample / shadow (textures, images)
// The slot rules (driver segment offsets) are upstream Service.cs's.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

static class ShaderService
{
    const int TexBase = 0x168, SampBase = 0x568, ImgBase = 0x120, CombinedBase = 0x20, CombinedSlots = 32;
    static string outDir;
    static int debugMode;

    static long Long(JsonNode n) => n.AsValue().TryGetValue(out long l) ? l : (long)n.GetValue<double>();
    static JsonObject Obj(JsonObject o, string k) => o.TryGetPropertyValue(k, out JsonNode v) && v is JsonObject j ? j : new JsonObject();
    static string I(long v) => v.ToString(CultureInfo.InvariantCulture);

    // ---- SPIR-V: names, set / binding decorations, input / output locations; set numbers rewritten in place ----
    sealed class Spv
    {
        public uint[] W;
        public Dictionary<uint, string> Names = new();
        public Dictionary<uint, int> Binding = new(), Location = new();
        public Dictionary<uint, uint> StorageOf = new();   // variable id -> storage class
        public HashSet<uint> BuiltIn = new();
        public bool WritesDepth;
        public Spv(byte[] b)
        {
            W = new uint[b.Length / 4]; Buffer.BlockCopy(b, 0, W, 0, b.Length);
            if (W.Length < 5 || W[0] != 0x07230203) throw new InvalidDataException("not SPIR-V");
            var depthIds = new HashSet<uint>();
            for (int i = 5; i < W.Length; i += (int)(W[i] >> 16))
            {
                uint op = W[i] & 0xffff, n = W[i] >> 16;
                if (n == 0) throw new InvalidDataException("bad SPIR-V word count");
                if (op == 5) Names[W[i + 1]] = Str(i + 2, (int)n - 2);
                else if (op == 71 && n >= 4)
                {
                    uint id = W[i + 1], dec = W[i + 2];
                    if (dec == 33) Binding[id] = (int)W[i + 3];
                    else if (dec == 30) Location[id] = (int)W[i + 3];
                    else if (dec == 11) { BuiltIn.Add(id); if (W[i + 3] == 22) depthIds.Add(id); }   // FragDepth
                }
                else if (op == 59) StorageOf[W[i + 2]] = W[i + 3];
            }
            WritesDepth = depthIds.Any(id => StorageOf.TryGetValue(id, out uint sc) && sc == 3);
        }
        string Str(int at, int words)
        {
            var b = new List<byte>();
            for (int k = 0; k < words; k++) for (int s = 0; s < 32; s += 8) { byte c = (byte)(W[at + k] >> s); if (c == 0) return Encoding.UTF8.GetString(b.ToArray()); b.Add(c); }
            return Encoding.UTF8.GetString(b.ToArray());
        }
        public void SetDescriptorSet(uint set)
        {
            for (int i = 5; i < W.Length; i += (int)(W[i] >> 16))
                if ((W[i] & 0xffff) == 71 && (W[i] >> 16) >= 4 && W[i + 2] == 34) W[i + 3] = set;
        }
        public byte[] Bytes() { var b = new byte[W.Length * 4]; Buffer.BlockCopy(W, 0, b, 0, b.Length); return b; }
    }

    // ---- NVN slot of a texture / image / sampler handle (upstream Service.cs DecodeTex, from the handle itself) ----
    static long? Slot(long word, long @base, long cb) => cb == 2 && word * 4 >= @base && (word * 4 - @base) % 8 == 0 ? (word * 4 - @base) / 8 : null;
    static (long t, long s) TexSlots(long h, long cbufSlot, bool image)
    {
        long tb = cbufSlot >= 0 ? (cbufSlot & 0xffff) : 2;
        long sb = cbufSlot >= 0 && (cbufSlot >> 16) != 0 ? (cbufSlot >> 16) - 1 : tb;
        long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = (h >> 28) & 0xf;
        long[] words = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
        if (image) return (words.Select(x => ImgBase <= x * 4 && x * 4 < TexBase ? Slot(x, ImgBase, tb) : null).FirstOrDefault(v => v != null) ?? -1, -1);
        long? t = words.Select(x => x * 4 < SampBase ? Slot(x, TexBase, tb) : null).FirstOrDefault(v => v != null);
        t ??= words.Select(x => CombinedBase <= x * 4 && x * 4 < CombinedBase + 8 * CombinedSlots ? Slot(x, CombinedBase, tb) : null).FirstOrDefault(v => v != null);
        long? s = words.Select(x => x * 4 >= SampBase ? Slot(x, SampBase, sb) : null).FirstOrDefault(v => v != null);
        return (t ?? -1, s ?? -1);
    }
    static string Dim(long type)
    {
        string d = (type & 0xff) switch { 1 => "1d", 2 => "buffer", 3 => "2d", 4 => "3d", 5 => "cube", _ => "2d" };
        if ((type & 0x100) != 0) d += "a";
        if ((type & 0x200) != 0) d += "m";
        if ((type & 0x400) != 0) d += "s";
        return d;
    }

    static void Handle(JsonObject rq)
    {
        string variant = (string)rq["variant"], stage = (string)rq["stage"];
        if (stage != "vs" && stage != "ps" && stage != "cs") throw new Exception($"stage {stage} not supported by the translation chain");
        var job = new JsonObject
        {
            ["key"] = variant, ["stage"] = stage, ["code"] = (string)rq["code"], ["c1"] = rq.TryGetPropertyValue("c1", out JsonNode c1) ? (string)c1 : "",
            ["debug"] = debugMode != 0, ["reduced"] = true,
            ["tex"] = Obj(rq, "tex").DeepClone(), ["img"] = Obj(rq, "img").DeepClone(), ["attr"] = Obj(rq, "attr").DeepClone(), ["fout"] = Obj(rq, "fout").DeepClone(),
        };
        if (rq.TryGetPropertyValue("local", out JsonNode local) && local is JsonArray) job["local"] = local.DeepClone();
        job["depth_minus_w"] = rq.TryGetPropertyValue("depth_minus_w", out JsonNode dm) && dm is JsonValue dv && dv.TryGetValue(out bool db) && db;
        string err = SwshXlate.Translate(job, outDir);
        if (err != null) throw new Exception("translation failed: " + err);
        string spvPath = Path.Combine(outDir, variant + ".spv"), resPath = Path.Combine(outDir, variant + ".res.json");
        JsonObject res = JsonNode.Parse(File.ReadAllText(resPath)).AsObject();
        var spv = new Spv(File.ReadAllBytes(spvPath));
        spv.SetDescriptorSet(stage == "ps" ? 1u : 0u);

        var cb = res["cbufs"].AsArray().ToDictionary(b => Long(b["binding"]), b => Long(b["slot"]));
        var sb = res["sbufs"].AsArray().ToDictionary(b => Long(b["binding"]), b => b.AsObject());
        var tx = res["textures"].AsArray().ToDictionary(b => Long(b["binding"]), b => b.AsObject());
        var im = res["images"].AsArray().ToDictionary(b => Long(b["binding"]), b => b.AsObject());
        long sbBase = stage switch { "vs" => 0x110, "cs" => 0x310, _ => 0x510 };
        var lines = new List<string> { "wt-vk 1", "stage " + stage };
        var texSlots = new SortedSet<long>(res["tex_queried"].AsArray().Select(Long));
        foreach (var kv in spv.Binding.OrderBy(kv => kv.Value))
        {
            long b = kv.Value; string type, kind = "none"; long reg = -1, sreg = -1, off = 0, count = 1; bool rw = false; string dim = "-";
            if (cb.TryGetValue(b, out long slot))
            {
                type = "ubo";
                if (slot >= 3 && slot <= 16) { kind = "cbuf"; reg = slot - 3; }
                else kind = slot == 1 ? "c1" : slot == 0 ? "drv0" : slot == 2 ? "drv2" : "none";
            }
            else if (sb.TryGetValue(b, out JsonObject s))
            {
                type = "ssbo"; long cs = Long(s["cb_slot"]), co = Long(s["cb_offset"]) * 4; rw = (Long(s["flags"]) & 1) != 0;
                if (cs == 0 && stage == "cs" && co >= 0x210 && co < 0x310 && (co - 0x210) % 16 == 0) { kind = "ubo_ptr"; reg = (co - 0x210) / 16; }
                else if (cs >= 3 && cs <= 16) { kind = "sbuf_ubo"; reg = cs - 3; off = co; }
                else if (cs == 0 && co >= sbBase && co < sbBase + 0x100 && (co - sbBase) % 16 == 0) { kind = "sbuf"; reg = (co - sbBase) / 16; }
            }
            else if (tx.TryGetValue(b, out JsonObject t))
            {
                long ty = Long(t["type"]); bool sep = t["separate"] is JsonValue sv && sv.GetValue<bool>();
                count = Math.Max(1, Long(t["array"]));
                var (ts, ss) = TexSlots(Long(t["handle"]), Long(t["cbuf_slot"]), false);
                if ((ty & 0xff) == 0) { type = "samp"; kind = ss >= 0 ? "samp" : "none"; reg = ss; }
                else
                {
                    type = (ty & 0xff) == 2 ? "utb" : sep ? "si" : "cis"; dim = Dim(ty);
                    if (ts >= 0) { kind = "tex"; reg = ts; sreg = ss; texSlots.Add(ts); }
                }
            }
            else if (im.TryGetValue(b, out JsonObject g))
            {
                long ty = Long(g["type"]); type = (ty & 0xff) == 2 ? "stb" : "simg"; dim = Dim(ty); rw = true;
                count = Math.Max(1, Long(g["array"]));
                var (ts, _) = TexSlots(Long(g["handle"]), Long(g["cbuf_slot"]), true);
                if (ts >= 0) { kind = "img"; reg = ts; }
            }
            else if (b == 0 && spv.Names.TryGetValue(kv.Key, out string nm) && nm == "support_buffer") { type = "ubo"; kind = "support"; }
            else if (b == 0) { type = "ubo"; kind = "support"; }
            else throw new Exception($"binding {b} of {variant} is in no resource list");
            lines.Add($"bind {I(b)} {type} {I(count)} {kind} {I(reg)} {I(sreg)} {I(off)} {(rw ? 1 : 0)} {dim}");
        }
        var ins = spv.Location.Where(kv => spv.StorageOf.TryGetValue(kv.Key, out uint sc) && sc == 1 && !spv.BuiltIn.Contains(kv.Key)).Select(kv => (long)kv.Value).OrderBy(x => x).ToList();
        var outs = spv.Location.Where(kv => spv.StorageOf.TryGetValue(kv.Key, out uint sc) && sc == 3 && !spv.BuiltIn.Contains(kv.Key)).Select(kv => (long)kv.Value).OrderBy(x => x).ToList();
        lines.Add("in " + string.Join(" ", ins.Select(I)));
        lines.Add("out " + string.Join(" ", outs.Select(I)));
        if (stage == "cs") { var l = rq["local"].AsArray(); lines.Add($"local {I(Long(l[0]))} {I(Long(l[1]))} {I(Long(l[2]))}"); }
        if (spv.WritesDepth) lines.Add("depth 1");
        // the answers this translation depended on (upstream: queried / used texture slots, consumed inputs, written outputs)
        JsonObject tex = Obj(rq, "tex"), img = Obj(rq, "img"), attr = Obj(rq, "attr"), fout = Obj(rq, "fout");
        foreach (long s in texSlots) lines.Add($"dep tex {I(s)} {(tex.TryGetPropertyValue(I(s), out JsonNode tv) ? (string)tv : "2d")}");
        foreach (long s in res["img_queried"].AsArray().Select(Long))
        {
            JsonObject io = img.TryGetPropertyValue(I(s), out JsonNode iv) && iv is JsonObject o ? o : new JsonObject();
            lines.Add($"dep img {I(s)} {(io.TryGetPropertyValue("type", out JsonNode a) ? (string)a : "-")} {(io.TryGetPropertyValue("fmt", out JsonNode f) ? (string)f : "-")}");
        }
        if (stage == "vs") foreach (long s in ins) lines.Add($"dep attr {I(s)} {(attr.TryGetPropertyValue(I(s), out JsonNode av) ? (string)av : "float")}");
        if (stage == "ps") foreach (long s in outs) lines.Add($"dep fout {I(s)} {(fout.TryGetPropertyValue(I(s), out JsonNode fv) ? (string)fv : "float")}");
        foreach (JsonNode n in res["notes"].AsArray()) lines.Add("note " + ((string)n).Replace('\n', ' '));
        File.WriteAllBytes(spvPath, spv.Bytes());
        string tmp = Path.Combine(outDir, variant + ".wtr.tmp");
        File.WriteAllText(tmp, string.Join("\n", lines) + "\n");
        File.Move(tmp, Path.Combine(outDir, variant + ".wtr"), true);
        File.Delete(resPath);
    }

    public static int Run(string[] args)
    {
        string dm = Environment.GetEnvironmentVariable("TOTK_SHADER_DEBUGMODE");
        debugMode = string.IsNullOrEmpty(dm) ? 1 : int.Parse(dm.Trim(), CultureInfo.InvariantCulture);   // upstream's default: AST optimizer off
        for (int i = 1; i < args.Length; i++)
        {
            if (args[i] == "--out" && i + 1 < args.Length) outDir = args[++i];
            else { Console.Error.WriteLine($"wt_xlate --service: unknown argument {args[i]}"); return 2; }
        }
        if (outDir == null) { Console.Error.WriteLine("usage: wt_xlate --service --out <dir>"); return 2; }
        Directory.CreateDirectory(outDir);
        var stdout = Console.Out;
        string line;
        while ((line = Console.In.ReadLine()) != null)
        {
            if (line.Trim().Length == 0) continue;
            string reply;
            try { Handle(JsonNode.Parse(line).AsObject()); reply = "{\"ok\": true}"; }
            catch (Exception e) { reply = new JsonObject { ["ok"] = false, ["error"] = $"{e.GetType().Name}: {e.Message}".Replace('\n', ' ') }.ToJsonString(); }
            stdout.Write(reply + "\n");
            stdout.Flush();
        }
        return 0;
    }
}
