
/* from valgrind tests */

/* ================ sha1.c ================ */
/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain

Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/

/* #define LITTLE_ENDIAN * This should be #define'd already, if true. */
/* #define SHA1HANDSOFF * Copies data before messing with it. */

#ifdef __UCE_WASM_CORE__
#include <stdint.h>
typedef uint32_t u_int32_t;
#endif

typedef struct {
    u_int32_t state[5];
    u_int32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Transform(u_int32_t state[5], const unsigned char buffer[64]);
void SHA1Init(SHA1_CTX* context);
void SHA1Update(SHA1_CTX* context, const unsigned char* data, u_int32_t len);
void SHA1Final(unsigned char digest[20], SHA1_CTX* context);

#define SHA1HANDSOFF

#include <stdio.h>
#include <string.h>
#include <sys/types.h>	/* for u_int*_t */
#include "hash.h"

#ifndef BYTE_ORDER
#if (BSD >= 199103)
# include <machine/endian.h>
#else
#if defined(linux) || defined(__linux__)
# include <endian.h>
#else
#define	LITTLE_ENDIAN	1234	/* least-significant byte first (vax, pc) */
#define	BIG_ENDIAN	4321	/* most-significant byte first (IBM, net) */
#define	PDP_ENDIAN	3412	/* LSB first in word, MSW first in long (pdp)*/

#if defined(vax) || defined(ns32000) || defined(sun386) || defined(__i386__) || \
    defined(MIPSEL) || defined(_MIPSEL) || defined(BIT_ZERO_ON_RIGHT) || \
    defined(__alpha__) || defined(__alpha)
#define BYTE_ORDER	LITTLE_ENDIAN
#endif

#if defined(sel) || defined(pyr) || defined(mc68000) || defined(sparc) || \
    defined(is68k) || defined(tahoe) || defined(ibm032) || defined(ibm370) || \
    defined(MIPSEB) || defined(_MIPSEB) || defined(_IBMR2) || defined(DGUX) ||\
    defined(apollo) || defined(__convex__) || defined(_CRAY) || \
    defined(__hppa) || defined(__hp9000) || \
    defined(__hp9000s300) || defined(__hp9000s700) || \
    defined (BIT_ZERO_ON_LEFT) || defined(m68k) || defined(__sparc)
#define BYTE_ORDER	BIG_ENDIAN
#endif
#endif /* linux */
#endif /* BSD */
#endif /* BYTE_ORDER */

#if defined(__BYTE_ORDER) && !defined(BYTE_ORDER)
#if (__BYTE_ORDER == __LITTLE_ENDIAN)
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#endif

#if !defined(BYTE_ORDER) || \
    (BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN && \
    BYTE_ORDER != PDP_ENDIAN)
	/* you must determine what the correct bit order is for
	 * your compiler - the next line is an intentional error
	 * which will force your compiles to bomb until you fix
	 * the above macros.
	 */
#error "Undefined or invalid BYTE_ORDER"
#endif

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
#if BYTE_ORDER == LITTLE_ENDIAN
#define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
    |(rol(block->l[i],8)&0x00FF00FF))
#elif BYTE_ORDER == BIG_ENDIAN
#define blk0(i) block->l[i]
#else
#error "Endianness not defined!"
#endif
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
    ^block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);


/* Hash a single 512-bit block. This is the core of the algorithm. */

void SHA1Transform(u_int32_t state[5], const unsigned char buffer[64])
{
u_int32_t a, b, c, d, e;
typedef union {
    unsigned char c[64];
    u_int32_t l[16];
} CHAR64LONG16;
#ifdef SHA1HANDSOFF
CHAR64LONG16 block[1];  /* use array to appear as a pointer */
    memcpy(block, buffer, 64);
#else
    /* The following had better never be used because it causes the
     * pointer-to-const buffer to be cast into a pointer to non-const.
     * And the result is written through.  I threw a "const" in, hoping
     * this will cause a diagnostic.
     */
CHAR64LONG16* block = (const CHAR64LONG16*)buffer;
#endif
    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);
    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    /* Wipe variables */
    a = b = c = d = e = 0;
#ifdef SHA1HANDSOFF
    memset(block, '\0', sizeof(block));
#endif
}


/* SHA1Init - Initialize new context */

