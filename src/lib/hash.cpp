
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

#ifdef __BEARER_WASM_CORE__
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
#include "uri.h"

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
struct SHA256_CTX_BEARER { u8 data[64]; u32 datalen; unsigned long long bitlen; u32 state[8]; };
#define BEARER_SHA256_ROTR(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define BEARER_SHA256_CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define BEARER_SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BEARER_SHA256_EP0(x) (BEARER_SHA256_ROTR(x,2) ^ BEARER_SHA256_ROTR(x,13) ^ BEARER_SHA256_ROTR(x,22))
#define BEARER_SHA256_EP1(x) (BEARER_SHA256_ROTR(x,6) ^ BEARER_SHA256_ROTR(x,11) ^ BEARER_SHA256_ROTR(x,25))
#define BEARER_SHA256_SIG0(x) (BEARER_SHA256_ROTR(x,7) ^ BEARER_SHA256_ROTR(x,18) ^ ((x) >> 3))
#define BEARER_SHA256_SIG1(x) (BEARER_SHA256_ROTR(x,17) ^ BEARER_SHA256_ROTR(x,19) ^ ((x) >> 10))
static const u32 bearer_sha256_k[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static void bearer_sha256_transform(SHA256_CTX_BEARER* ctx, const u8 data[])
{
	u32 m[64];
	for(u32 i=0,j=0; i<16; ++i,j+=4) m[i]=((u32)data[j]<<24)|((u32)data[j+1]<<16)|((u32)data[j+2]<<8)|((u32)data[j+3]);
	for(u32 i=16; i<64; ++i) m[i]=BEARER_SHA256_SIG1(m[i-2])+m[i-7]+BEARER_SHA256_SIG0(m[i-15])+m[i-16];
	u32 a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3],e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
	for(u32 i=0; i<64; ++i) { u32 t1=h+BEARER_SHA256_EP1(e)+BEARER_SHA256_CH(e,f,g)+bearer_sha256_k[i]+m[i]; u32 t2=BEARER_SHA256_EP0(a)+BEARER_SHA256_MAJ(a,b,c); h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
	ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d; ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void bearer_sha256_init(SHA256_CTX_BEARER* ctx)
{
	ctx->datalen=0; ctx->bitlen=0; ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a; ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}
static void bearer_sha256_update(SHA256_CTX_BEARER* ctx, const u8 data[], size_t len)
{
	for(size_t i=0; i<len; ++i) { ctx->data[ctx->datalen++]=data[i]; if(ctx->datalen==64) { bearer_sha256_transform(ctx,ctx->data); ctx->bitlen += 512; ctx->datalen=0; } }
}
static void bearer_sha256_final(SHA256_CTX_BEARER* ctx, u8 hash[])
{
	u32 i=ctx->datalen;
	ctx->data[i++]=0x80;
	if(i>56) { while(i<64) ctx->data[i++]=0; bearer_sha256_transform(ctx,ctx->data); i=0; }
	while(i<56) ctx->data[i++]=0;
	ctx->bitlen += (unsigned long long)ctx->datalen * 8ull;
	for(int j=7; j>=0; --j) ctx->data[63-j]=(u8)(ctx->bitlen >> (j*8));
	bearer_sha256_transform(ctx,ctx->data);
	for(i=0; i<4; ++i) for(u32 j=0; j<8; ++j) hash[i + j*4] = (u8)((ctx->state[j] >> (24 - i*8)) & 0xff);
}
}

String sha256_native(String data)
{
	u8 digest[32]; SHA256_CTX_BEARER ctx; bearer_sha256_init(&ctx); bearer_sha256_update(&ctx, (const u8*)data.data(), data.size()); bearer_sha256_final(&ctx, digest);
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

static bool uce_crypto_utf8_string(String value)
{
	for(size_t i = 0; i < value.size();)
	{
		u8 c = (u8)value[i];
		if(c < 0x80) { i++; continue; }
		size_t need = c >= 0xC2 && c <= 0xDF ? 1 : (c >= 0xE0 && c <= 0xEF ? 2 : (c >= 0xF0 && c <= 0xF4 ? 3 : 0));
		if(need == 0 || i + need >= value.size()) return(false);
		for(size_t j = 1; j <= need; j++) if(((u8)value[i + j] & 0xC0) != 0x80) return(false);
		u8 second = (u8)value[i + 1];
		if((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0) || (c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) return(false);
		i += need + 1;
	}
	return(true);
}

static bool uce_crypto_utf8_json_string(String value)
{
	if(!uce_crypto_utf8_string(value)) return(false);
	for(unsigned char c : value) if(c < 0x20) return(false);
	return(true);
}

static bool uce_crypto_value_valid(const DValue& value, size_t depth, size_t& nodes, size_t& bytes)
{
	if(depth > 16 || ++nodes > 256) return(false);
	const DValue& item = value.deref();
	if(item.type == 'S')
	{
		bytes += item._String.size();
		return(bytes <= 32768 && uce_crypto_utf8_json_string(item._String));
	}
	if(item.type == 'F') return(std::isfinite(item._float));
	if(item.type == 'B') return(true);
	if(item.type != 'M') return(false);
	bool list = item.is_list();
	for(const auto& child : item._map)
	{
		if(!list)
		{
			bytes += child.first.size();
			if(bytes > 32768 || !uce_crypto_utf8_json_string(child.first)) return(false);
		}
		if(!uce_crypto_value_valid(child.second, depth + 1, nodes, bytes)) return(false);
	}
	return(true);
}

bool crypto_operation_request_valid(DValue request)
{
	const DValue& root = request.deref();
	if(root.type != 'M' || root.is_list()) return(false);
	size_t nodes = 0, bytes = 0;
	return(uce_crypto_value_valid(root, 0, nodes, bytes));
}


#ifndef __BEARER_WASM_CORE__
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
const u64 BEARER_PASSWORD_SCRYPT_N = 65536;
const u64 BEARER_PASSWORD_SCRYPT_R = 8;
const u64 BEARER_PASSWORD_SCRYPT_P = 1;

String bearer_hex_encode(const unsigned char* bytes, size_t size)
{
	String encoded;
	encoded.reserve(size * 2);
	for(size_t i = 0; i < size; i++)
		encoded += to_hex(bytes[i], 2);
	return(to_lower(encoded));
}

bool bearer_hex_decode(String encoded, String& decoded)
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

bool bearer_decimal_u64(String value, u64& parsed)
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

bool bearer_password_parts(String encoded, u64& n, u64& r, u64& p, String& salt, String& digest)
{
	const String prefix = "$bearer$scrypt$";
	if(encoded.size() <= prefix.size() || encoded.substr(0, prefix.size()) != prefix)
		return(false);
	auto parts = split_strings(encoded.substr(prefix.size()), "$");
	if(parts.size() != 5 || !bearer_decimal_u64(parts[0], n) || !bearer_decimal_u64(parts[1], r) || !bearer_decimal_u64(parts[2], p))
		return(false);
	if(n < 16384 || n > BEARER_PASSWORD_SCRYPT_N || (n & (n - 1)) != 0 || r < 1 || r > BEARER_PASSWORD_SCRYPT_R || p != BEARER_PASSWORD_SCRYPT_P || n * r > BEARER_PASSWORD_SCRYPT_N * BEARER_PASSWORD_SCRYPT_R)
		return(false);
	return(bearer_hex_decode(parts[3], salt) && salt.size() == 16 && bearer_hex_decode(parts[4], digest) && digest.size() == 32);
}

bool bearer_password_derive(String password, String salt, u64 n, u64 r, u64 p, unsigned char* output)
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
	if(RAND_bytes(salt, sizeof(salt)) != 1 || !bearer_password_derive(password, String((const char*)salt, sizeof(salt)), BEARER_PASSWORD_SCRYPT_N, BEARER_PASSWORD_SCRYPT_R, BEARER_PASSWORD_SCRYPT_P, digest))
		return("");
	return("$bearer$scrypt$" + std::to_string(BEARER_PASSWORD_SCRYPT_N) + "$" + std::to_string(BEARER_PASSWORD_SCRYPT_R) + "$" + std::to_string(BEARER_PASSWORD_SCRYPT_P) + "$" + bearer_hex_encode(salt, sizeof(salt)) + "$" + bearer_hex_encode(digest, sizeof(digest)));
}

bool password_verify_native(String password, String encoded)
{
	u64 n = 0, r = 0, p = 0;
	String salt, expected;
	if(!bearer_password_parts(encoded, n, r, p, salt, expected))
		return(false);
	unsigned char digest[32];
	if(!bearer_password_derive(password, salt, n, r, p, digest))
		return(false);
	return(crypto_equal_native(String((const char*)digest, sizeof(digest)), expected));
}

bool password_needs_rehash_native(String encoded)
{
	u64 n = 0, r = 0, p = 0;
	String salt, digest;
	return(!bearer_password_parts(encoded, n, r, p, salt, digest) || n != BEARER_PASSWORD_SCRYPT_N || r != BEARER_PASSWORD_SCRYPT_R || p != BEARER_PASSWORD_SCRYPT_P);
}
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

namespace {
static constexpr size_t UCE_ES256_COORDINATE_BYTES = 32;
static constexpr size_t UCE_ES256_SIGNATURE_BYTES = 64;
static constexpr size_t UCE_ES256_JSON_MAX = 16 * 1024;
static constexpr size_t UCE_ES256_VALUE_MAX = 256;
static constexpr size_t UCE_ES256_DEPTH_MAX = 16;
static constexpr size_t UCE_ES256_COORDINATE_BASE64URL_MAX = 43;
static constexpr size_t UCE_CBOR_MAX_BYTES = 16 * 1024;
static constexpr size_t UCE_CBOR_MAX_BASE64URL = 21846;
static constexpr size_t UCE_ES256_DER_MAX_BYTES = 144;
static constexpr size_t UCE_ES256_DER_BASE64URL_MAX = 192;

struct UcePkeyDeleter { void operator()(EVP_PKEY* value) const { EVP_PKEY_free(value); } };
struct UcePkeyCtxDeleter { void operator()(EVP_PKEY_CTX* value) const { EVP_PKEY_CTX_free(value); } };
struct UceEcdsaSigDeleter { void operator()(ECDSA_SIG* value) const { ECDSA_SIG_free(value); } };
struct UceBnDeleter { void operator()(BIGNUM* value) const { BN_clear_free(value); } };
struct UceParamBldDeleter { void operator()(OSSL_PARAM_BLD* value) const { OSSL_PARAM_BLD_free(value); } };
struct UceParamsDeleter { void operator()(OSSL_PARAM* value) const { OSSL_PARAM_free(value); } };

static String uce_base64url_encode(const unsigned char* bytes, size_t size)
{
	String encoded = base64_encode(String((const char*)bytes, size));
	encoded = replace(replace(encoded, "+", "-"), "/", "_");
	while(!encoded.empty() && encoded.back() == '=') encoded.pop_back();
	return(encoded);
}

static bool uce_base64url_decode(String encoded, String& decoded, size_t max_encoded)
{
	if(encoded.empty() || encoded.size() > max_encoded || encoded.find('=') != String::npos || encoded.size() % 4 == 1)
		return(false);
	for(char c : encoded)
		if(!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' && c != '_')
			return(false);
	String padded = replace(replace(encoded, "-", "+"), "_", "/");
	while(padded.size() % 4) padded += "=";
	bool ok = false;
	decoded = base64_decode(padded, ok);
	return(ok && uce_base64url_encode((const unsigned char*)decoded.data(), decoded.size()) == encoded);
}

static bool uce_es256_json_value(const DValue& value, size_t depth, size_t& values, size_t& bytes)
{
	if(depth > UCE_ES256_DEPTH_MAX || ++values > UCE_ES256_VALUE_MAX)
		return(false);
	const DValue& item = value.deref();
	if(item.type == 'S')
		return((bytes += item._String.size()) <= UCE_ES256_JSON_MAX);
	if(item.type == 'F' || item.type == 'B')
		return(true);
	if(item.type != 'M')
		return(false);
	for(const auto& child : item._map)
	{
		if((bytes += child.first.size()) > UCE_ES256_JSON_MAX || !uce_es256_json_value(child.second, depth + 1, values, bytes))
			return(false);
	}
	return(true);
}

static bool uce_es256_json_map(const DValue& value)
{
	if(value.deref().type != 'M' || value.deref().is_list())
		return(false);
	size_t values = 0, bytes = 0;
	return(uce_es256_json_value(value, 0, values, bytes));
}

static bool uce_es256_jwk_string(const DValue& jwk, const String& field, String& value)
{
	const DValue* found = jwk.key(field);
	if(!found)
		return(false);
	const DValue& item = found->deref();
	if(item.type != 'S' || item._String.empty() || item._String.size() > 32768)
		return(false);
	value = item._String;
	return(true);
}

static std::unique_ptr<EVP_PKEY, UcePkeyDeleter> uce_es256_key_from_jwk(const DValue& jwk)
{
	if(jwk.deref().type != 'M')
		return(nullptr);
	String kty, crv, x64, y64, d64, x, y, d;
	if(!uce_es256_jwk_string(jwk, "kty", kty) || !uce_es256_jwk_string(jwk, "crv", crv) || !uce_es256_jwk_string(jwk, "x", x64) || !uce_es256_jwk_string(jwk, "y", y64) || !uce_es256_jwk_string(jwk, "d", d64) ||
		kty != "EC" || crv != "P-256" || !uce_base64url_decode(x64, x, UCE_ES256_COORDINATE_BASE64URL_MAX) || !uce_base64url_decode(y64, y, UCE_ES256_COORDINATE_BASE64URL_MAX) || !uce_base64url_decode(d64, d, UCE_ES256_COORDINATE_BASE64URL_MAX) ||
		x.size() != UCE_ES256_COORDINATE_BYTES || y.size() != UCE_ES256_COORDINATE_BYTES || d.size() != UCE_ES256_COORDINATE_BYTES)
		return(nullptr);
	unsigned char public_key[65];
	public_key[0] = 4;
	memcpy(public_key + 1, x.data(), x.size());
	memcpy(public_key + 33, y.data(), y.size());
	std::unique_ptr<BIGNUM, UceBnDeleter> private_bn(BN_bin2bn((const unsigned char*)d.data(), d.size(), 0));
	std::unique_ptr<OSSL_PARAM_BLD, UceParamBldDeleter> builder(OSSL_PARAM_BLD_new());
	if(!private_bn || !builder || OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) <= 0 ||
		OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, public_key, sizeof(public_key)) <= 0 ||
		OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, private_bn.get()) <= 0)
		return(nullptr);
	std::unique_ptr<OSSL_PARAM, UceParamsDeleter> params(OSSL_PARAM_BLD_to_param(builder.get()));
	std::unique_ptr<EVP_PKEY_CTX, UcePkeyCtxDeleter> ctx(EVP_PKEY_CTX_new_from_name(0, "EC", 0));
	EVP_PKEY* raw = 0;
	if(!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 || EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_KEYPAIR, params.get()) <= 0)
		return(nullptr);
	std::unique_ptr<EVP_PKEY, UcePkeyDeleter> key(raw);
	std::unique_ptr<EVP_PKEY_CTX, UcePkeyCtxDeleter> check(EVP_PKEY_CTX_new(key.get(), 0));
	return(check && EVP_PKEY_public_check(check.get()) > 0 && EVP_PKEY_private_check(check.get()) > 0 && EVP_PKEY_pairwise_check(check.get()) > 0 ? std::move(key) : nullptr);
}

