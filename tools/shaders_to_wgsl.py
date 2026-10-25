#!/usr/bin/env python3
"""
shaders_to_wgsl.py -- converts renderer/shaders/*.vert|frag|comp (Vulkan GLSL)
to WGSL for the WebGPU renderer, and writes a C header with the WGSL sources
and the bindings each shader uses.

Steps per shader:
  1. inline #include "..." files
  2. push constants -> uniform buffer at set 1, binding 0 (WebGPU has no push
     constants; the scalars/vec2/vec4 members used here have the same offsets
     under std140 and std430)
  3. combined samplers (sampler2D, usampler2D, sampler3D, sampler2DArray) ->
     separate texture (set 0, same binding) + sampler (set 2, same binding);
     every use of the name becomes samplerXX(name_t, name_s)
  4. glslangValidator -> SPIR-V, naga -> WGSL (naga also validates)
  5. reflect the @group/@binding declarations from the WGSL

Shaders listed in SKIP are converted by hand (the ocean FFT needs read-write
rgba32float storage, which core WebGPU does not have).

Usage: python tools/shaders_to_wgsl.py <out_dir> <header.h>
"""
import os, re, subprocess, sys, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'renderer', 'shaders')
SKIP = {'ocean_spectrum.comp', 'ocean_fft.comp', 'ocean_finalize.comp'}
STAGE = {'.vert': 'vert', '.frag': 'frag', '.comp': 'comp'}

SAMPLER_DECL = re.compile(
    r'layout\(\s*set\s*=\s*0\s*,\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+'
    r'(u?)sampler(2D|3D|2DArray)\s+(\w+)\s*;')


def inline_includes(text, seen=()):
    def rep(m):
        name = m.group(1)
        if name in seen:
            return ''
        with open(os.path.join(SRC, name)) as f:
            return inline_includes(f.read(), seen + (name,))
    return re.sub(r'^\s*#include\s+"([^"]+)"\s*$', rep, text, flags=re.M)


def rewrite(text):
    # naga's SPIR-V reader mistypes the scalar in "ivec2 * int" (reads it as a
    # 64-bit integer vector); spelling the vector out is identical in meaning.
    text = re.sub(r'(ivec2\([^;]*?\))\s*\*\s*pc\.scale\b', r'\1 * ivec2(pc.scale)', text)
    text = re.sub(r'layout\(\s*push_constant\s*\)\s*uniform',
                  'layout(set = 1, binding = 0, std140) uniform', text)
    names = []
    def decl(m):
        binding, u, dim, name = m.groups()
        names.append((name, u, dim))
        return (f'layout(set = 0, binding = {binding}) uniform {u}texture{dim} {name}_t;\n'
                f'layout(set = 2, binding = {binding}) uniform sampler {name}_s;')
    text = SAMPLER_DECL.sub(decl, text)
    for name, u, dim in names:
        text = re.sub(rf'(?<![\w.]){name}(?!_t\b|_s\b)(?=\W)',
                      f'{u}sampler{dim}({name}_t, {name}_s)', text)
    return text


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout


def reflect(wgsl):
    out = []
    for m in re.finditer(r'@group\((\d+)\)\s*@binding\((\d+)\)\s*var(<[^>]*>)?\s+(\w+)\s*:\s*([^;]+);', wgsl):
        g, b, space, name, ty = m.groups()
        out.append({'group': int(g), 'binding': int(b), 'name': name,
                    'space': (space or '').strip('<>'), 'type': ty.strip()})
    return out


def c_string(s):
    return '\n'.join('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"'
                     for line in s.split('\n'))


def main():
    out_dir, header = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    naga = os.environ.get('NAGA', 'naga')
    done, failed, manifest = [], [], {}
    for fn in sorted(os.listdir(SRC)):
        base, ext = os.path.splitext(fn)
        if ext not in STAGE or fn in SKIP:
            continue
        with open(os.path.join(SRC, fn)) as f:
            glsl = rewrite(inline_includes(f.read()))
        g = os.path.join(out_dir, fn)
        with open(g, 'w') as f:
            f.write(glsl)
        spv = g + '.spv'
        wgsl_path = os.path.join(out_dir, base + '_' + STAGE[ext] + '.wgsl')
        try:
            run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-S', STAGE[ext], '-o', spv, g])
            try:
                run([naga, spv, wgsl_path])
            except RuntimeError:
                # naga's SPIR-V reader occasionally mistypes a valid shader
                # (hw_prim.frag); its GLSL reader takes the same source fine.
                run([naga, '--input-kind', 'glsl', '--shader-stage', STAGE[ext], g, wgsl_path])
                print(f'note: {fn} converted through naga\'s GLSL reader')
            wgsl = open(wgsl_path).read()
            if STAGE[ext] == 'frag':
                # GLSL allows implicit-derivative sampling in non-uniform branches
                # (hw_cloud_resolve, hw_post); WGSL rejects it unless told to
                # accept it. Same behaviour as the native renderer.
                wgsl = 'diagnostic(off, derivative_uniformity);\n' + wgsl
                with open(wgsl_path, 'w') as f:
                    f.write(wgsl)
            manifest[fn] = reflect(wgsl)
            done.append((fn, base + '_' + STAGE[ext], wgsl))
        except RuntimeError as e:
            failed.append((fn, str(e).strip().splitlines()[-6:]))
    with open(header, 'w') as h:
        h.write('/* generated by tools/shaders_to_wgsl.py -- do not edit */\n#pragma once\n\n')
        for fn, sym, wgsl in done:
            h.write(f'/* {fn} */\nstatic const char wgsl_{sym}[] =\n{c_string(wgsl)};\n\n')
    with open(os.path.join(out_dir, 'bindings.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print(f'converted {len(done)}, failed {len(failed)}, hand-written: {", ".join(sorted(SKIP))}')
    for fn, msg in failed:
        print(f'FAILED {fn}:\n    ' + '\n    '.join(msg))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
