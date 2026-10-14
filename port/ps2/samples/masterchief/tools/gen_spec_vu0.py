#!/usr/bin/env python3
"""Writes src/spec_vu0.h: the specular colour of the Chief's and the rifle's vertices on VU0 (macro mode, COP2 inline assembly).

What specular_rgbaq (main.c) works out for a vertex, in three steps, a group of three vertices at a time (VU0 stalls on
a dependent instruction, so one vertex alone would be as slow as the FPU; three independent ones keep its pipeline full):

  A  (VU0) v = camera - position, normalised; n.v; the mirror direction 2 (n.v) n - v; 1 - n.v; the half vector
     v + L0, normalised; n.h ^ 18.   Out: the mirror direction, and (1 - n.v, n.h^18).
  .  (EE)  the reflection's colour: the armour's cube map in the mirror direction (64x64x6 bytes: not for VU0's 4 KB).
  C  (VU0) 1.14 (0.40 cube (0.5 + fres) + 1.1 spec tint) + 0.25 lit sum, to the gamma curve and minus the same curve of
     the sum's quarter, which is a table (ge) the program keeps per vertex, times 128. The curve x^(1/2.2) is a cubic
     in sqrt (x), accurate to 0.21 of the 128 levels over [0.03, 3] (the EE's table is not better).
     Out: three integers, the colour bytes.

Run from here:  python3 tools/gen_spec_vu0.py > src/spec_vu0.h
"""
import sys

LANES = 3
# the polynomial: g(x) = x^(1/2.2) ~ c0 + c1 t + c2 t^2 + c3 t^3, t = sqrt (x), least squares on [0.03, 3]
POLY = (0.02131, 1.07821, -0.12567, 0.02647)

out = []
def emit(s): out.append(s)

class Asm:
    """collects COP2 instructions and the C operands they use; lanes are named by their registers"""
    def __init__(self, nin):
        self.lines = []
        self.nin = nin
    def i(self, text): self.lines.append(text)
    def lanes(self, fmt, **kw):
        for k in range(LANES): self.i(fmt.format(k=k, **kw))

def regs_a(k):
    b = 1 + 8 * k
    return dict(P=b, N=b + 1, T=b + 2, R=b + 3, H=b + 4, T3=b + 5, T5=b + 6, O2=b + 7)