static bool uce_es256_key_coordinates(EVP_PKEY* key, String& x, String& y, String& d)
{
	unsigned char public_key[65]; size_t public_key_size = sizeof(public_key);
	BIGNUM* private_bn = 0;
	if(EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, public_key, sizeof(public_key), &public_key_size) <= 0 || public_key_size != sizeof(public_key) || public_key[0] != 4 ||
		EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &private_bn) <= 0)
		return(false);
	std::unique_ptr<BIGNUM, UceBnDeleter> private_key(private_bn);
	if(BN_bn2binpad(private_key.get(), (unsigned char*)d.data(), UCE_ES256_COORDINATE_BYTES) != UCE_ES256_COORDINATE_BYTES)
		return(false);
	x.assign((const char*)public_key + 1, UCE_ES256_COORDINATE_BYTES);
	y.assign((const char*)public_key + 33, UCE_ES256_COORDINATE_BYTES);
	return(true);
}

enum class UceCborKind { Unsigned, Negative, Bytes, Text, Array, Map };
struct UceCbor { UceCborKind kind; u64 number = 0; String bytes; std::vector<UceCbor> items; };
static constexpr size_t UCE_CBOR_MAX_NODES=256, UCE_CBOR_MAX_DEPTH=16;

static bool uce_cbor_uint(const String& in,size_t& p,u8 ai,u64& n)
{
	if(ai<24) { n=ai; return true; } size_t count=ai==24?1:ai==25?2:ai==26?4:ai==27?8:0;
	if(!count||p>in.size()||count>in.size()-p) return false; n=0;
	for(size_t i=0;i<count;i++) { if(n>(UINT64_MAX-(u8)in[p+i])/256) return false; n=n*256+(u8)in[p+i]; } p+=count;
	return((count==1&&n>=24)||(count==2&&n>0xff)||(count==4&&n>0xffff)||(count==8&&n>0xffffffff));
}
static bool uce_cbor_equal(const UceCbor& left, const UceCbor& right)
{
	if(left.kind != right.kind || left.number != right.number || left.bytes != right.bytes || left.items.size() != right.items.size()) return(false);
	for(size_t i = 0; i < left.items.size(); i++) if(!uce_cbor_equal(left.items[i], right.items[i])) return(false);
	return(true);
}
static bool uce_cbor_read(const String& in,size_t& p,UceCbor& out,size_t depth,size_t& nodes)
{
	if(p>=in.size()||depth>UCE_CBOR_MAX_DEPTH||++nodes>UCE_CBOR_MAX_NODES) return false;
	u8 h=(u8)in[p++], major=h>>5, ai=h&31; u64 n=0; if(ai==31||!uce_cbor_uint(in,p,ai,n)) return false;
	if(major==0||major==1) { out.kind=major?UceCborKind::Negative:UceCborKind::Unsigned; out.number=n; return true; }
	if(major==2||major==3) { if(n>UCE_CBOR_MAX_BYTES||n>in.size()-p||(major==3&&!uce_crypto_utf8_string(String(in.data()+p,(size_t)n)))) return false; out.kind=major==2?UceCborKind::Bytes:UceCborKind::Text; out.bytes.assign(in.data()+p,(size_t)n); p+=(size_t)n; return true; }
	if((major!=4&&major!=5)||n>UCE_CBOR_MAX_NODES) return false;
	out.kind=major==4?UceCborKind::Array:UceCborKind::Map; if(n>(UCE_CBOR_MAX_NODES-nodes)/(major==5?2:1)) return false; out.items.reserve((size_t)n*(major==5?2:1));
	for(u64 i=0;i<n*(major==5?2:1);i++) { UceCbor child; if(!uce_cbor_read(in,p,child,depth+1,nodes)) return false; if(major==5&&!(i&1)) for(size_t previous=0;previous<out.items.size();previous+=2) if(uce_cbor_equal(out.items[previous],child)) return false; out.items.push_back(std::move(child)); }
	return true;
}
static DValue uce_cbor_value(const UceCbor& value)
{
	DValue out; switch(value.kind) { case UceCborKind::Unsigned: out["type"]="unsigned"; out["value"]=std::to_string(value.number); break; case UceCborKind::Negative: out["type"]="negative"; out["value"]=value.number==UINT64_MAX?"-18446744073709551616":"-"+std::to_string(value.number+1); break; case UceCborKind::Bytes: out["type"]="bytes"; out["base64url"]=uce_base64url_encode((const unsigned char*)value.bytes.data(),value.bytes.size()); break; case UceCborKind::Text: out["type"]="text"; out["value"]=value.bytes; break; case UceCborKind::Array: out["type"]="array"; out["items"].set_array(); for(const auto& x:value.items) out["items"].push(uce_cbor_value(x)); break; case UceCborKind::Map: out["type"]="map"; out["entries"].set_array(); for(size_t i=0;i<value.items.size();i+=2) { DValue entry; entry["key"]=uce_cbor_value(value.items[i]); entry["value"]=uce_cbor_value(value.items[i+1]); out["entries"].push(entry); } } return out;
}
static bool uce_cbor_es256(const UceCbor& cose,String& x,String& y)
{
	if(cose.kind!=UceCborKind::Map) return false; const UceCbor *kty=0,*alg=0,*crv=0,*xx=0,*yy=0;
	for(size_t i=0;i<cose.items.size();i+=2) { const UceCbor& k=cose.items[i]; const UceCbor& v=cose.items[i+1]; if(k.kind!=UceCborKind::Unsigned&&k.kind!=UceCborKind::Negative) continue; bool neg=k.kind==UceCborKind::Negative; if(!neg&&k.number==1) kty=&v; else if(!neg&&k.number==3) alg=&v; else if(neg&&k.number==0) crv=&v; else if(neg&&k.number==1) xx=&v; else if(neg&&k.number==2) yy=&v; }
	return(kty&&alg&&crv&&xx&&yy&&kty->kind==UceCborKind::Unsigned&&kty->number==2&&alg->kind==UceCborKind::Negative&&alg->number==6&&crv->kind==UceCborKind::Unsigned&&crv->number==1&&xx->kind==UceCborKind::Bytes&&yy->kind==UceCborKind::Bytes&&xx->bytes.size()==32&&yy->bytes.size()==32&&(x=xx->bytes,true)&&(y=yy->bytes,true));
}
static std::unique_ptr<EVP_PKEY,UcePkeyDeleter> uce_es256_public_key(String x,String y)
{
	if(x.size()!=32||y.size()!=32) return nullptr; unsigned char point[65]={4}; memcpy(point+1,x.data(),32); memcpy(point+33,y.data(),32); OSSL_PARAM p[]={OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,(char*)"prime256v1",0),OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,point,sizeof(point)),OSSL_PARAM_construct_end()}; std::unique_ptr<EVP_PKEY_CTX,UcePkeyCtxDeleter> ctx(EVP_PKEY_CTX_new_from_name(0,"EC",0)); EVP_PKEY* raw=0; if(!ctx||EVP_PKEY_fromdata_init(ctx.get())<=0||EVP_PKEY_fromdata(ctx.get(),&raw,EVP_PKEY_PUBLIC_KEY,p)<=0) return nullptr; std::unique_ptr<EVP_PKEY,UcePkeyDeleter> key(raw); std::unique_ptr<EVP_PKEY_CTX,UcePkeyCtxDeleter> check(EVP_PKEY_CTX_new(key.get(),0)); return check&&EVP_PKEY_public_check(check.get())>0?std::move(key):nullptr;
}
static bool uce_es256_cose(String encoded,String& x,String& y)
{
	String raw; UceCbor cose; size_t p=0,nodes=0; return uce_base64url_decode(encoded,raw,UCE_CBOR_MAX_BASE64URL)&&raw.size()<=UCE_CBOR_MAX_BYTES&&uce_cbor_read(raw,p,cose,0,nodes)&&p==raw.size()&&uce_cbor_es256(cose,x,y)&&uce_es256_public_key(x,y);
}

