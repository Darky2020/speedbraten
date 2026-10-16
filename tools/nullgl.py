#!/usr/bin/env python3
"""Generates src/engine/nullgl.cpp: do-nothing implementations of every GL function the engine
calls, for the headless re-simulation build (sauer_resim), which links no GL library.

Usage (from the repo root): tools/nullgl.py
Function names come from the engine sources (direct calls and getprocaddress("...") names);
signatures from the macOS SDK GL headers in src/macos/sdk_11.3. Functions whose results the
engine reads get hand-written bodies in BODIES below; everything else does nothing and
returns 0. Re-run after the engine starts using a new GL function (sauer_resim then fails
to link, or getprocaddress logs the missing name).
"""
import glob
import re
import sys

SRC = 'src'
HEADERS = ['gl.h', 'gl3.h', 'glext.h']
HEADERDIR = SRC + '/macos/sdk_11.3/System/Library/Frameworks/OpenGL.framework/Headers/'
OUT = SRC + '/engine/nullgl.cpp'
SUFFIXES = ('ARB', 'EXT', 'APPLE')

BODIES = {
    'glGetString': '''    switch(name)
    {
        case 0x1F00: return (const GLubyte *)"speedbraten";      // GL_VENDOR
        case 0x1F01: return (const GLubyte *)"null renderer";    // GL_RENDERER
        case 0x1F02: return (const GLubyte *)"3.0 null";         // GL_VERSION
        case 0x8B8C: return (const GLubyte *)"1.30";             // GL_SHADING_LANGUAGE_VERSION
        default: return (const GLubyte *)"";
    }''',
    'glGetStringi': '    return (const GLubyte *)"";',
    'glGetIntegerv': '''    switch(pname)
    {
        case 0x0BA2:                                    // GL_VIEWPORT
        case 0x0C10:                                    // GL_SCISSOR_BOX
            params[0] = params[1] = 0; params[2] = 640; params[3] = 400; return;
        case 0x0D3A: params[0] = params[1] = 4096; return;  // GL_MAX_VIEWPORT_DIMS
        case 0x0D33:                                    // GL_MAX_TEXTURE_SIZE
        case 0x84E8:                                    // GL_MAX_RENDERBUFFER_SIZE
        case 0x851C: params[0] = 4096; return;            // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x8872:                                    // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8869: params[0] = 16; return;              // GL_MAX_VERTEX_ATTRIBS
        case 0x8824: params[0] = 4; return;               // GL_MAX_DRAW_BUFFERS
        case 0x8B4B: params[0] = 64; return;              // GL_MAX_VARYING_COMPONENTS
        case 0x8B4A:                                    // GL_MAX_VERTEX_UNIFORM_COMPONENTS
        case 0x8B49: params[0] = 1024; return;            // GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
        default: params[0] = 0; return;
    }''',
    'glIsEnabled': '    return 0;',
    'glIsVertexArray': '    return 1;',
    'glCheckFramebufferStatus': '    return 0x8CD5; // GL_FRAMEBUFFER_COMPLETE',
    'glCreateShader': '    return nextid();',
    'glCreateProgram': '    return nextid();',
    'glGetShaderiv': '    *params = pname == 0x8B81 ? 1 : 0; // GL_COMPILE_STATUS',
    'glGetProgramiv': '    *params = pname == 0x8B82 || pname == 0x8B83 ? 1 : 0; // GL_LINK_STATUS, GL_VALIDATE_STATUS',
    'glGetShaderInfoLog': '    if(length) *length = 0;\n    if(bufSize > 0) infoLog[0] = 0;',
    'glGetProgramInfoLog': '    if(length) *length = 0;\n    if(bufSize > 0) infoLog[0] = 0;',
    'glGetUniformLocation': '    return GLint(nextid());',
    'glGetUniformBlockIndex': '    return 0xFFFFFFFFu; // GL_INVALID_INDEX',
    'glGetQueryObjectiv': '    *params = 1;',
    'glGetQueryObjectuiv': '    *params = 1;',
    'glGetQueryiv': '    *params = 0;',
    'glGetTexLevelParameteriv': '    *params = 0;',
    'glGetActiveUniform': '    if(length) *length = 0;\n    if(size) *size = 0;\n    if(type) *type = 0;\n    if(bufSize > 0) name[0] = 0;',
    'glMapBuffer': '    return mapbuffer(0);',
    'glMapBufferRange': '    return mapbuffer(size_t(length));',
    'glUnmapBuffer': '    return 1;',
    'glBufferData': '    growmap(size_t(size));',
}
GEN = {'glGenTextures', 'glGenBuffers', 'glGenQueries', 'glGenFramebuffers', 'glGenRenderbuffers',
       'glGenVertexArrays'}