void SHA1Init(SHA1_CTX* context)
{
    /* SHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}


/* Run your data through this. */

void SHA1Update(SHA1_CTX* context, const unsigned char* data, u_int32_t len)
{
u_int32_t i;
u_int32_t j;

    j = context->count[0];
    if ((context->count[0] += len << 3) < j)
	context->count[1]++;
    context->count[1] += (len>>29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64-j));
        SHA1Transform(context->state, context->buffer);
        for ( ; i + 63 < len; i += 64) {
            SHA1Transform(context->state, &data[i]);
        }
        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}


/* Add padding and return the message digest. */

void SHA1Final(unsigned char digest[20], SHA1_CTX* context)
{
unsigned i;
unsigned char finalcount[8];
unsigned char c;

#if 0	/* untested "improvement" by DHR */
    /* Convert context->count to a sequence of bytes
     * in finalcount.  Second element first, but
     * big-endian order within element.
     * But we do it all backwards.
     */
    unsigned char *fcp = &finalcount[8];

    for (i = 0; i < 2; i++)
    {
	u_int32_t t = context->count[i];
	int j;

	for (j = 0; j < 4; t >>= 8, j++)
	    *--fcp = (unsigned char) t
    }
#else
    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)]
         >> ((3-(i & 3)) * 8) ) & 255);  /* Endian independent */
    }
#endif
    c = 0200;
    SHA1Update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
	c = 0000;
        SHA1Update(context, &c, 1);
    }
    SHA1Update(context, finalcount, 8);  /* Should cause a SHA1Transform() */
    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)
         ((context->state[i>>2] >> ((3-(i & 3)) * 8) ) & 255);
    }
    /* Wipe variables */
    memset(context, '\0', sizeof(*context));
    memset(&finalcount, '\0', sizeof(finalcount));
}
/* ================ end of sha1.c ================ */

String
gen_sha1(String s, bool as_binary)
{
	unsigned char v[20];
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char *)s.data(), s.length());
    SHA1Final(v, &ctx);
	String result;
	if(as_binary)
		for(int i=0; i<20; i++)
			result.append(1, v[i]);
	else
		for(int i=0; i<20; i++)
			result += to_hex(v[i], 2);
	return(result);
}

#define BIT_NOISE1 0xB5297A4D
#define BIT_NOISE2 0x68E31DA4
#define BIT_NOISE3 0x1B56C4E9

// based on Squirrel3 https://www.youtube.com/watch?v=LWFzPP8ZbdU&t=2666s
u32 gen_noise32(u32 index, u32 seed)
{
	u32 r = index;
	r *= BIT_NOISE1;
	r += seed;
	r ^= (r >> 8);
	r += BIT_NOISE2;
	r ^= (r << 8);
	r *= BIT_NOISE3;
	r ^= (r >> 8);
	return(r);
}

#define BIT_NOISE61 0x5134811636f8cc8a
#define BIT_NOISE62 0xb8E31DA41B56C4E9
#define BIT_NOISE63 0x18cd227aaa1168c1

u64 gen_noise64(u64 index, u64 seed)
{
	u64 r = index;
	r *= BIT_NOISE61;
	r += seed;
	r ^= (r >> 8);
	r += BIT_NOISE62;
	r ^= (r << 8);
	r *= BIT_NOISE63;
	r ^= (r >> 8);
	return(r);
}

#define MAX_64 0xffffffffffffffff

f64 gen_noise01(u64 index, u64 seed)
{
	return((float)gen_noise64(index, seed)/(float)MAX_64);
}

u64 gen_int(u64 from, u64 to, u64 index, u64 seed)
{
	u64 b = 1 + to - from;
	return(from + (gen_noise64(index, seed) % b));
}

#include <tgmath.h>
f64 gen_float(f64 from, f64 to, u64 index, u64 seed, f64 decimal_precision)
{
	f64 b = to - from;
	return(from + fmod( decimal_precision*(f64)gen_noise64(index, seed), b));
}

u64 draw_int(u64 from, u64 to)
{
	return(gen_int(from, to, context->random_index++, context->random_seed));
}

f64 draw_float(f64 from, f64 to, f64 decimal_precision)
{
	return(gen_float(from, to, context->random_index++, context->random_seed, decimal_precision));
}