static DValue uce_es256_jwk(String x, String y, String d = "")
{
	DValue jwk;
	jwk["kty"] = "EC";
	jwk["crv"] = "P-256";
	jwk["x"] = uce_base64url_encode((const unsigned char*)x.data(), x.size());
	jwk["y"] = uce_base64url_encode((const unsigned char*)y.data(), y.size());
	if(d != "") jwk["d"] = uce_base64url_encode((const unsigned char*)d.data(), d.size());
	return(jwk);
}

static String uce_es256_thumbprint(const DValue& public_jwk)
{
	const DValue* x = public_jwk.key("x");
	const DValue* y = public_jwk.key("y");
	if(!x || !y)
		return("");
	String canonical = "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + x->to_string() + "\",\"y\":\"" + y->to_string() + "\"}";
	String digest = sha256_native(canonical);
	return(uce_base64url_encode((const unsigned char*)digest.data(), digest.size()));
}
}

static DValue uce_es256_key_create()
{
	std::unique_ptr<EVP_PKEY_CTX, UcePkeyCtxDeleter> ctx(EVP_PKEY_CTX_new_from_name(0, "EC", 0));
	EVP_PKEY* raw = 0;
	OSSL_PARAM params[] = { OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char*)"prime256v1", 0), OSSL_PARAM_construct_end() };
	if(!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_params(ctx.get(), params) <= 0 || EVP_PKEY_generate(ctx.get(), &raw) <= 0)
		return(DValue());
	std::unique_ptr<EVP_PKEY, UcePkeyDeleter> key(raw);
	String x(UCE_ES256_COORDINATE_BYTES, 0), y(UCE_ES256_COORDINATE_BYTES, 0), d(UCE_ES256_COORDINATE_BYTES, 0);
	if(!uce_es256_key_coordinates(key.get(), x, y, d))
		return(DValue());
	DValue result;
	result["public_jwk"] = uce_es256_jwk(x, y);
	String kid = uce_es256_thumbprint(result["public_jwk"]);
	result["public_jwk"]["kid"] = kid;
	result["private_jwk"] = uce_es256_jwk(x, y, d);
	result["private_jwk"]["kid"] = kid;
	result["kid"] = kid;
	result["thumbprint"] = kid;
	return(result);
}

