#include <math.h>
#include <errno.h>
#include <stdint.h>
#include "../ldouble.h"

#define TWO_OVER_PI_BITS 16512
#define TWO_OVER_PI_LIMBS (TWO_OVER_PI_BITS / 64)
#define PRODUCT_LIMBS (TWO_OVER_PI_LIMBS + 1)

/* floor((2/pi) * 2^16512), most significant hexadecimal digit first. The
 * extra precision covers the complete exponent range of x87 binary80. */
static const char two_over_pi_hex[] =
    "a2f9836e4e441529fc2757d1f534ddc0db6295993c439041fe5163abdebbc561b7246e3a424dd2e006492eea09d1921c"
    "fe1deb1cb129a73ee88235f52ebb4484e99c7026b45f7e413991d639835339f49c845f8bbdf9283b1ff897ffde05980f"
    "ef2f118b5a0a6d1f6d367ecf27cb09b74f463f669e5fea2d7527bac7ebe5f17b3d0739f78a5292ea6bfb5fb11f8d5d08"
    "56033046fc7b6babf0cfbc209af4361da9e391615ee61b086599855f14a068408dffd8804d73273106061556ca73a8c9"
    "60e27bc08c6b47c419c367cddce8092a8359c4768b961ca6ddaf44d15719053ea5ff07053f7e33e832c2de4f98327dbb"
    "c33d26ef6b1e5ef89f3a1f35caf27f1d87f121907c7c246afa6ed5772d30433b15c614b59d19c3c2c4ad414d2c5d000c"
    "467d862d71e39ac69b0062337cd2b497a7b4d55537f63ed71810a3fc764d2a9d64abd770f87c6357b07ae715175649c0"
    "d9d63b3884a7cb2324778ad623545ab91f001b0af1dfce19ff319f6a1e6661579947fbacd87f7eb7652289e83260bfe6"
    "cdc4ef09366cd43f5dd7de16de3b58929bde2822d2e886284d58e232cac616e308cb7de050c017a71df35be01834132e"
    "6212830148835b8ef57fb0adf2e91e434a48d36710d8ddaa425faece616aa4280ab499d3f2a6067f775c83c2a3883c61"
    "78738a5a8cafbdd76f63a62dcbbff4ef818d67c12645ca5536d9cad2a8288d61c277c9121426049b4612c459c444c5c8"
    "91b24df31700ad43d4e5492910d5fdfcbe00cc941eeece70f53e1380f1ecc3e7b328f8c79405933e71c1b3092ef3450b"
    "9c12887b20ab9fb52ec292472f327b6d550c90a7721fe76b96cb314a1679e2794189dff49794e884e6e29731996bed88"
    "365f5f0efdbbb49a486ca467427271325d8db8159f09e5bc25318d3974f71c0530010c0d68084b58ee2c90aa4702e774"
    "24d6bda67df772486eef169fa6948ef691b45153d1f20acf3398207e4bf56863b25f3edd035d407f8985295255c06437"
    "10d86d324832754c5bd4714e6e5445c1090b69f52ad566149d072750045ddb3bb4c576ea17f9877d6b49ba271d296996"
    "acccc65414ad6ae29089d98850722cbea4049407777030f327fc00a871ea49c2663de06483dd97973fa3fd94438c860d"
    "de41319d39928c70dde7b7173bdf082b3715a0805c93805a921110d8e80faf806c4bffdb0f903876185915a562bbcb61"
    "b989c7bd401004f2d2277549f6b6ebbb22dbaa140a2f2689768364333b091a940eaa3a51c2a31daeedaf12265c4dc26d"
    "9c7a2d9756c0833f03f6f0098c402b99316d07b43915200c5bc3d8c492f54badc6a5ca4ecd37a736a9e69492ab6842dd"
    "de6319ef8c76528b6837dbfcaba1ae3115dfa1ae00dafb0c664d64b705ed306529bf56573aff47b9f96af3be75df9328"
    "3080abf68c6615cb040622fa1de4d9a4b33d8f1b5709cd36e9424ea4be13b523331aaaf0a8654fa5c1d20f3f0bcd785b"
    "76f923048b7b72178953a6c6e26e6f00ebef584a9bb7dac4ba66aacfcf761d02d12df1b1c1998c77adc3da4886a05df7"
    "f480c62ff0ac9aecddbc5c3f6dded01fc790b6db2a3a25a39aaf009353ad0457b6b42d297e804ba707da0eaa76a1597b"
    "2a12162db7dcfde5fafedb89fdbe896c76e4fca90670803e156e85ff87fd073e2833676186182aeabd4dafe7b36e6d8f"
    "3967955bbf3148d78416df30432dc7356125ce70c9b8cb30fd6cbfa200a4e46c05a0dd5a476f21d21262845cb9496170"
    "e0566b015299375550b7d51ec4f1335f6e13e4305da92e85c3b21d3632a1a4b708d4b1ea21f716e4698f77ff2780030c"
    "2d408da0cd4f99a520d3a2b30a5d2f42f9b4cbda11d0be7dc1db9bbd17ab81a2ca5c6a0817552e550027f0147f8607e1"
    "640b148d4196debe872afddab6256b34897bfef3059ebfb94f6a68a82a4a5ac44fbcf82d985ad795c7f48d4d0da63a20"
    "5f57a4b13f149538800120cc86dd71b6dec9f560bf11654d6b0701acb08cd0c0b24855510efb1ec372953b06a33540c0"
    "7bdc06cc45e0fa294ec8cad641f3e8de647cd8649b31bed9c397a4d45877c5e36913daf03c3aba4618465f7555f5bdd2"
    "c6926e5d2eaced440e423e1c87c461e9fd29f3d6e7ca7c2235916fc5e0088dd7ffe26a6ec6fdb0c10893745d7cb2ad6b"
    "9d6ecd7b723e6a11c6a9cff7df7329bac9b55100b70db2e224ba74607de58ad8742c150d0c188194667e162901767a9f"
    "befdfdef4556367ed913d9ecb9ba8bfc97c427a831c36ef136c59456a8d8b5a8b40ecccf2d891234576f89562ce3ce99"
    "b920d6aa5e6b9c2a3ecc5f114a0bfdfbf4e16d3b8e2c86e284d4e9a9b4fcd1eeefc9352e61392f442138c8d91b0afc81"
    "6a4afbd81c2f84b4538c994ecc2254dc552ad6c6c096190bb8701a649569605a26ee523f0f117f11b5f4f5cbfc2dbc34"
    "eebc34cc5de8605edd9b8e67ef3392b817c99b5861bc57e1c68351103ed84871dddd1c2da118af462c21d7f359987ad9"
    "c0549efa864ffc0656ae79e536228922ad38dc9367aae8553826829be7caa40d51b133990ed7a9480569f0b265a7887f"
    "974c8836d1f9b392214a827b21cf98dc9f405547dc3a74e142eb67df9dfe5fd45ea4677b7aacbaa2f65523882b55ba41"
    "086e59862a21834739e6e389d49ee540fb49e956ffca0f1c8a59c52bfa94c5c1d3cfc50fae5adb86c5476243853b8621"
    "94792c8761107b4c2a1a2c8012bf43902688893c78e4c4a87bdbe5c23ac4eaf4268a67f7bf920d2ba365b1933d0b7cbd"
    "dc51a463dd27dde16919949a9529a828ce68b4ed09209f44ca984e638270237c7e32b90f8ef5a7e7561408f1212a9db5"
    "4d7e6f5119a5abf9b5d6df8261dd960236169f3ac4a1a2836ded727a8d39a9b8825c326b5b2746ed34007700d255f4fc";


