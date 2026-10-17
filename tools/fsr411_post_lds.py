#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce the pinned fsr4vk v0.4.3 2160/Quality/sRGB output-store rewrite.

Input: upstream assets/fsr411/colors/2160/quality/srgb/pass-27.spv.
Requires SPIRV-Tools on PATH. Used by maintainers, never at application startup.
Emits a shader for validation and optionally the small C++ edit recipe. No model
weights or original shader binary are embedded in that recipe.
"""
import argparse
import difflib
import hashlib
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ORIGINAL_SHA256 = '590d5a48eaf7390059249b59c0b61e664cb6e3198380f0f011b12d165260e7b6'


def transform(source):
    active = ['Out']
    definitions={m[1]:m[2].split() for line in source.splitlines() if (m:=re.match(r'\s*(%\S+) = (.*)',line))}
    assert definitions['%59']==['OpSDiv','%uint','%57','%uint_2']
    assert definitions['%61']==['OpSDiv','%uint','%58','%uint_2']
    assert source.count('OpReturn')==1
    assert not re.search(r'OpVariable\s+%\S+\s+Function',source)
    globals='''
    %bbC2 = OpConstant %uint 2
    %bbC5 = OpConstant %uint 5
    %bbC31 = OpConstant %uint 31
    %bbC32 = OpConstant %uint 32
    %bbC256 = OpConstant %uint 256
    %bbC512 = OpConstant %uint 512
    %bbC768 = OpConstant %uint 768
    %bbC1024 = OpConstant %uint 1024
    %bbC264 = OpConstant %uint 264
    %bbArr = OpTypeArray %v4float %bbC1024
    %bbArrPtr = OpTypePointer Workgroup %bbArr
    %bbVecPtr = OpTypePointer Workgroup %v4float
    %bbOut = OpVariable %bbArrPtr Workgroup
    '''
    source=source.replace('       %main = OpFunction',globals+'       %main = OpFunction',1)
    entry=re.search(r'(%main = OpFunction[^\n]*\n\s*%\S+ = OpLabel\n)',source)
    assert entry
    base='''%bbGroup = OpLoad %v3uint %gl_WorkGroupID
    %bbGx = OpCompositeExtract %uint %bbGroup 0
    %bbGy = OpCompositeExtract %uint %bbGroup 1
    %bbBaseX = OpShiftLeftLogical %uint %bbGx %bbC5
    %bbBaseY = OpShiftLeftLogical %uint %bbGy %bbC5
    '''
    source=source[:entry.end()]+base+source[entry.end():]
    counts={'Rec':0,'Hist':0,'Out':0}
    def store(match):
     image,coord,value=match.groups()
     loaded=definitions[image]
     assert loaded[:2]==['OpLoad','%24']
     pointer=definitions[loaded[2]]
     assert pointer[:2]==['OpAccessChain','%_ptr_UniformConstant_24']
     if pointer[2]=='%30': name='Rec'
     else:
      assert pointer[2]=='%27'
      index=definitions[pointer[3]]
      assert index[0]=='OpIAdd'
      name={'%uint_1':'Hist','%uint_2':'Out'}[index[-1]]
     assert definitions[coord][1]=='%v2uint'
     assert definitions[value][1]=='%v4float'
     counts[name]+=1
     if name not in active: return match[0]
     tag=f'%bb{name}{counts[name]}'
     return f'''{tag}X = OpCompositeExtract %uint {coord} 0
    {tag}Y = OpCompositeExtract %uint {coord} 1
    {tag}Lx = OpISub %uint {tag}X %bbBaseX
    {tag}Ly = OpISub %uint {tag}Y %bbBaseY
    {tag}Row = OpIMul %uint {tag}Ly %bbC32
    {tag}Index = OpIAdd %uint {tag}Row {tag}Lx
    {tag}Ptr = OpAccessChain %bbVecPtr %bb{name} {tag}Index
    OpStore {tag}Ptr {value}'''
    source=re.sub(r'OpImageWrite\s+(%\S+)\s+(%\S+)\s+(%\S+)',store,source)
    assert counts=={'Rec':4,'Hist':4,'Out':4},counts
    flush='''OpControlBarrier %bbC2 %bbC2 %bbC264
    %bbLocal = OpLoad %v3uint %gl_LocalInvocationID
    %bbLane = OpCompositeExtract %uint %bbLocal 0
    %bbWidth = OpIMul %uint %59 %bbC2
    %bbHeight = OpIMul %uint %61 %bbC2
    %bbRootPtr = OpAccessChain %_ptr_PushConstant_uint %registers %uint_1
    %bbRoot = OpLoad %uint %bbRootPtr
    '''
    for name,variable,offset in [('Rec','%30','%uint_6'),('Hist','%27','%uint_1'),('Out','%27','%uint_2')]:
     if name not in active: continue
     flush+=f'''%bb{name}Descriptor = OpIAdd %uint %bbRoot {offset}
    %bb{name}ImagePtr = OpAccessChain %_ptr_UniformConstant_24 {variable} %bb{name}Descriptor
    %bb{name}Image = OpLoad %24 %bb{name}ImagePtr
    '''
    for k,constant in enumerate(['%uint_0','%bbC256','%bbC512','%bbC768']):
     tag=f'%bbFlush{k}'
     flush+=f'''{tag}Index = OpIAdd %uint %bbLane {constant}
    {tag}Lx = OpBitwiseAnd %uint {tag}Index %bbC31
    {tag}Ly = OpShiftRightLogical %uint {tag}Index %bbC5
    {tag}X = OpIAdd %uint %bbBaseX {tag}Lx
    {tag}Y = OpIAdd %uint %bbBaseY {tag}Ly
    {tag}InX = OpULessThan %bool {tag}X %bbWidth
    {tag}InY = OpULessThan %bool {tag}Y %bbHeight
    {tag}Valid = OpLogicalAnd %bool {tag}InX {tag}InY
    OpSelectionMerge {tag}End None
    OpBranchConditional {tag}Valid {tag}Write {tag}End
    {tag}Write = OpLabel
    {tag}Coord = OpCompositeConstruct %v2uint {tag}X {tag}Y
    '''
     for name in ['Rec','Hist','Out']:
      if name not in active: continue
      flush+=f'''{tag}{name}Ptr = OpAccessChain %bbVecPtr %bb{name} {tag}Index
    {tag}{name}Value = OpLoad %v4float {tag}{name}Ptr
    OpImageWrite %bb{name}Image {tag}Coord {tag}{name}Value
    '''
     flush+=f'OpBranch {tag}End\n{tag}End = OpLabel\n'
    source=source.replace('               OpReturn',flush+'OpReturn',1)

    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--header', type=Path)
    args = parser.parse_args()
    original = args.original.read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        parser.error('unexpected shader revision; input must match ' + ORIGINAL_SHA256)
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        named, raw, modified = (directory/name for name in ('named.spvasm', 'raw.spvasm', 'modified.spvasm'))
        subprocess.run(['spirv-dis', str(args.original), '-o', str(named)], check=True)
        subprocess.run(['spirv-dis', '--raw-id', str(args.original), '-o', str(raw)], check=True)
        # Retain all original numeric IDs, including unnamed SSA values. New IDs
        # are allocated above the original bound; original arithmetic stays intact.
        names = {}
        for left, right in zip(named.read_text().splitlines(), raw.read_text().splitlines(), strict=True):
            a, b = re.findall(r'%[\w]+', left), re.findall(r'%[\w]+', right)
            if len(a) != len(b):
                raise ValueError('disassembler output mismatch')
            for x, y in zip(a, b):
                if x in names and names[x] != y:
                    raise ValueError('inconsistent numeric ID')
                names[x] = y
        source = transform(named.read_text())
        source = re.sub(r'%[\w]+', lambda match: names.get(match[0], match[0]), source)
        modified.write_text(source, encoding='utf-8')
        subprocess.run(['spirv-as', '--preserve-numeric-ids', '--target-env', 'spv1.3', str(modified), '-o', str(args.output)], check=True)
    before = list(struct.unpack('<%dI' % (len(original)//4), original))
    after = list(struct.unpack('<%dI' % (args.output.stat().st_size//4), args.output.read_bytes()))
    after[2] = before[2]  # Preserve the original generator metadata too.
    args.output.write_bytes(struct.pack('<%dI' % len(after), *after))
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(args.output)], check=True)
    if args.header:
        edits = [(a, b-a, after[c:d]) for op, a, b, c, d in difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes() if op != 'equal']
        if sum(len(data) for _, _, data in edits) > 1024:
            raise ValueError('unexpectedly large edit recipe')
        lines = ['// SPDX-License-Identifier: GPL-2.0-or-later',
                 '// Generated by tools/fsr411_post_lds.py. Only new instructions, no model weights.',
                 '// Original SHA-256: ' + ORIGINAL_SHA256,
                 '// Result SHA-256: ' + hashlib.sha256(args.output.read_bytes()).hexdigest(), '']
        for i, (_, _, data) in enumerate(edits):
            lines.append('constexpr std::array<uint32_t, %d> Edit%d{{' % (len(data), i))
            lines.extend('    ' + ', '.join('0x%08xu' % word for word in data[j:j+8]) + ',' for j in range(0, len(data), 8))
            lines.append('}};')
        lines.append('constexpr std::array Edits{')
        lines.extend('    Edit{%d, %d, Edit%d},' % (offset, count, i) for i, (offset, count, _) in enumerate(edits))
        lines.append('};')
        args.header.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('Validated optimized shader SHA-256: ' + hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