static String uce_es256_jwt(DValue private_jwk, DValue protected_header, DValue claims)
{
	if(!uce_es256_json_map(protected_header) || !uce_es256_json_map(claims))
		return("");
	std::unique_ptr<EVP_PKEY, UcePkeyDeleter> key = uce_es256_key_from_jwk(private_jwk);
	if(!key)
		return("");
	protected_header["alg"] = "ES256";
	String header_json = json_encode(protected_header);
	String claims_json = json_encode(claims);
	if(header_json.size() > UCE_ES256_JSON_MAX || claims_json.size() > UCE_ES256_JSON_MAX)
		return("");
	String signing_input = uce_base64url_encode((const unsigned char*)header_json.data(), header_json.size()) + "." + uce_base64url_encode((const unsigned char*)claims_json.data(), claims_json.size());
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	size_t der_size = 0;
	if(!ctx || EVP_DigestSignInit(ctx.get(), 0, EVP_sha256(), 0, key.get()) <= 0 || EVP_DigestSign(ctx.get(), 0, &der_size, (const unsigned char*)signing_input.data(), signing_input.size()) <= 0 || der_size == 0 || der_size > 256)
		return("");
	String der(der_size, 0);
	if(EVP_DigestSign(ctx.get(), (unsigned char*)der.data(), &der_size, (const unsigned char*)signing_input.data(), signing_input.size()) <= 0)
		return("");
	const unsigned char* cursor = (const unsigned char*)der.data();
	std::unique_ptr<ECDSA_SIG, UceEcdsaSigDeleter> signature(d2i_ECDSA_SIG(0, &cursor, der_size));
	const BIGNUM *r = 0, *s = 0;
	if(!signature || cursor != (const unsigned char*)der.data() + der_size)
		return("");
	ECDSA_SIG_get0(signature.get(), &r, &s);
	String jose(UCE_ES256_SIGNATURE_BYTES, 0);
	if(!r || !s || BN_bn2binpad(r, (unsigned char*)jose.data(), UCE_ES256_COORDINATE_BYTES) != UCE_ES256_COORDINATE_BYTES || BN_bn2binpad(s, (unsigned char*)jose.data() + UCE_ES256_COORDINATE_BYTES, UCE_ES256_COORDINATE_BYTES) != UCE_ES256_COORDINATE_BYTES)
		return("");
	return(signing_input + "." + uce_base64url_encode((const unsigned char*)jose.data(), jose.size()));
}

