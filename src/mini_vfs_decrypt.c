/*
 * mini_vfs_decrypt.c — ChaCha20-Poly1305 AEAD (RFC 8439) in pure C99, plus
 * the VFS bundle decrypt/encrypt glue.
 *
 * No external crypto. The ChaCha20 stream cipher, the Poly1305 MAC and the
 * AEAD construction are all implemented here and are SELF-VERIFIED against
 * the official RFC 8439 test vectors (see mini_vfs_selftest at the bottom,
 * compiled in when -DVFS_SELFTEST is defined).
 *
 * Poly1305 uses the 5x26-bit limb representation (poly1305-donna style).
 * The high "1" padding bit after each message block is placed generically
 * by computing limb = bit/26, off = bit%26, which handles both full and
 * final partial blocks without special cases.
 */
#include "mini_vfs.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* little-endian I/O                                                   */
/* ------------------------------------------------------------------ */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void le32s(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
#define ROTL32(x,c) (((x)<<(c)) | ((x)>>(32-(c))))

/* ================================================================== */
/* ChaCha20                                                            */
/* ================================================================== */
static void chacha_qr(uint32_t *s, int a, int b, int c, int d) {
    s[a]+=s[b]; s[d]=ROTL32(s[d]^s[a],16);
    s[c]+=s[d]; s[b]=ROTL32(s[b]^s[c],12);
    s[a]+=s[b]; s[d]=ROTL32(s[d]^s[a], 8);
    s[c]+=s[d]; s[b]=ROTL32(s[b]^s[c], 7);
}

/* Generate the 64-byte keystream block for (key, counter, nonce). */
static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16];
    s[0]=0x61707865; s[1]=0x3320646e; s[2]=0x79622d32; s[3]=0x6b206574;
    for (int i=0;i<8;i++) s[4+i]=le32(key+4*i);
    s[12]=counter;
    s[13]=le32(nonce+0); s[14]=le32(nonce+4); s[15]=le32(nonce+8);

    uint32_t x[16]; memcpy(x, s, sizeof x);
    for (int i=0;i<10;i++) {            /* 20 rounds = 10 double-rounds */
        chacha_qr(x,0,4,8,12); chacha_qr(x,1,5,9,13);
        chacha_qr(x,2,6,10,14); chacha_qr(x,3,7,11,15);  /* column */
        chacha_qr(x,0,5,10,15); chacha_qr(x,1,6,11,12);
        chacha_qr(x,2,7,8,13); chacha_qr(x,3,4,9,14);    /* diagonal */
    }
    for (int i=0;i<16;i++) le32s(out+4*i, x[i]+s[i]);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12],
                         const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t blk[64];
    while (n) {
        chacha20_block(key, counter++, nonce, blk);
        size_t m = n<64 ? n : 64;
        for (size_t i=0;i<m;i++) out[i] = (uint8_t)(in[i] ^ blk[i]);
        in+=m; out+=m; n-=m;
    }
}

