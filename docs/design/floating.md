# Floating point and SIMD in user space

MiniOS permits floating point instructions only in user space. Kernel C files
remain compiled with `-mno-sse -mno-sse2 -mno-mmx -mno-80387`; interrupt and
system call code cannot depend on floating point state or use it as scratch
storage.

## Architectural state

M23 enabled `CR4.OSFXSR` and `CR4.OSXMMEXCPT`, cleared the emulation and task
switched bits in `CR0`, and installed the default masked exception state with
MXCSR `0x1f80`. Every thread owns a 512-byte, 16-byte-aligned FXSAVE area.
Context switches, fork, exec and signal delivery preserve or initialize that
area as appropriate. It contains the x87 stack, MMX aliases, MXCSR and
XMM0-XMM15.

The user compiler flags state the corresponding contract explicitly:

- `-msse2 -mfpmath=sse` selects the x86_64 SSE2 baseline for `float` and
  `double` expressions.
- `-ftree-vectorize -fvect-cost-model=dynamic` permits loop and basic-block
  vectorization when GCC's cost model finds it useful.
- `-mno-avx` prevents instructions that write the upper halves of YMM
  registers. FXSAVE has no place for those bits.

AVX support requires an architectural-state change in the kernel: detect
XSAVE with CPUID, enable `CR4.OSXSAVE`, select x87/SSE/AVX in XCR0, size and
align each thread's save area from CPUID leaf `0xD`, and replace every FXSAVE
path—including signal frames—with XSAVE/XRSTOR. Enabling AVX in the compiler
before that work would allow one thread's YMM upper halves to leak into
another thread and would corrupt values across scheduling points.

## Math runtime

`libc/include/float.h` describes the compiler's binary32, binary64 and x87
extended formats, including true minima and decimal round-trip precision.
`libc/include/math.h` supplies classification and ordered-comparison macros,
common constants and these function families:

- `fabs`, `copysign`, `sqrt`, `trunc`, `floor`, `ceil` and `round`;
- `fmin` and `fmax`, including NaN and signed-zero handling;
- `frexp`, `ldexp`/`scalbn`, and `modf`;
- `fmod` and IEEE `remainder`;
- `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`, `log1p` and `pow`;
- `hypot`, `cbrt`, `sin`, `cos` and `tan`;
- `asin`, `acos`, `atan` and quadrant-preserving `atan2`;
- `sinh`, `cosh`, `tanh`, `asinh`, `acosh` and `atanh`;
- `float` and x87 `long double` variants of every function above.

Classification and most elementary operations use IEEE-754 bit layouts.
Square root uses `sqrtsd` or `sqrtss`. A negative finite square-root argument
returns NaN and sets `errno` to `EDOM`; range failures in scaling set
`ERANGE`.

The exponential kernel reduces its input by a split `ln(2)` and evaluates a
short Taylor series on the resulting interval before using `ldexp`. Logarithms
use `frexp`, move the mantissa near one, and evaluate the rapidly convergent
odd series for `2*atanh((m-1)/(m+1))`. `expm1` and `log1p` have separate small
argument series to avoid cancellation. Power composes the logarithm and
exponential kernels and handles negative bases only for integral exponents.
`hypot` scales by its larger operand, while `cbrt` uses a logarithmic initial
estimate followed by Newton refinement.

The long-double elementary kernels use binary80 arithmetic throughout. x87
`fsqrt`, `f2xm1`, `fyl2x`, `fyl2xp1`, `fpatan`, `fprem` and `fprem1` provide
the primitive square-root, exponential, logarithmic, inverse-tangent and
remainder operations. The remaining inverse trigonometric functions use
stable `atan2` identities. Hyperbolic functions use `expm1` near zero and
scaled exponential or logarithmic identities away from zero to avoid
cancellation and premature overflow.

Trigonometric reduction has two paths. Small inputs use a split pi/2 constant.
Larger inputs use Payne-Hanek reduction: the 64-bit input significand is
multiplied by a 16,512-bit fixed-point expansion of 2/pi, the nearest quotient
and its quadrant are extracted as integers, and the signed fractional quotient
is converted back to binary80 on the interval [-pi/4, pi/4]. The table covers
every finite binary80 exponent, so double and long-double arguments retain
their quadrant at large magnitude. Sine and cosine are evaluated on the
reduced interval with binary80 series kernels; tangent divides those kernels.
This avoids the host-dependent precision of emulated x87 transcendental
instructions.

