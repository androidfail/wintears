#!/usr/bin/env python3
"""Assembles and validates the three tiny shaders of src/gfx/vk/vk_clear.c (masked clears) into src/gfx/vk/vk_clear_spv.h.

    python tools/build_clear_shaders.py        (needs spirv-as and spirv-val: pacman -S mingw-w64-ucrt-x86_64-spirv-tools)

The header is committed: this only runs when a shader changes. GLSL equivalents:

  vertex: a full-screen triangle, one instance per layer of a layered target
      void main() { vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
                    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); gl_Layer = gl_InstanceIndex; }
  fragment (float / uint): the clear value (4 words of push constant, bits as the game gave them) to every output
      layout(push_constant) uniform PC { uvec4 bits; };   layout(location = N) out vec4 (or uvec4) oN;
      void main() { o0 = uintBitsToFloat(bits); ... o7 = ...; }       (the pipeline's write mask picks the one target)
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'src/gfx/vk/vk_clear_spv.h'
BIN = Path('C:/msys64/ucrt64/bin')

VERT = """
               OpCapability Shader
               OpCapability ShaderLayer
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %out_block %layer %vidx %iidx
               OpDecorate %block Block
               OpMemberDecorate %block 0 BuiltIn Position
               OpDecorate %layer BuiltIn Layer
               OpDecorate %vidx BuiltIn VertexIndex
               OpDecorate %iidx BuiltIn InstanceIndex
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
         %v4 = OpTypeVector %float 4
      %block = OpTypeStruct %v4
  %p_out_blk = OpTypePointer Output %block
  %out_block = OpVariable %p_out_blk Output
  %p_out_int = OpTypePointer Output %int
      %layer = OpVariable %p_out_int Output
   %p_in_int = OpTypePointer Input %int
       %vidx = OpVariable %p_in_int Input
       %iidx = OpVariable %p_in_int Input
   %p_out_v4 = OpTypePointer Output %v4
         %i0 = OpConstant %int 0
         %i1 = OpConstant %int 1
         %i2 = OpConstant %int 2
         %f0 = OpConstant %float 0
         %f1 = OpConstant %float 1
         %f2 = OpConstant %float 2
        %fm1 = OpConstant %float -1
       %main = OpFunction %void None %fn
        %lbl = OpLabel
         %vi = OpLoad %int %vidx
         %sh = OpShiftLeftLogical %int %vi %i1
          %a = OpBitwiseAnd %int %sh %i2
          %b = OpBitwiseAnd %int %vi %i2
         %fa = OpConvertSToF %float %a
         %fb = OpConvertSToF %float %b
         %x2 = OpFMul %float %fa %f2
         %y2 = OpFMul %float %fb %f2
          %x = OpFAdd %float %x2 %fm1
          %y = OpFAdd %float %y2 %fm1
        %pos = OpCompositeConstruct %v4 %x %y %f0 %f1
         %pp = OpAccessChain %p_out_v4 %out_block %i0
               OpStore %pp %pos
         %ii = OpLoad %int %iidx
               OpStore %layer %ii
               OpReturn
               OpFunctionEnd
"""


def frag(as_float):
    outs = ' '.join(f'%o{i}' for i in range(8))
    decor = '\n'.join(f'               OpDecorate %o{i} Location {i}' for i in range(8))
    ovars = '\n'.join(f'         %o{i} = OpVariable %p_out Output' for i in range(8))
    stores = '\n'.join(f'               OpStore %o{i} %val' for i in range(8))
    otype, conv = ('%v4f', '        %val = OpBitcast %v4f %bits') if as_float else ('%v4u', '        %val = OpCopyObject %v4u %bits')
    return f"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %pc {outs}
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %pcblk Block
               OpMemberDecorate %pcblk 0 Offset 0
{decor}
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
        %v4u = OpTypeVector %uint 4
        %v4f = OpTypeVector %float 4
      %pcblk = OpTypeStruct %v4u
       %p_pc = OpTypePointer PushConstant %pcblk
         %pc = OpVariable %p_pc PushConstant
   %p_pc_v4u = OpTypePointer PushConstant %v4u
      %p_out = OpTypePointer Output {otype}
{ovars}
         %u0 = OpConstant %uint 0
       %main = OpFunction %void None %fn
        %lbl = OpLabel
         %pp = OpAccessChain %p_pc_v4u %pc %u0
       %bits = OpLoad %v4u %pp
{conv}
{stores}
               OpReturn
               OpFunctionEnd
"""


def tool(name):
    found = shutil.which(name) or str(BIN / (name + '.exe'))
    if not Path(found).exists():
        sys.exit(f'{name} not found: pacman -S mingw-w64-ucrt-x86_64-spirv-tools')
    return found


def assemble(text, tmp, name):
    src, out = Path(tmp) / (name + '.spvasm'), Path(tmp) / (name + '.spv')
    src.write_text(text)
    subprocess.run([tool('spirv-as'), '--target-env', 'vulkan1.2', str(src), '-o', str(out)], check=True)
    subprocess.run([tool('spirv-val'), '--target-env', 'vulkan1.2', str(out)], check=True)
    data = out.read_bytes()
    assert len(data) % 4 == 0
    return [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        mods = [('vk_clear_vs', assemble(VERT, tmp, 'vs')), ('vk_clear_fs_float', assemble(frag(True), tmp, 'fsf')),
                ('vk_clear_fs_uint', assemble(frag(False), tmp, 'fsu'))]
    lines = ['/* Generated by tools/build_clear_shaders.py (assembled and validated with SPIRV-Tools, Vulkan 1.2 / SPIR-V 1.5). Do not edit. */']
    for name, words in mods:
        lines.append(f'static const uint32_t {name}[{len(words)}] = {{')
        for i in range(0, len(words), 8):
            lines.append('    ' + ', '.join(f'0x{w:08x}' for w in words[i:i + 8]) + ',')
        lines.append('};')
    OUT.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print(f'wrote {OUT} ({sum(len(w) for _, w in mods)} words)')


if __name__ == '__main__':
    main()