DValue crypto_operation_native(DValue request)
{
	DValue result;
	result["ok"].set_bool(false);
	if(!crypto_operation_request_valid(request))
	{
		result["error"] = "invalid_request";
		return(result);
	}
	String operation, algorithm;
	if(!uce_es256_jwk_string(request, "operation", operation) || !uce_es256_jwk_string(request, "algorithm", algorithm))
	{
		result["error"] = "invalid_request";
		return(result);
	}
	if(algorithm != "ES256")
	{
		result["error"] = "unsupported_algorithm";
		return(result);
	}
	if(operation == "key_generate")
	{
		DValue key = uce_es256_key_create();
		if(key["private_jwk"]["d"].to_string() == "")
		{
			result["error"] = "operation_failed";
			return(result);
		}
		key["ok"].set_bool(true);
		key["operation"] = operation;
		key["algorithm"] = algorithm;
		return(key);
	}
	if(operation == "cbor_decode")
	{
		String encoded, raw; UceCbor value; size_t p=0,nodes=0;
		if(!uce_es256_jwk_string(request,"cbor_base64url",encoded) || !uce_base64url_decode(encoded,raw,UCE_CBOR_MAX_BASE64URL) || raw.size()>UCE_CBOR_MAX_BYTES || !uce_cbor_read(raw,p,value,0,nodes) || p!=raw.size()) { result["error"]="invalid_cbor"; return result; }
		result["ok"].set_bool(true); result["operation"]=operation; result["value"]=uce_cbor_value(value); return result;
	}
	if(operation == "cose_es256_parse")
	{
		String encoded,x,y; if(!uce_es256_jwk_string(request,"cose_key_base64url",encoded)||!uce_es256_cose(encoded,x,y)) { result["error"]="invalid_cose_key"; return result; }
		result["ok"].set_bool(true); result["operation"]=operation; result["algorithm"]=algorithm; result["x_base64url"]=uce_base64url_encode((const unsigned char*)x.data(),x.size()); result["y_base64url"]=uce_base64url_encode((const unsigned char*)y.data(),y.size()); return result;
	}
	if(operation == "es256_verify")
	{
		String encoded,message64,signature64,x,y,message,der; if(!uce_es256_jwk_string(request,"cose_key_base64url",encoded)||!uce_es256_jwk_string(request,"message_base64url",message64)||!uce_es256_jwk_string(request,"signature_der_base64url",signature64)||!uce_es256_cose(encoded,x,y)||!uce_base64url_decode(message64,message,UCE_CBOR_MAX_BASE64URL)||!uce_base64url_decode(signature64,der,UCE_ES256_DER_BASE64URL_MAX)||message.size()>UCE_CBOR_MAX_BYTES||der.empty()||der.size()>UCE_ES256_DER_MAX_BYTES) { result["error"]="invalid_key_or_payload"; return result; }
		const unsigned char* cursor=(const unsigned char*)der.data(); std::unique_ptr<ECDSA_SIG,UceEcdsaSigDeleter> signature(d2i_ECDSA_SIG(0,&cursor,der.size())); int canonical=signature?i2d_ECDSA_SIG(signature.get(),0):0; String reencoded(canonical>0?(size_t)canonical:0,0); unsigned char* dest=(unsigned char*)reencoded.data(); if(!signature||cursor!=(const unsigned char*)der.data()+der.size()||canonical<=0||i2d_ECDSA_SIG(signature.get(),&dest)!=canonical||reencoded!=der) { result["error"]="invalid_signature"; return result; }
		std::unique_ptr<EVP_PKEY,UcePkeyDeleter> key=uce_es256_public_key(x,y); std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free); if(!key||!ctx||EVP_DigestVerifyInit(ctx.get(),0,EVP_sha256(),0,key.get())<=0) { result["error"]="operation_failed"; return result; } int verified=EVP_DigestVerify(ctx.get(),(const unsigned char*)der.data(),der.size(),(const unsigned char*)message.data(),message.size()); if(verified<0) { result["error"]="operation_failed"; return result; } result["ok"].set_bool(true); result["operation"]=operation; result["algorithm"]=algorithm; result["valid"].set_bool(verified==1); return result;
	}
	if(operation == "jwt_sign")
	{
		String jwt = uce_es256_jwt(request["private_jwk"], request["protected_header"], request["claims"]);
		if(jwt == "")
		{
			result["error"] = "invalid_key_or_payload";
			return(result);
		}
		result["ok"].set_bool(true);
		result["operation"] = operation;
		result["algorithm"] = algorithm;
		result["jwt"] = jwt;
		return(result);
	}
	result["error"] = "unsupported_operation";
	return(result);
}

#endif