/* ================================================================== */
/* Poly1305  (5x26-bit limbs)                                          */
/* ================================================================== */
static void poly1305_mac(uint8_t mac[16], const uint8_t key[32],
                         const uint8_t *msg, size_t len) {
    uint32_t r0,r1,r2,r3,r4, s1,s2,s3,s4;
    uint32_t h0=0,h1=0,h2=0,h3=0,h4=0;

    /* clamp r */
    r0 =  le32(key+0)      & 0x03ffffff;
    r1 = (le32(key+3) >> 2) & 0x03ffff03;
    r2 = (le32(key+6) >> 4) & 0x03ffc0ff;
    r3 = (le32(key+9) >> 6) & 0x03f03fff;
    r4 = (le32(key+12)>> 8) & 0x00fffff;
    s1=r1*5; s2=r2*5; s3=r3*5; s4=r4*5;

    const uint8_t *p = msg;
    size_t left = len;
    uint8_t buf[16];

    while (left > 0) {
        size_t bs = left>=16 ? 16 : left;
        memset(buf, 0, 16);
        memcpy(buf, p, bs);

        uint32_t t0=le32(buf+0), t1=le32(buf+4), t2=le32(buf+8), t3=le32(buf+12);
        uint32_t nn[5];
        nn[0] =  t0                 & 0x3ffffff;
        nn[1] = ((t0>>26)|(t1<<6))  & 0x3ffffff;
        nn[2] = ((t1>>20)|(t2<<12)) & 0x3ffffff;
        nn[3] = ((t2>>14)|(t3<<18)) & 0x3ffffff;
        nn[4] =  (t3>>8);

        /* append the 1-bit after `bs` bytes of data (generic placement:
           limb = bit/26, off = bit%26 — handles full & partial alike) */
        int hb = (int)(8*bs);
        nn[hb/26] |= (1u << (hb%26));

        /* accumulate */
        h0 += nn[0]; h1 += nn[1]; h2 += nn[2]; h3 += nn[3]; h4 += nn[4];

        /* h = h * r mod (2^130-5), using r*5 to fold high limb */
        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint64_t c;
        c = d0>>26; d0&=0x3ffffff; d1+=c;
        c = d1>>26; d1&=0x3ffffff; d2+=c;
        c = d2>>26; d2&=0x3ffffff; d3+=c;
        c = d3>>26; d3&=0x3ffffff; d4+=c;
        c = d4>>26; d4&=0x3ffffff;

        h0 = (uint32_t)d0 + (uint32_t)c*5; c = h0>>26; h0&=0x3ffffff;
        h1 = (uint32_t)d1 + (uint32_t)c;    c = h1>>26; h1&=0x3ffffff;
        h2 = (uint32_t)d2 + (uint32_t)c;    c = h2>>26; h2&=0x3ffffff;
        h3 = (uint32_t)d3 + (uint32_t)c;    c = h3>>26; h3&=0x3ffffff;
        h4 = (uint32_t)d4 + (uint32_t)c;    c = h4>>26; h4&=0x3ffffff;
        h0 += (uint32_t)c*5;                 c = h0>>26; h0&=0x3ffffff; h1 += (uint32_t)c;

        p += bs; left -= bs;
    }

    /* fully carry h — TWO passes: the multiply's final `h1 += c` can leave
       h1 one bit over 2^26, which one pass would propagate into h2..h0 but
       leave h1 over again. The second pass fully reduces. (Matches
       poly1305-donna-32.) This was the AEAD §2.8.2 bug. */
    uint32_t c;
    c = h1>>26; h1&=0x3ffffff; h2+=c;
    c = h2>>26; h2&=0x3ffffff; h3+=c;
    c = h3>>26; h3&=0x3ffffff; h4+=c;
    c = h4>>26; h4&=0x3ffffff; h0+=c*5; c = h0>>26; h0&=0x3ffffff; h1+=c;
    c = h1>>26; h1&=0x3ffffff; h2+=c;
    c = h2>>26; h2&=0x3ffffff; h3+=c;
    c = h3>>26; h3&=0x3ffffff; h4+=c;
    c = h4>>26; h4&=0x3ffffff; h0+=c*5; c = h0>>26; h0&=0x3ffffff; h1+=c;

    /* h + (-p) ; select if h >= p */
    uint32_t g0 = h0+5;          c=g0>>26; g0&=0x3ffffff;
    uint32_t g1 = h1+c;          c=g1>>26; g1&=0x3ffffff;
    uint32_t g2 = h2+c;          c=g2>>26; g2&=0x3ffffff;
    uint32_t g3 = h3+c;          c=g3>>26; g3&=0x3ffffff;
    uint32_t g4 = h4+c - (1u<<26);
    uint32_t mask = (g4>>31) - 1;     /* 0xffffffff if h>=p, else 0 */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    /* repack 5x26 -> 4x32 with 64-bit accumulators so a leftover carry in
       h1 (the final `h1 += c` can leave h1 = 2^26) is NOT truncated — a
       uint32 `h1<<26` would drop bit 52. This was the §2.8.2 AEAD bug:
       §2.5.2's small accumulator never overflowed h1, so it passed. */
    uint32_t w[4];
    uint64_t acc;
    acc = (uint64_t)h0 | ((uint64_t)h1 << 26);    w[0] = (uint32_t)acc; acc >>= 32;
    acc |= (uint64_t)h2 << 20;                    w[1] = (uint32_t)acc; acc >>= 32;
    acc |= (uint64_t)h3 << 14;                    w[2] = (uint32_t)acc; acc >>= 32;
    acc |= (uint64_t)h4 << 8;                     w[3] = (uint32_t)acc;        /* bits 128+ dropped */
    /* add pad s, output 16 bytes (mod 2^128) */
    uint32_t p0=le32(key+16), p1=le32(key+20), p2=le32(key+24), p3=le32(key+28);
    uint64_t f = (uint64_t)w[0] + p0;             w[0]=(uint32_t)f;
    f = (uint64_t)w[1] + p1 + (f>>32);            w[1]=(uint32_t)f;
    f = (uint64_t)w[2] + p2 + (f>>32);            w[2]=(uint32_t)f;
    f = (uint64_t)w[3] + p3 + (f>>32);            w[3]=(uint32_t)f;
    le32s(mac+0,w[0]); le32s(mac+4,w[1]); le32s(mac+8,w[2]); le32s(mac+12,w[3]);
}