static unsigned hex_value(char c)
{
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
}

static uint64_t constant_limb(int index)
{
    int position = TWO_OVER_PI_BITS / 4 - (index + 1) * 16;
    uint64_t value = 0;
    for (int i = 0; i < 16; i++)
        value = value << 4 | hex_value(two_over_pi_hex[position + i]);
    return value;
}

static unsigned product_bit(const uint64_t product[PRODUCT_LIMBS], int bit)
{
    if (bit < 0 || bit >= PRODUCT_LIMBS * 64)
        return 0;
    return (unsigned)(product[bit / 64] >> (bit % 64) & 1U);
}

static unsigned product_bits2(const uint64_t product[PRODUCT_LIMBS], int bit)
{
    return product_bit(product, bit) | product_bit(product, bit + 1) << 1;
}

static int lower_product_nonzero(const uint64_t product[PRODUCT_LIMBS], int limit)
{
    int complete = limit / 64;
    for (int i = 0; i < complete; i++)
        if (product[i])
            return 1;
    int partial = limit % 64;
    return partial && (product[complete] & ((1ULL << partial) - 1ULL));
}

/* Return r and n such that x = n*pi/2 + r, |r| <= pi/4. Only n modulo four
 * is required by the trigonometric reconstruction. */
static long double reduce_pio2(long double x, int *quadrant)
{
    /* The arguments come from double values, so the leading 64
     * significand bits are the whole significand on both formats. */
    struct ld_parts bits = ld_split(x);
    int negative = (int)bits.negative;
    int exponent = (int)bits.raw_exponent - 16383;
    uint64_t significand = bits.significand;

    uint64_t product[PRODUCT_LIMBS];
    __uint128_t carry = 0;
    for (int i = 0; i < TWO_OVER_PI_LIMBS; i++) {
        __uint128_t value = (__uint128_t)constant_limb(i) * significand + carry;
        product[i] = (uint64_t)value;
        carry = value >> 64;
    }
    product[TWO_OVER_PI_LIMBS] = (uint64_t)carry;

    int shift = TWO_OVER_PI_BITS + 63 - exponent;
    unsigned floor_quadrant = product_bits2(product, shift);
    int half = (int)product_bit(product, shift - 1);
    int lower = lower_product_nonzero(product, shift - 1);
    int round_up = half && (lower || (floor_quadrant & 1U));
    unsigned reduced_quadrant = (floor_quadrant + (unsigned)round_up) & 3U;

    int words = (shift + 63) / 64;
    uint64_t remainder[PRODUCT_LIMBS];
    for (int i = 0; i < words; i++)
        remainder[i] = product[i];
    int top_bits = shift % 64;
    if (top_bits)
        remainder[words - 1] &= (1ULL << top_bits) - 1ULL;

    if (round_up) {
        uint64_t add = 1;
        for (int i = 0; i < words; i++) {
            uint64_t inverted = ~remainder[i];
            uint64_t value = inverted + add;
            add = value < inverted;
            remainder[i] = value;
        }
        if (top_bits)
            remainder[words - 1] &= (1ULL << top_bits) - 1ULL;
    }

    int highest = words - 1;
    while (highest >= 0 && remainder[highest] == 0)
        highest--;
    long double fraction = 0.0L;
    int lowest = highest > 3 ? highest - 3 : 0;
    for (int i = highest; i >= lowest; i--)
        fraction += ldexpl((long double)remainder[i], i * 64 - shift);
    if (round_up)
        fraction = -fraction;

    long double reduced = fraction * M_PI_2L;
    if (negative) {
        reduced = -reduced;
        reduced_quadrant = (0U - reduced_quadrant) & 3U;
    }
    *quadrant = (int)reduced_quadrant;
    return reduced;
}