def gen_a():
    CAM, L0, ONE = 25, 26, 27
    a = Asm(0)
    # operands: %0..%2 the positions, %3..%5 the normals, %6..%8 the mirror directions out, %9..%11 (1 - n.v, n.h^18) out
    def f(fmt):
        for k in range(LANES):
            r = regs_a(k)
            a.i(fmt.format(k=k, CAM=CAM, L0=L0, ONE=ONE, pp=k, nn=3 + k, ro=6 + k, fo=9 + k, **r))
    f("lqc2 $vf{P}, 0(%{pp})")
    f("lqc2 $vf{N}, 0(%{nn})")
    f("vsub.xyz $vf{P}, $vf{CAM}, $vf{P}")                       # v = camera - position
    f("vmul.xyz $vf{T}, $vf{P}, $vf{P}")
    f("vaddy.x $vf{T}, $vf{T}, $vf{T}")
    f("vaddz.x $vf{T}, $vf{T}, $vf{T}")                          # T.x = |v|^2
    # normalise v, the reciprocal square roots one after the other (one divide unit), the next lane's work in their waits
    r0, r1, r2 = regs_a(0), regs_a(1), regs_a(2)
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r0['T'])
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r0['P'], r0['P']))
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r1['T'])
    for r in (r0,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T'], r['P'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T']))
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r1['P'], r1['P']))
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r2['T'])
    for r in (r1,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T'], r['P'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T']))
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r2['P'], r2['P']))
    for r in (r2,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T'], r['P'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T'], r['T'], r['T']))
    f("vmax.x $vf{T}, $vf{T}, $vf0")                             # n.v, not below 0 (vf0's x is 0)
    f("vadd.x $vf{R}, $vf{T}, $vf{T}")                           # R.x = 2 n.v
    f("vmulx.xyz $vf{R}, $vf{N}, $vf{R}x")                       # n * 2 n.v
    f("vsub.xyz $vf{R}, $vf{R}, $vf{P}")                         # the mirror direction
    f("vsub.x $vf{O2}, $vf{ONE}, $vf{T}")                        # 1 - n.v
    f("vadd.xyz $vf{H}, $vf{P}, $vf{L0}")                        # the half vector with the key light
    f("vmul.xyz $vf{T3}, $vf{H}, $vf{H}")
    f("vaddy.x $vf{T3}, $vf{T3}, $vf{T3}")
    f("vaddz.x $vf{T3}, $vf{T3}, $vf{T3}")
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r0['T3'])
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r0['H'], r0['H']))
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r1['T3'])
    for r in (r0,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T3'], r['H'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3']))
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r1['H'], r1['H']))
    a.i("vrsqrt $Q, $vf0w, $vf%dx" % r2['T3'])
    for r in (r1,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T3'], r['H'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3']))
    a.i("vwaitq")
    a.i("vmulq.xyz $vf%d, $vf%d, $Q" % (r2['H'], r2['H']))
    for r in (r2,):
        a.i("vmul.xyz $vf%d, $vf%d, $vf%d" % (r['T3'], r['H'], r['N'])); a.i("vaddy.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3'])); a.i("vaddz.x $vf%d, $vf%d, $vf%d" % (r['T3'], r['T3'], r['T3']))
    f("vmax.x $vf{T3}, $vf{T3}, $vf0")                           # n.h, not below 0
    f("vmul.x $vf{T5}, $vf{T3}, $vf{T3}")                        # ^2
    f("vmul.x $vf{T3}, $vf{T5}, $vf{T5}")                        # ^4
    f("vmul.x $vf{T3}, $vf{T3}, $vf{T3}")                        # ^8
    f("vmul.x $vf{T3}, $vf{T3}, $vf{T3}")                        # ^16
    f("vmul.x $vf{T5}, $vf{T3}, $vf{T5}")                        # ^18
    f("vaddx.y $vf{O2}, $vf0, $vf{T5}")                          # O2.y = n.h^18 (vf0's y is 0)
    f("sqc2 $vf{R}, 0(%{ro})")
    f("sqc2 $vf{O2}, 0(%{fo})")
    return a.lines

def regs_c(k):
    b = 1 + 6 * k
    return dict(CU=b, O2=b + 1, SM=b + 2, GE=b + 3, K=b + 4, X=b + 5)

def gen_c():
    CK, TINT, POLYR, CM = 25, 26, 27, 28
    a = Asm(0)
    # operands: %0..%2 the cube colours, %3..%5 (1 - n.v, n.h^18), %6..%8 the lit sums, %9..%11 the gamma of their quarters, %12..%14 out
    def f(fmt):
        for k in range(LANES):
            r = regs_c(k)
            a.i(fmt.format(k=k, CK=CK, TINT=TINT, PL=POLYR, CM=CM, cu=k, fs=3 + k, sm=6 + k, ge=9 + k, oo=12 + k, **r))
    f("lqc2 $vf{CU}, 0(%{cu})")
    f("lqc2 $vf{O2}, 0(%{fs})")
    f("lqc2 $vf{SM}, 0(%{sm})")
    f("lqc2 $vf{GE}, 0(%{ge})")
    f("vmulx.x $vf{K}, $vf{CK}, $vf{O2}x")                       # K.x = 0.456 (1 - n.v)
    f("vmuly.z $vf{K}, $vf{CK}, $vf{O2}y")                       # K.z = 1.254 n.h^18
    f("vaddy.x $vf{K}, $vf{K}, $vf{CK}y")                        # K.x += 0.228: 1.14 * 0.40 * (0.5 + fres)
    f("vmulx.xyz $vf{CU}, $vf{CU}, $vf{K}x")                     # cube * that
    f("vmulz.xyz $vf{X}, $vf{TINT}, $vf{K}z")                    # tint * 1.254 spec (1.14 * 1.1)
    f("vadd.xyz $vf{CU}, $vf{CU}, $vf{X}")                       # what the reflection and highlight add
    f("vmulw.xyz $vf{SM}, $vf{SM}, $vf{CK}w")                    # a quarter of the lit sum: a mid-tone armour pixel
    f("vadd.xyz $vf{SM}, $vf{SM}, $vf{CU}")                      # the argument of the gamma curve
    # t = sqrt (argument), a channel at a time
    for k in range(LANES):
        r = regs_c(k)
        for ch in "xyz":
            a.i("vsqrt $Q, $vf%d%s" % (r['SM'], ch))
            a.i("vwaitq")
            a.i("vaddq.%s $vf%d, $vf0, $Q" % (ch, r['X']))      # X.channel = 0 + sqrt (vf0's x, y, z are 0)
    f("vmulw.xyz $vf{CU}, $vf{X}, $vf{PL}w")                     # c3 t
    f("vaddz.xyz $vf{CU}, $vf{CU}, $vf{PL}z")                    # + c2
    f("vmul.xyz $vf{CU}, $vf{CU}, $vf{X}")
    f("vaddy.xyz $vf{CU}, $vf{CU}, $vf{PL}y")                    # + c1
    f("vmul.xyz $vf{CU}, $vf{CU}, $vf{X}")
    f("vaddx.xyz $vf{CU}, $vf{CU}, $vf{PL}x")                    # + c0: the gamma curve of the argument
    f("vsub.xyz $vf{CU}, $vf{CU}, $vf{GE}")                      # less that of the quarter sum
    f("vmulx.xyz $vf{CU}, $vf{CU}, $vf{CM}x")                    # the GS reads 128 as 1
    f("vmax.xyz $vf{CU}, $vf{CU}, $vf0")
    f("vftoi0.xyz $vf{CU}, $vf{CU}")
    f("sqc2 $vf{CU}, 0(%{oo})")
    return a.lines

def c_function(name, lines, nops, doc, arg_decl):
    emit("/* %s */" % doc)
    emit("static inline void %s(%s)" % (name, arg_decl))
    emit("{")
    emit("\t__asm__ __volatile__(")
    for l in lines:
        emit('\t\t"%s\\n"' % l)
    emit("\t\t: : %s : \"memory\");" % ", ".join('"r"(%s)' % x for x in OPERANDS[name]))
    emit("}")
    emit("")

OPERANDS = {
    "spec_vu0_a": ["p0", "p1", "p2", "n0", "n1", "n2", "r0", "r1", "r2", "f0", "f1", "f2"],
    "spec_vu0_c": ["c0", "c1", "c2", "f0", "f1", "f2", "s0", "s1", "s2", "g0", "g1", "g2", "o0", "o1", "o2"],
}

emit("/* generated by tools/gen_spec_vu0.py: the Chief's and the rifle's specular colours on VU0 (see there); do not edit */")
emit("")
emit("/* the constants stay in vf25 to vf28 (vf1 to vf24 are the three lanes') between the calls of one stage */")
emit("static inline void spec_vu0_load_a(const qvec *consts)       /* camera (w 0), the key light, one */")
emit("{")
emit('\t__asm__ __volatile__("lqc2 $vf25, 0(%0)\\n" "lqc2 $vf26, 16(%0)\\n" "lqc2 $vf27, 32(%0)\\n" : : "r"(consts) : "memory");')
emit("}")
emit("")
emit("static inline void spec_vu0_load_c(const qvec *consts)       /* (0.456, 0.228, 1.254, 0.25), the highlight's tint, the cubic's c0 to c3, (128, 0, 0, 0) */")
emit("{")
emit('\t__asm__ __volatile__("lqc2 $vf25, 0(%0)\\n" "lqc2 $vf26, 16(%0)\\n" "lqc2 $vf27, 32(%0)\\n" "lqc2 $vf28, 48(%0)\\n" : : "r"(consts) : "memory");')
emit("}")
emit("")
c_function("spec_vu0_a", gen_a(), 12, "stage A for three vertices: the mirror direction (xyz) and (1 - n.v, n.h^18) of each",
           "const qvec *p0, const qvec *p1, const qvec *p2, const qvec *n0, const qvec *n1, const qvec *n2, qvec *r0, qvec *r1, qvec *r2, qvec *f0, qvec *f1, qvec *f2")
c_function("spec_vu0_c", gen_c(), 15, "stage C for three vertices: the colour bytes (xyz, as integers) from the cube colour, (1 - n.v, n.h^18), the lit sum and the gamma of its quarter",
           "const qvec *c0, const qvec *c1, const qvec *c2, const qvec *f0, const qvec *f1, const qvec *f2, const qvec *s0, const qvec *s1, const qvec *s2, const qvec *g0, const qvec *g1, const qvec *g2, qvec *o0, qvec *o1, qvec *o2")
emit("#define SPEC_VU0_POLY { %s }" % ", ".join("%rf" % c for c in POLY))
sys.stdout.write("\n".join(out) + "\n")