/* ================================================================== */
/* AEAD construction (RFC 8439 §2.8)                                   */
/* ================================================================== */
static void pad16(const uint8_t *d, size_t n, uint8_t **cur) {
    memcpy(*cur, d, n); *cur += n;
    size_t rem = n % 16;
    if (rem) { size_t pad = 16 - rem; memset(*cur, 0, pad); *cur += pad; }
}

/* Seal: derive poly one-time key from ChaCha20 block(counter=0); encrypt
   with counter=1; MAC over pad16(aad)||pad16(ct)||le64(len_aad)||le64(len_ct).
   The trailing 8-byte length fields are MANDATORY (RFC 8439 §2.8.1) —
   omitting them was the bug: tag came out 27e6daaa instead of 1ae10b59. */
static void aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, size_t in_len,
                      uint8_t *ct, uint8_t tag[16]) {
    uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);          /* poly one-time key */
    chacha20_xor(key, 1, nonce, in, ct, in_len);    /* ciphertext */
    size_t mlen = ((aad_len+15)/16)*16 + ((in_len+15)/16)*16 + 16;
    uint8_t *m = (uint8_t*)malloc(mlen ? mlen : 1), *cur = m;
    pad16(aad, aad_len, &cur);
    pad16(ct, in_len, &cur);
    uint64_t la = aad_len, lc = in_len;
    for (int i=0;i<8;i++) *cur++ = (uint8_t)(la >> (8*i));
    for (int i=0;i<8;i++) *cur++ = (uint8_t)(lc >> (8*i));
    poly1305_mac(tag, block0, m, (size_t)(cur - m));
    /* wipe the poly key from RAM ASAP */
    memset(block0, 0, sizeof block0);
    free(m);
}

static int aead_open(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, size_t ct_len,
                     uint8_t *pt, const uint8_t tag[16]) {
    uint8_t block0[64], tag2[16];
    chacha20_block(key, 0, nonce, block0);
    size_t mlen = ((aad_len+15)/16)*16 + ((ct_len+15)/16)*16 + 16;
    uint8_t *m = (uint8_t*)malloc(mlen ? mlen : 1), *cur = m;
    pad16(aad, aad_len, &cur);
    pad16(ct, ct_len, &cur);
    uint64_t la = aad_len, lc = ct_len;
    for (int i=0;i<8;i++) *cur++ = (uint8_t)(la >> (8*i));
    for (int i=0;i<8;i++) *cur++ = (uint8_t)(lc >> (8*i));
    poly1305_mac(tag2, block0, m, (size_t)(cur - m));
    memset(block0, 0, sizeof block0);
    free(m);
    /* constant-time tag compare */
    uint8_t diff = 0;
    for (int i=0;i<16;i++) diff |= (uint8_t)(tag[i] ^ tag2[i]);
    if (diff) return -1;                       /* authentication failed */
    chacha20_xor(key, 1, nonce, ct, pt, ct_len);
    return 0;
}

/* ================================================================== */
/* VFS bundle glue                                                    */
/* ================================================================== */
int mini_vfs_encrypt(const uint8_t *in, size_t in_len,
                     const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     uint8_t **out, size_t *out_len) {
    if (!in || !key || !nonce) return -1;
    size_t total = 12 + in_len + 16;
    uint8_t *buf = (uint8_t*)malloc(total);
    if (!buf) return -1;
    memcpy(buf, nonce, 12);
    aead_seal(key, nonce, aad, aad_len, in, in_len, buf+12, buf+12+in_len);
    *out = buf; *out_len = total;
    return 0;
}

int mini_vfs_decrypt(const uint8_t *bundle, size_t bundle_size,
                     const uint8_t key[32],
                     const uint8_t *aad, size_t aad_len,
                     uint8_t **out, size_t *out_len) {
    if (!bundle || !key) return -1;
    if (bundle_size < 12 + 16) return -1;       /* need nonce + tag at least */
    size_t ct_len = bundle_size - 12 - 16;
    const uint8_t *nonce = bundle;
    const uint8_t *ct    = bundle + 12;
    const uint8_t *tag   = bundle + 12 + ct_len;
    uint8_t *pt = (uint8_t*)malloc(ct_len ? ct_len : 1);
    if (!pt) return -1;
    if (aead_open(key, nonce, aad, aad_len, ct, ct_len, pt, tag) != 0) {
        free(pt);
        return -2;                              /* tag mismatch / tampered */
    }
    *out = pt; *out_len = ct_len;
    return 0;
}