static long double kernel_sin(long double x)
{
    long double square = x * x;
    long double term = x;
    long double sum = x;
    for (int n = 1; n <= 13; n++) {
        term *= -square / ((long double)(2 * n) * (long double)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static long double kernel_cos(long double x)
{
    long double square = x * x;
    long double term = 1.0L;
    long double sum = 1.0L;
    for (int n = 1; n <= 13; n++) {
        term *= -square / ((long double)(2 * n - 1) * (long double)(2 * n));
        sum += term;
    }
    return sum;
}

static long double reduce_small(long double x, int *quadrant)
{
    static const long double pio2_high = 0x1.921fb54442d18p+0L;
    static const long double pio2_low = 6.12323399573676603587e-17L;
    long double integral = roundl(x * M_2_PIL);
    long long multiple = (long long)integral;
    *quadrant = (int)(multiple & 3LL);
    return (x - integral * pio2_high) - integral * pio2_low;
}

long double sinl(long double x)
{
    if (isnan(x))
        return x;
    if (x == 0.0L)
        return x;
    if (isinf(x)) {
        errno = EDOM;
        return NAN;
    }
    int quadrant;
    long double reduced = fabsl(x) < 64.0L ? reduce_small(x, &quadrant) :
                                             reduce_pio2(x, &quadrant);
    switch (quadrant) {
    case 0: return kernel_sin(reduced);
    case 1: return kernel_cos(reduced);
    case 2: return -kernel_sin(reduced);
    default: return -kernel_cos(reduced);
    }
}

long double cosl(long double x)
{
    if (isnan(x))
        return x;
    if (isinf(x)) {
        errno = EDOM;
        return NAN;
    }
    int quadrant;
    long double reduced = fabsl(x) < 64.0L ? reduce_small(x, &quadrant) :
                                             reduce_pio2(x, &quadrant);
    switch (quadrant) {
    case 0: return kernel_cos(reduced);
    case 1: return -kernel_sin(reduced);
    case 2: return -kernel_cos(reduced);
    default: return kernel_sin(reduced);
    }
}

long double tanl(long double x)
{
    if (isnan(x))
        return x;
    if (x == 0.0L)
        return x;
    if (isinf(x)) {
        errno = EDOM;
        return NAN;
    }
    int quadrant;
    long double reduced = fabsl(x) < 64.0L ? reduce_small(x, &quadrant) :
                                             reduce_pio2(x, &quadrant);
    long double tangent = kernel_sin(reduced) / kernel_cos(reduced);
    return quadrant & 1 ? -1.0L / tangent : tangent;
}

double sin(double x) { return (double)sinl((long double)x); }
float sinf(float x) { return (float)sinl((long double)x); }
double cos(double x) { return (double)cosl((long double)x); }
float cosf(float x) { return (float)cosl((long double)x); }
double tan(double x) { return (double)tanl((long double)x); }
float tanf(float x) { return (float)tanl((long double)x); }