## Floating environment

`libc/include/fenv.h` exposes the C exception flags, four rounding modes,
`fexcept_t`, `fenv_t`, `FE_DFL_ENV`, and the complete environment operation
set. The stored environment contains the 28-byte x87 image and MXCSR. Every
operation updates both units: exception queries merge their status flags,
rounding-mode changes write the x87 control word and MXCSR rounding field, and
hold/update operations preserve and merge pending flags. The default
environment is x87 control word `0x037f` and MXCSR `0x1f80`, with exceptions
masked and round-to-nearest-even selected. `FLT_ROUNDS` queries the active
control word and therefore follows changes made by `fesetround`.

`strtod`, `strtof` and `atof` accept decimal integer/fraction syntax, decimal
exponents, C hexadecimal significands with binary `p` exponents,
case-insensitive infinity, and NaN with an optional alphanumeric payload. The
decimal parser retains 18 significant digits; the hexadecimal parser retains
60 significand bits before narrowing. Both observe the standard no-conversion
`endptr` rule and set `ERANGE` on overflow or underflow to zero. Locale-specific
decimal separators are not supported.

The printf family handles `%f`, `%e`, `%g`, `%a` and their uppercase forms,
together with sign, alternate form, width, zero padding, left alignment and
precision. Hexadecimal formatting is bit based: it emits exact normal and
subnormal values, preserves binary80 payload precision for `%La`, and obeys
the active rounding direction when an explicit precision discards bits. `%g`
accepts up to 17 significant digits; fixed and scientific fractional precision
is capped at 16 digits. Decimal formatting remains a compact freestanding
conversion rather than a fully correctly-rounded decimal package.

## Explicit vector API

`libc/include/minios/simd.h` defines native 128-bit `simd_f32x4`,
`simd_f64x2`, signed/unsigned integer mask types. Inline constructors, splats,
unaligned loads and stores, arithmetic, absolute value, packed square root,
native minimum/maximum, clamps, horizontal sums and dot products allow user
programs to compose vector expressions directly. The public unaligned access
helpers use explicit `__builtin_memcpy` calls so the compiler can select
`movups` or `movupd` without violating C aliasing rules.

The libc also exports array operations for float and double addition,
multiplication, scaling and square root, float SAXPY, and float/double dot
products. Each routine processes complete vectors first and then a scalar
tail, so the input length and alignment have no restrictions. SIMD reductions
group additions differently from a scalar left-to-right loop and can therefore
differ by normal floating point rounding error. Native SSE minimum and maximum
return the second operand for equal or unordered lanes; their NaN and
signed-zero behavior intentionally follows the instruction rather than scalar
`fmin` and `fmax`. The SSE2 operations are in `bits/x86_64/simd.h`; the NEON
operations in `bits/aarch64/simd.h` select lanes with a comparison mask so
that minimum and maximum return the same lanes as on x86_64.

## Verification

`tests/cases/float` runs `/bin/floattest`. It checks classification, exceptional
values, rounding, subnormal and overflow behavior, decimal parsing, every new
printf conversion, unaligned vectors, scalar tails, SAXPY and reductions.
`tests/cases/fpu` remains the architectural-state test: concurrent work,
forking and a signal handler all modify floating registers while the test
checks that context restoration preserves results.

`tests/cases/mathvec` runs `/bin/mathvectest`. It checks exponential/logarithm
round trips across two hundred binary exponents, trigonometric identities,
power and cube-root composition, exact hexadecimal boundary parsing, and a
packed multiply/scale/square-root pipeline whose length requires a scalar
tail.

`tests/cases/libmfull` runs `/bin/libmfulltest`. It covers inverse
trigonometric and hyperbolic values and domains, every public long-double
family, binary80 precision, Payne-Hanek references at `1e300` and `2^16000`,
x87/MXCSR rounding and exception operations, and `%a`, `%A` and `%La` including
directed rounding and the smallest double subnormal.

The build additionally inspects `libc/src/stdlib/simd.o`; packed SSE2
instructions such as `addps`, `mulps`, `sqrtps`, `addpd`, `mulpd` and `sqrtpd`
must be present, while VEX/AVX instructions must be absent.