/* ================================================================== */
/* Self-test against RFC 8439 §2.8.2 (compile: gcc ... -DVFS_SELFTEST) */
/* ================================================================== */
#ifdef VFS_SELFTEST
#include <stdio.h>
static void hex(const char *lbl, const uint8_t *b, size_t n){
    printf("  %s: ", lbl); for (size_t i=0;i<n;i++) printf("%02x", b[i]); printf("\n");
}
int main(void){
    int fails = 0;

    /* ---- Isolate ChaCha20: RFC 8439 §2.3.2 keystream (64 zero bytes) ---- */
    {
        uint8_t k2[32]; for (int i=0;i<32;i++) k2[i]=(uint8_t)i;
        uint8_t n2[12] = {0,0,0,9, 0,0,0,0x4a, 0,0,0,0};
        uint8_t zero[64]; memset(zero,0,64);
        uint8_t c2[64];
        chacha20_xor(k2, 1, n2, zero, c2, 64);          /* keystream block 1 */
        static const uint8_t e2[16] = {0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,
                                       0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4};
        hex("chacha20 §2.3.2 ks16", c2, 16);
        if (memcmp(c2,e2,16)==0) printf("[PASS] ChaCha20 keystream (§2.3.2)\n");
        else { printf("[FAIL] ChaCha20 keystream (§2.3.2)\n"); fails++; }
    }

    /* ---- Isolate Poly1305: RFC 8439 §2.5.2 ---- */
    {
        uint8_t pk[32] = {0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,
                          0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
                          0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,
                          0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b};
        const char *pmsg = "Cryptographic Forum Research Group";   /* 34 bytes */
        uint8_t mt[16];
        poly1305_mac(mt, pk, (const uint8_t*)pmsg, strlen(pmsg));
        static const uint8_t e3[16] = {0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,
                                       0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9};
        hex("poly1305 §2.5.2 tag", mt, 16);
        if (memcmp(mt,e3,16)==0) printf("[PASS] Poly1305 (§2.5.2)\n");
        else { printf("[FAIL] Poly1305 (§2.5.2)\n"); fails++; }
    }

    /* ---- AEAD composition: RFC 8439 §2.8.2 ---- */
    {
    uint8_t key[32]; for (int i=0;i<32;i++) key[i]=(uint8_t)(0x80+i);
    uint8_t nonce[12] = {0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    uint8_t aad[12]   = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
    const char *ptxt = "Ladies and Gentlemen of the class of '99: If I could "
                       "offer you only one tip for the future, sunscreen "
                       "would be it.";
    size_t plen = strlen(ptxt);
    uint8_t ct[256], tag[16];
    aead_seal(key, nonce, aad, 12, (const uint8_t*)ptxt, plen, ct, tag);
    hex("ct16", ct, 16);                 /* matches authoritative d31a8d34... */
    hex("tag", tag, 16);                 /* matches authoritative 1ae10b59... */

    static const uint8_t exp_tag[16]  = {0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,
                                         0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};
    static const uint8_t exp_ct16[16] = {0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,
                                         0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2};

    if (memcmp(tag, exp_tag, 16)!=0) { printf("[FAIL] AEAD tag\n"); hex("got",tag,16); fails++; }
    else if (memcmp(ct, exp_ct16, 16)!=0) { printf("[FAIL] AEAD ciphertext\n"); hex("got",ct,16); fails++; }
    else printf("[PASS] AEAD seal vs RFC 8439 2.8.2 (tag + ct)\n");

    uint8_t pt[256];
    if (aead_open(key,nonce,aad,12,ct,plen,pt,tag)!=0 || memcmp(pt,ptxt,plen)!=0) {
        printf("[FAIL] AEAD roundtrip\n"); fails++;
    } else printf("[PASS] AEAD roundtrip\n");

    ct[0] ^= 0xff;
    if (aead_open(key,nonce,aad,12,ct,plen,pt,tag)==0) { printf("[FAIL] tamper undetected\n"); fails++; }
    else printf("[PASS] AEAD tamper rejected\n");

    /* VFS bundle layer */
    uint8_t *bundle=0; size_t blen=0;
    mini_vfs_encrypt((const uint8_t*)ptxt, plen, key, nonce, aad, 12, &bundle, &blen);
    uint8_t *dec=0; size_t dlen=0;
    if (mini_vfs_decrypt(bundle, blen, key, aad, 12, &dec, &dlen)!=0 || dlen!=plen || memcmp(dec,ptxt,plen)!=0) {
        printf("[FAIL] VFS roundtrip\n"); fails++;
    } else printf("[PASS] VFS bundle roundtrip\n");
    free(bundle); free(dec);

    printf(fails ? "VFS_SELFTEST: %d FAIL\n" : "VFS_SELFTEST: all PASS\n", fails);
    return fails ? 1 : 0;
    }
}
#endif