namespace {
struct SHA256_CTX_UCE { u8 data[64]; u32 datalen; unsigned long long bitlen; u32 state[8]; };
#define UCE_SHA256_ROTR(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define UCE_SHA256_CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define UCE_SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define UCE_SHA256_EP0(x) (UCE_SHA256_ROTR(x,2) ^ UCE_SHA256_ROTR(x,13) ^ UCE_SHA256_ROTR(x,22))
#define UCE_SHA256_EP1(x) (UCE_SHA256_ROTR(x,6) ^ UCE_SHA256_ROTR(x,11) ^ UCE_SHA256_ROTR(x,25))
#define UCE_SHA256_SIG0(x) (UCE_SHA256_ROTR(x,7) ^ UCE_SHA256_ROTR(x,18) ^ ((x) >> 3))
#define UCE_SHA256_SIG1(x) (UCE_SHA256_ROTR(x,17) ^ UCE_SHA256_ROTR(x,19) ^ ((x) >> 10))
static const u32 uce_sha256_k[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static void uce_sha256_transform(SHA256_CTX_UCE* ctx, const u8 data[])
{
	u32 m[64];
	for(u32 i=0,j=0; i<16; ++i,j+=4) m[i]=((u32)data[j]<<24)|((u32)data[j+1]<<16)|((u32)data[j+2]<<8)|((u32)data[j+3]);
	for(u32 i=16; i<64; ++i) m[i]=UCE_SHA256_SIG1(m[i-2])+m[i-7]+UCE_SHA256_SIG0(m[i-15])+m[i-16];
	u32 a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3],e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
	for(u32 i=0; i<64; ++i) { u32 t1=h+UCE_SHA256_EP1(e)+UCE_SHA256_CH(e,f,g)+uce_sha256_k[i]+m[i]; u32 t2=UCE_SHA256_EP0(a)+UCE_SHA256_MAJ(a,b,c); h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
	ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d; ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void uce_sha256_init(SHA256_CTX_UCE* ctx)
{
	ctx->datalen=0; ctx->bitlen=0; ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a; ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}
static void uce_sha256_update(SHA256_CTX_UCE* ctx, const u8 data[], size_t len)
{
	for(size_t i=0; i<len; ++i) { ctx->data[ctx->datalen++]=data[i]; if(ctx->datalen==64) { uce_sha256_transform(ctx,ctx->data); ctx->bitlen += 512; ctx->datalen=0; } }
}
static void uce_sha256_final(SHA256_CTX_UCE* ctx, u8 hash[])
{
	u32 i=ctx->datalen;
	ctx->data[i++]=0x80;
	if(i>56) { while(i<64) ctx->data[i++]=0; uce_sha256_transform(ctx,ctx->data); i=0; }
	while(i<56) ctx->data[i++]=0;
	ctx->bitlen += (unsigned long long)ctx->datalen * 8ull;
	for(int j=7; j>=0; --j) ctx->data[63-j]=(u8)(ctx->bitlen >> (j*8));
	uce_sha256_transform(ctx,ctx->data);
	for(i=0; i<4; ++i) for(u32 j=0; j<8; ++j) hash[i + j*4] = (u8)((ctx->state[j] >> (24 - i*8)) & 0xff);
}
}

String sha256_native(String data)
{
	u8 digest[32]; SHA256_CTX_UCE ctx; uce_sha256_init(&ctx); uce_sha256_update(&ctx, (const u8*)data.data(), data.size()); uce_sha256_final(&ctx, digest);
	return(String((const char*)digest, 32));
}
String sha256_hex_native(String data)
{
	String digest = sha256_native(data), out; for(unsigned char c : digest) out += to_hex(c, 2); return(to_lower(out));
}
String hmac_sha256_native(String key, String data)
{
	if(key.size() > 64) key = sha256_native(key);
	key.resize(64, '\0');
	String o(64, '\0'), i(64, '\0');
	for(size_t n=0; n<64; n++) { o[n] = key[n] ^ 0x5c; i[n] = key[n] ^ 0x36; }
	return(sha256_native(o + sha256_native(i + data)));
}
String hmac_sha256_hex_native(String key, String data)
{
	String digest = hmac_sha256_native(key, data), out; for(unsigned char c : digest) out += to_hex(c, 2); return(to_lower(out));
}
bool crypto_equal_native(String a, String b)
{
	u8 diff = (u8)(a.size() ^ b.size());
	size_t n = a.size() > b.size() ? a.size() : b.size();
	for(size_t i=0; i<n; i++) { u8 ca = i<a.size() ? (u8)a[i] : 0; u8 cb = i<b.size() ? (u8)b[i] : 0; diff |= ca ^ cb; }
	return(diff == 0);
}

#ifndef __UCE_WASM_CORE__
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
const u64 UCE_PASSWORD_SCRYPT_N = 65536;
const u64 UCE_PASSWORD_SCRYPT_R = 8;
const u64 UCE_PASSWORD_SCRYPT_P = 1;

String uce_hex_encode(const unsigned char* bytes, size_t size)
{
	String encoded;
	encoded.reserve(size * 2);
	for(size_t i = 0; i < size; i++)
		encoded += to_hex(bytes[i], 2);
	return(to_lower(encoded));
}

bool uce_hex_decode(String encoded, String& decoded)
{
	if(encoded.size() % 2 != 0)
		return(false);
	decoded.clear();
	decoded.reserve(encoded.size() / 2);
	for(size_t i = 0; i < encoded.size(); i += 2)
	{
		u8 value = 0;
		for(size_t j = 0; j < 2; j++)
		{
			char c = encoded[i + j];
			u8 digit = c >= '0' && c <= '9' ? (u8)(c - '0') : c >= 'a' && c <= 'f' ? (u8)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u8)(c - 'A' + 10) : 255;
			if(digit == 255)
				return(false);
			value = (u8)((value << 4) | digit);
		}
		decoded.push_back((char)value);
	}
	return(true);
}

bool uce_decimal_u64(String value, u64& parsed)
{
	if(value == "")
		return(false);
	parsed = 0;
	for(char c : value)
	{
		if(c < '0' || c > '9' || parsed > (UINT64_MAX - (u64)(c - '0')) / 10)
			return(false);
		parsed = parsed * 10 + (u64)(c - '0');
	}
	return(true);
}

bool uce_password_parts(String encoded, u64& n, u64& r, u64& p, String& salt, String& digest)
{
	const String prefix = "$uce$scrypt$";
	if(encoded.size() <= prefix.size() || encoded.substr(0, prefix.size()) != prefix)
		return(false);
	auto parts = split(encoded.substr(prefix.size()), "$");
	if(parts.size() != 5 || !uce_decimal_u64(parts[0], n) || !uce_decimal_u64(parts[1], r) || !uce_decimal_u64(parts[2], p))
		return(false);
	if(n < 16384 || n > UCE_PASSWORD_SCRYPT_N || (n & (n - 1)) != 0 || r < 1 || r > UCE_PASSWORD_SCRYPT_R || p != UCE_PASSWORD_SCRYPT_P || n * r > UCE_PASSWORD_SCRYPT_N * UCE_PASSWORD_SCRYPT_R)
		return(false);
	return(uce_hex_decode(parts[3], salt) && salt.size() == 16 && uce_hex_decode(parts[4], digest) && digest.size() == 32);
}

bool uce_password_derive(String password, String salt, u64 n, u64 r, u64 p, unsigned char* output)
{
	if(password.size() > 1024 * 1024)
		return(false);
	u64 max_memory = 128 * n * r + 2 * 1024 * 1024;
	return(EVP_PBE_scrypt(password.data(), password.size(), (const unsigned char*)salt.data(), salt.size(), n, r, p, max_memory, output, 32) == 1);
}
}