def wanted():
    direct, proc = set(), set()
    files = [f for pat in ('engine/*.cpp', 'engine/*.h', 'shared/*.cpp', 'shared/*.h', 'fpsgame/*.cpp',
                           'fpsgame/*.h', 'p1xbraten/*.cpp') for f in glob.glob(SRC + '/' + pat)]
    for f in files:
        if f.endswith('nullgl.cpp'):
            continue
        text = open(f, errors='replace').read()
        direct.update(re.findall(r'\b(gl[A-Z][A-Za-z0-9]*)_?\s*\(', text))
        proc.update(re.findall(r'getprocaddress\("(gl[A-Za-z0-9]+)"\)', text))
    for line in open(SRC + '/shared/glexts.h'):
        m = re.match(r'#define (gl[A-Za-z0-9]+)_ (gl[A-Za-z0-9]+)', line)
        if m:
            direct.add(m.group(2))
    return direct, proc


def prototypes():
    protos = {}
    for h in HEADERS:
        for line in open(HEADERDIR + h, errors='replace'):
            m = re.match(r'\s*(?:extern|GLAPI)\s+(.+?)\s*(?:APIENTRY\s+)?\b(gl[A-Za-z0-9]+)\s*\(([^)]*)\)', line)
            if m and m.group(2) not in protos:
                ret = m.group(1).replace('APIENTRY', '').strip()
                protos[m.group(2)] = (ret, m.group(3).strip())
    return protos


def base(name):
    for s in SUFFIXES:
        if name.endswith(s):
            return name[:-len(s)]
    return name


def main():
    direct, proc = wanted()
    protos = prototypes()
    names = sorted({base(n) for n in direct | proc} | (direct & set(protos)))
    names = [n for n in names if n in protos]
    missing = sorted(n for n in {base(x) for x in direct | proc} if n not in protos)
    if missing:
        print('ignored (no prototype, not a GL function?):', ' '.join(missing), file=sys.stderr)
    out = [
        '// Generated by tools/nullgl.py - do not edit. GL functions that do nothing, for the',
        '// headless re-simulation build (sauer_resim). Calls arrive through the engine\'s',
        '// direct references and through getprocaddress().',
        '',
        '#ifdef HEADLESS',
        '',
        '#include <stddef.h>',
        '#include <stdlib.h>',
        '#include <string.h>',
        '',
        'typedef unsigned int GLenum; typedef unsigned int GLbitfield; typedef unsigned int GLuint;',
        'typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean; typedef signed char GLbyte;',
        'typedef short GLshort; typedef unsigned char GLubyte; typedef unsigned short GLushort;',
        'typedef float GLfloat; typedef float GLclampf; typedef double GLdouble; typedef double GLclampd;',
        'typedef void GLvoid; typedef char GLchar; typedef ptrdiff_t GLintptr; typedef ptrdiff_t GLsizeiptr;',
        'typedef ptrdiff_t GLintptrARB; typedef ptrdiff_t GLsizeiptrARB; typedef char GLcharARB;',
        '',
        'static GLuint lastid = 0;',
        'static GLuint nextid() { return ++lastid; }',
        '',
        '// glMapBuffer* hands out one scratch buffer, as large as the largest buffer created',
        'static void *mapmem = NULL;',
        'static size_t mapsize = 0;',
        'static void growmap(size_t size) { if(size > mapsize) { mapmem = realloc(mapmem, size); mapsize = size; } }',
        'static void *mapbuffer(size_t size) { growmap(size ? size : 1); return mapmem; }',
        '',
        'extern "C" {',
        '',
    ]
    for n in names:
        ret, args = protos[n]
        out.append('%s %s(%s)' % (ret, n, args))
        out.append('{')
        if n in BODIES:
            out.append(BODIES[n])
        elif n in GEN:
            argnames = [a.split()[-1].lstrip('*') for a in args.split(',')]
            out.append('    for(GLsizei i = 0; i < %s; i++) %s[i] = nextid();' % (argnames[0], argnames[1]))
        elif ret != 'void':
            out.append('    return 0;')
        out.append('}')
        out.append('')
    out.append('}')
    out.append('')
    out.append('static const struct { const char *name; void *fn; } nullglfuncs[] = {')
    for n in names:
        out.append('    { "%s", (void *)%s },' % (n, n))
    out.append('};')
    out.append('')
    out.append('''void *nullgl_getprocaddress(const char *name)
{
    size_t len = strlen(name);
    static const char *const suffixes[] = { "ARB", "EXT", "APPLE" };
    for(size_t s = 0; s < sizeof(suffixes)/sizeof(suffixes[0]); s++)
    {
        size_t slen = strlen(suffixes[s]);
        if(len > slen && !strcmp(name + len - slen, suffixes[s])) { len -= slen; break; }
    }
    for(size_t i = 0; i < sizeof(nullglfuncs)/sizeof(nullglfuncs[0]); i++)
        if(strlen(nullglfuncs[i].name) == len && !strncmp(nullglfuncs[i].name, name, len)) return nullglfuncs[i].fn;
    return NULL;
}

#endif''')
    open(OUT, 'w').write('\n'.join(out) + '\n')
    print('%s: %d functions' % (OUT, len(names)), file=sys.stderr)


if __name__ == '__main__':
    main()