String password_hash_native(String password)
{
	unsigned char salt[16];
	unsigned char digest[32];
	if(RAND_bytes(salt, sizeof(salt)) != 1 || !uce_password_derive(password, String((const char*)salt, sizeof(salt)), UCE_PASSWORD_SCRYPT_N, UCE_PASSWORD_SCRYPT_R, UCE_PASSWORD_SCRYPT_P, digest))
		return("");
	return("$uce$scrypt$" + std::to_string(UCE_PASSWORD_SCRYPT_N) + "$" + std::to_string(UCE_PASSWORD_SCRYPT_R) + "$" + std::to_string(UCE_PASSWORD_SCRYPT_P) + "$" + uce_hex_encode(salt, sizeof(salt)) + "$" + uce_hex_encode(digest, sizeof(digest)));
}

bool password_verify_native(String password, String encoded)
{
	u64 n = 0, r = 0, p = 0;
	String salt, expected;
	if(!uce_password_parts(encoded, n, r, p, salt, expected))
		return(false);
	unsigned char digest[32];
	if(!uce_password_derive(password, salt, n, r, p, digest))
		return(false);
	return(crypto_equal_native(String((const char*)digest, sizeof(digest)), expected));
}

bool password_needs_rehash_native(String encoded)
{
	u64 n = 0, r = 0, p = 0;
	String salt, digest;
	return(!uce_password_parts(encoded, n, r, p, salt, digest) || n != UCE_PASSWORD_SCRYPT_N || r != UCE_PASSWORD_SCRYPT_R || p != UCE_PASSWORD_SCRYPT_P);
}
#endif
