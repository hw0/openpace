/*
 * Copyright (c) 2010-2012 Frank Morgner and Dominik Oepen
 *
 * This file is part of OpenPACE.
 *
 * OpenPACE is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * OpenPACE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file eac_util.c
 * @brief Utility functions
 *
 * @author Frank Morgner <morgner@informatik.hu-berlin.de>
 * @author Dominik Oepen <oepen@informatik.hu-berlin.de>
 */

#include "eac_asn1.h"
#include "eac_dh.h"
#include "eac_ecdh.h"
#include "eac_err.h"
#include "eac_util.h"
#include "misc.h"
#include <eac/eac.h>
#include <openssl/bio.h>
#include <openssl/dh.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

/**
 * @brief Wrapper to the OpenSSL encryption functions.
 *
 * @param ctx (optional)
 * @param type
 * @param impl (optional)
 * @param key
 * @param iv (optional)
 * @param enc
 * @param in
 *
 * @return cipher of in or NULL if an error occurred
 */
static BUF_MEM *
cipher(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, ENGINE *impl,
        const unsigned char *key, const unsigned char *iv, int enc, const BUF_MEM * in);
static EVP_PKEY *
EVP_PKEY_from_pubkey(EVP_PKEY *key, const BUF_MEM *pub, BN_CTX *bn_ctx);
/**
 * @brief Computes the authentication token over the other parties public key
 * using the MAC key derived during PACE
 *
 */
static BUF_MEM *
compute_authentication_token(int protocol, const KA_CTX *ka_ctx, EVP_PKEY *opp_key,
        BN_CTX *bn_ctx, enum eac_tr_version tr_version);
/**
 * @brief Convert an ECDSA signature from plain format to X9.62 format
 *
 * @param[in] plain_sig signature in plain format
 *
 * @return signature in X9.62 format or NULL in case of an error */
static BUF_MEM *
convert_from_plain_sig(const BUF_MEM *plain_sig);
/**
 * @brief OpenSSL uses the X9.62 signature format, we have to convert it to the
 * plain format format specified in BSI TR 03111
 *
 * @param x962_sig X9.62 formatted signature
 *
 * @return plain format signature or NULL in case of an error
 */
static BUF_MEM *
convert_to_plain_sig(const BUF_MEM *x962_sig);

BUF_MEM *
hash(const EVP_MD * md, EVP_MD_CTX * ctx, ENGINE * impl, const BUF_MEM * in)
{
    BUF_MEM * out = NULL;
    EVP_MD_CTX * tmp_ctx = NULL;
    unsigned int tmp_len;

    check((md && in), "Invalid arguments");

    if (ctx)
        tmp_ctx = ctx;
    else {
        tmp_ctx = EVP_MD_CTX_create();
        if (!tmp_ctx)
            goto err;
    }

    tmp_len = EVP_MD_size(md);
    out = BUF_MEM_create(tmp_len);
    if (!out || !EVP_DigestInit_ex(tmp_ctx, md, impl) ||
            !EVP_DigestUpdate(tmp_ctx, in->data, in->length) ||
            !EVP_DigestFinal_ex(tmp_ctx, (unsigned char *) out->data,
                &tmp_len))
        goto err;
    out->length = tmp_len;

    if (!ctx)
        EVP_MD_CTX_destroy(tmp_ctx);

    return out;

err:
    if (out)
        BUF_MEM_free(out);
    if (tmp_ctx && !ctx)
        EVP_MD_CTX_destroy(tmp_ctx);

    return NULL;
}

static BUF_MEM *
cipher(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, ENGINE *impl,
        const unsigned char *key, const unsigned char *iv, int enc, const BUF_MEM * in)
{
    BUF_MEM * out = NULL;
    EVP_CIPHER_CTX * tmp_ctx = NULL;
    int i;

    check(in, "Invalid arguments");

    if (ctx)
        tmp_ctx = ctx;
    else {
        tmp_ctx = EVP_CIPHER_CTX_new();
        if (!tmp_ctx)
            goto err;
        EVP_CIPHER_CTX_init(tmp_ctx);
        if (!EVP_CipherInit_ex(tmp_ctx, type, impl, key, iv, enc))
            goto err;
    }

    if (tmp_ctx->flags & EVP_CIPH_NO_PADDING) {
        i = in->length;
        check((in->length % EVP_CIPHER_block_size(type) == 0), "Data is not of blocklength");
    } else
        i = in->length + EVP_CIPHER_block_size(type);

    out = BUF_MEM_create(i);
    if (!out)
        goto err;

    if (!EVP_CipherUpdate(tmp_ctx, (unsigned char *) out->data, &i,
            (unsigned char *) in->data, in->length))
        goto err;
    out->length = i;

    if (!EVP_CipherFinal_ex(tmp_ctx, (unsigned char *) (out->data + out->length),
            &i))
            goto err;

    if (!(tmp_ctx->flags & EVP_CIPH_NO_PADDING))
        out->length += i;

    if (!ctx)
        EVP_CIPHER_CTX_free(tmp_ctx);

    return out;

err:

    if (out)
        BUF_MEM_free(out);
    if (!ctx && tmp_ctx)
        EVP_CIPHER_CTX_free(tmp_ctx);

    return NULL;
}

BUF_MEM *
cipher_no_pad(KA_CTX *ctx, EVP_CIPHER_CTX *cipher_ctx, const BUF_MEM *key_enc, const BUF_MEM *data, int enc)
{
    BUF_MEM *out = NULL;
    EVP_CIPHER_CTX *tmp_ctx = NULL;

    check(ctx, "Invalid arguments");

    if (cipher_ctx)
        tmp_ctx = cipher_ctx;
    else {
        tmp_ctx = EVP_CIPHER_CTX_new();
        if (!tmp_ctx)
            goto err;
        EVP_CIPHER_CTX_init(tmp_ctx);
    }

    if (!EVP_CipherInit_ex(tmp_ctx, ctx->cipher, ctx->cipher_engine,
                (unsigned char *)key_enc->data, ctx->iv, enc)
            || !EVP_CIPHER_CTX_set_padding(tmp_ctx, 0))
        goto err;

    out = cipher(tmp_ctx, ctx->cipher, ctx->cipher_engine,
            (unsigned char *)key_enc->data, ctx->iv, enc, data);

err:
    if (!cipher_ctx && tmp_ctx)
        EVP_CIPHER_CTX_free(tmp_ctx);

    return out;
}

BUF_MEM *
cmac(CMAC_CTX *ctx, const EVP_CIPHER *type, const BUF_MEM * key,
        const BUF_MEM * in, size_t maclen)
{
    CMAC_CTX * cmac_ctx = NULL;
    BUF_MEM * out = NULL, * tmp = NULL;
    size_t cmac_len = 0;

    check((key && in && type), "Invalid arguments");

    check((key->length >= (size_t) EVP_CIPHER_key_length(type)),
            "Key is too short");

    if (ctx)
        cmac_ctx = ctx;
    else {
        cmac_ctx = CMAC_CTX_new();
    }

    /* Initialize the CMAC context, feed in the data, and get the required
     * output buffer size */
    if (!cmac_ctx ||
            !CMAC_Init(cmac_ctx, key->data, EVP_CIPHER_key_length(type),
                type, NULL) ||
            !CMAC_Update(cmac_ctx, in->data, in->length) ||
            !CMAC_Final(cmac_ctx, NULL, &cmac_len))
        goto err;

    /* get buffer in required size */
    out = BUF_MEM_create(cmac_len);
    if (!out)
        goto err;

    /* get the actual CMAC */
    if (!CMAC_Final(cmac_ctx, (unsigned char*) out->data, &out->length))
        goto err;

    /* Truncate the CMAC if necessary */
    if (cmac_len > maclen) {
        tmp = BUF_MEM_create_init(out->data, maclen);
        BUF_MEM_free(out);
        out = tmp;
    }

    if (!ctx)
        CMAC_CTX_free(cmac_ctx);

    return out;

err:
    if (cmac_ctx && !ctx) {
        CMAC_CTX_free(cmac_ctx);
    }
    if (out) {
        BUF_MEM_free(out);
    }

    return NULL;
}

BUF_MEM *
randb(int numbytes)
{
    BUF_MEM * r = BUF_MEM_new();
    if (!r || !BUF_MEM_grow(r, numbytes) ||
            !RAND_bytes((unsigned char *) r->data, numbytes))
        goto err;

    return r;

err:
    if (r)
        BUF_MEM_free(r);

    return NULL;
}

BUF_MEM *
retail_mac_des(const BUF_MEM * key, const BUF_MEM * in)
{
    /* ISO 9797-1 algorithm 3 retail mac without any padding */
    BUF_MEM * c_tmp = NULL, *d_tmp = NULL, *mac = NULL, *block = NULL;
    EVP_CIPHER_CTX * ctx = NULL;
    size_t len;

    check(key, "Invalid arguments");

    /* Flawfinder: ignore */
    len = EVP_CIPHER_block_size(EVP_des_cbc());
    check(key->length >= 2*len, "Key too short");

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        goto err;
    EVP_CIPHER_CTX_init(ctx);
    /* Flawfinder: ignore */
    if (!EVP_CipherInit_ex(ctx, EVP_des_cbc(), NULL,
            (unsigned char *) key->data, NULL, 1) ||
            !EVP_CIPHER_CTX_set_padding(ctx, 0))
        goto err;

    /* get last block of des_cbc encrypted input */
    /* Flawfinder: ignore */
    c_tmp = cipher(ctx, EVP_des_cbc(), NULL, NULL, NULL, 1, in);
    if (!c_tmp)
        goto err;
    block = BUF_MEM_create_init(c_tmp->data + c_tmp->length - len, len);

    /* decrypt last block with the rest of the key */
    /* IV is always NULL */
    /* Flawfinder: ignore */
    if (!block || !EVP_CipherInit_ex(ctx, EVP_des_cbc(), NULL,
            (unsigned char *) key->data + len, NULL, 0) ||
            !EVP_CIPHER_CTX_set_padding(ctx, 0))
        goto err;
    /* Flawfinder: ignore */
    d_tmp = cipher(ctx, EVP_des_cbc(), NULL, NULL, NULL, 0, block);

    /* encrypt last block with the first key */
    /* IV is always NULL */
    /* Flawfinder: ignore */
    if (!d_tmp || !EVP_CipherInit_ex(ctx, EVP_des_cbc(), NULL,
            (unsigned char *) key->data, NULL, 1) ||
            !EVP_CIPHER_CTX_set_padding(ctx, 0))
        goto err;
    /* Flawfinder: ignore */
    mac = cipher(ctx, EVP_des_cbc(), NULL, NULL, NULL, 1, d_tmp);

    BUF_MEM_free(block);
    BUF_MEM_free(c_tmp);
    BUF_MEM_free(d_tmp);
    EVP_CIPHER_CTX_free(ctx);

    return mac;

err:
    if (block)
        BUF_MEM_free(block);
    if (c_tmp)
        BUF_MEM_free(c_tmp);
    if (d_tmp)
        BUF_MEM_free(d_tmp);
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    return NULL;
}

BUF_MEM *
add_iso_pad(const BUF_MEM * m, int block_size)
{
    BUF_MEM * out = NULL;
    int p_len;

    check(m, "Invalid arguments");

    /* calculate length of padded message */
    p_len = (m->length / block_size) * block_size + block_size;

    out = BUF_MEM_create(p_len);
    if (!out)
        goto err;

    /* Flawfinder: ignore */
    memcpy(out->data, m->data, m->length);

    /* now add iso padding */
    memset(out->data + m->length, 0x80, 1);
    memset(out->data + m->length + 1, 0, p_len - m->length - 1);

    return out;

err:
    if (out)
        BUF_MEM_free(out);

    return NULL;
}

int encode_ssc(const BIGNUM *ssc, const KA_CTX *ctx, unsigned char **encoded)
{
    unsigned char *p;
    size_t en_len, bn_len;

    if (!ctx || !encoded) {
        log_err("Invalid arguments");
        return -1;
    }

    en_len = EVP_CIPHER_block_size(ctx->cipher);
    p = OPENSSL_realloc(*encoded, en_len);
    if (!p) {
        log_err("Realloc failure");
        return -1;
    }
    *encoded = p;

    bn_len = BN_num_bytes(ssc);

    if (bn_len <= en_len) {
        memset(*encoded, 0, en_len - bn_len);
        BN_bn2bin(ssc, *encoded + en_len - bn_len);
    } else {
        p = OPENSSL_malloc(bn_len);
        if (!p)
            return -1;
        BN_bn2bin(ssc, p);
        /* Flawfinder: ignore */
        memcpy(*encoded, p + bn_len - en_len, en_len);
        OPENSSL_free(p);
    }

    return en_len;
}

int update_iv(KA_CTX *ctx, EVP_CIPHER_CTX *cipher_ctx, const BIGNUM *ssc)
{
    BUF_MEM *sscbuf = NULL, *ivbuf = NULL;
    const EVP_CIPHER *ivcipher = NULL, *oldcipher;
    unsigned char *ssc_buf = NULL;
    unsigned char *p;
    int r = 0;

    check(ctx, "Invalid arguments");

    switch (EVP_CIPHER_nid(ctx->cipher)) {
        case NID_aes_128_cbc:
            if (!ivcipher)
                ivcipher = EVP_aes_128_ecb();
            /* fall through */
        case NID_aes_192_cbc:
            if (!ivcipher)
                ivcipher = EVP_aes_192_ecb();
            /* fall through */
        case NID_aes_256_cbc:
            if (!ivcipher)
                ivcipher = EVP_aes_256_ecb();

            /* For AES decryption the IV is not needed,
             * so we always set it to the encryption IV=E(K_Enc, SSC) */
            r = encode_ssc(ssc, ctx, &ssc_buf);
            if (r < 0)
                goto err;
            sscbuf = BUF_MEM_create_init(ssc_buf, r);
            if (!sscbuf)
                goto err;
            oldcipher = ctx->cipher;
            ctx->cipher = ivcipher;
            ivbuf = cipher_no_pad(ctx, cipher_ctx, ctx->k_enc, sscbuf, 1);
            ctx->cipher = oldcipher;
            if (!ivbuf)
                goto err;
            p = OPENSSL_realloc(ctx->iv, ivbuf->length);
            if (!p)
                goto err;
            ctx->iv = p;
            /* Flawfinder: ignore */
            memcpy(ctx->iv, ivbuf->data, ivbuf->length);
            break;
        case NID_des_ede_cbc:
            /* For 3DES encryption or decryption the IV is always NULL */
            if (ctx->iv)
                free(ctx->iv);
            ctx->iv = NULL;
            break;
        default:
            log_err("Unknown cipher");
            goto err;
    }

    r = 1;

err:
    if (ssc_buf)
        free(ssc_buf);
    if (sscbuf)
        BUF_MEM_free(sscbuf);
    if (ivbuf)
        BUF_MEM_free(ivbuf);

    return r;
}

int
is_char_str(const BUF_MEM * str)
{
    const unsigned char *s = NULL;
    size_t i = 0;

    if (!str)
        return 0;

    s = (unsigned char *) str->data;
    i = str->length;

    while (i) {
        if (*s <= 0x1f || (0x7f <= *s && *s <= 0x9f)) {
            log_err("Invalid data");
            return 0;
        }
        s++;
        i--;
    }

    return 1;
}

int
is_bcd(const unsigned char *data, size_t length)
{
    size_t i;

    if (!data)
        return 0;

    for(i = 0; i < length; i++) {
        if (data[i] > 0x9)
            return 0;
    }
    return 1;
}

BUF_MEM *
authenticate(const KA_CTX *ctx, const BUF_MEM *data)
{
    switch (EVP_CIPHER_nid(ctx->cipher)) {
        case NID_des_ede_cbc:
            return retail_mac_des(ctx->k_mac, data);
        case NID_aes_128_cbc:
        case NID_aes_192_cbc:
        case NID_aes_256_cbc:
            return cmac(ctx->cmac_ctx, ctx->cipher, ctx->k_mac, data,
                    EAC_AES_MAC_LENGTH);
        default:
            log_err("Unknown cipher");
            return NULL;
    }
}

EVP_PKEY *
EVP_PKEY_dup(EVP_PKEY *key)
{
    EVP_PKEY *out = NULL;
    DH *dh_in = NULL, *dh_out = NULL;
    EC_KEY *ec_in = NULL, *ec_out = NULL;
    RSA *rsa_in = NULL, *rsa_out = NULL;

    check(key, "Invalid arguments");

    out = EVP_PKEY_new();

    if (!out)
        goto err;

    switch (EVP_PKEY_type(key->type)) {
        case EVP_PKEY_DH:
            dh_in = EVP_PKEY_get1_DH(key);
            if (!dh_in)
                goto err;

            dh_out = DHparams_dup_with_q(dh_in);
            if (!dh_out)
                goto err;

            EVP_PKEY_set1_DH(out, dh_out);
            DH_free(dh_out);
            DH_free(dh_in);
            break;

        case EVP_PKEY_EC:
            ec_in = EVP_PKEY_get1_EC_KEY(key);
            if (!ec_in)
                goto err;

            ec_out = EC_KEY_dup(ec_in);
            if (!ec_out)
                goto err;

            EVP_PKEY_set1_EC_KEY(out, ec_out);
            EC_KEY_free(ec_out);
            EC_KEY_free(ec_in);
            break;

        case EVP_PKEY_RSA:
            rsa_in = EVP_PKEY_get1_RSA(key);
            if (!rsa_in)
                goto err;

            rsa_out = RSAPrivateKey_dup(rsa_in);
            if (!rsa_out)
                goto err;

            EVP_PKEY_set1_RSA(out, rsa_out);
            RSA_free(rsa_out);
            RSA_free(rsa_in);
            break;

        default:
            log_err("Unknown protocol");
            goto err;
    }

    return out;

err:
    if (dh_in)
        DH_free(dh_in);
    if (dh_out)
        DH_free(dh_out);
    if (ec_in)
        EC_KEY_free(ec_in);
    if (ec_out)
        EC_KEY_free(ec_out);
    if (rsa_out)
        RSA_free(rsa_out);
    if (rsa_in)
        RSA_free(rsa_in);
    if (out)
        EVP_PKEY_free(out);
    return NULL;
}

int
EVP_PKEY_set_pubkey(EVP_PKEY *key, const BUF_MEM *pub, BN_CTX *bn_ctx)
{
    DH *dh = NULL;
    EC_KEY *ec = NULL;
    EC_POINT *ecp = NULL;

    check(key, "Invalid arguments");

    switch (EVP_PKEY_type(key->type)) {
        case EVP_PKEY_DH:
            dh = EVP_PKEY_get1_DH(key);
            if (!dh)
                goto err;

            dh->pub_key = BN_bin2bn((unsigned char *) pub->data, pub->length,
                    dh->pub_key);
            if (!dh->pub_key)
                goto err;
            EVP_PKEY_set1_DH(key, dh);
            DH_free(dh);
            break;
        case EVP_PKEY_EC:
            ec = EVP_PKEY_get1_EC_KEY(key);
            if (!ec)
                goto err;

            ecp = EC_POINT_new(EC_KEY_get0_group(ec));
            if (!ecp || !EC_POINT_oct2point(EC_KEY_get0_group(ec), ecp,
                    (unsigned char *) pub->data, pub->length, bn_ctx)
                     || !EC_KEY_set_public_key(ec, ecp))
                goto err;
            EVP_PKEY_set1_EC_KEY(key, ec);
            EC_POINT_free(ecp);
            EC_KEY_free(ec);
            break;
        default:
            log_err("Unknown protocol");
            goto err;
    }

    return 1;

err:
    if (dh)
        DH_free(dh);
    if (ec)
        EC_KEY_free(ec);
    if (ecp)
        EC_POINT_free(ecp);

    return 0;
}

static EVP_PKEY *
EVP_PKEY_from_pubkey(EVP_PKEY *key, const BUF_MEM *pub, BN_CTX *bn_ctx)
{
    EVP_PKEY *out = NULL;

    out = EVP_PKEY_dup(key);
    if (!out)
        return NULL;

    if (!EVP_PKEY_set_pubkey(out, pub, bn_ctx)) {
        EVP_PKEY_free(out);
        return NULL;
    }

    return out;
}

BUF_MEM *
get_authentication_token(int protocol, const KA_CTX *ka_ctx, BN_CTX *bn_ctx,
                   enum eac_tr_version tr_version, const BUF_MEM *pub_opp)
{
    BUF_MEM *out = NULL;
    EVP_PKEY *opp_key = NULL;

    check(ka_ctx, "Invalid arguments");

    opp_key = EVP_PKEY_from_pubkey(ka_ctx->key, pub_opp,
            bn_ctx);
    if (!opp_key)
        goto err;


    out = compute_authentication_token(protocol, ka_ctx, opp_key,
            bn_ctx, tr_version);

err:
    EVP_PKEY_free(opp_key);

    return out;
}

BUF_MEM *
compute_authentication_token(int protocol, const KA_CTX *ka_ctx, EVP_PKEY *opp_key,
        BN_CTX *bn_ctx, enum eac_tr_version tr_version)
{
    BUF_MEM *asn1 = NULL, *out = NULL, *pad =NULL;

    check(ka_ctx, "Invalid arguments");

    asn1 = asn1_pubkey(protocol, opp_key, bn_ctx, tr_version);

    /* ISO 9797-1 algorithm 3 retail MAC now needs extra padding (padding method 2) */
    if (EVP_CIPHER_nid(ka_ctx->cipher) == NID_des_ede_cbc) {
        pad = add_iso_pad(asn1, EVP_CIPHER_block_size(ka_ctx->cipher));
        if (!pad)
            goto err;
        out = authenticate(ka_ctx, pad);
    } else {
        out = authenticate(ka_ctx, asn1);
    }

err:
    if (asn1)
        BUF_MEM_free(asn1);
    if (pad)
        BUF_MEM_free(pad);

    return out;
}

int
verify_authentication_token(int protocol, const KA_CTX *ka_ctx, BN_CTX *bn_ctx,
                   enum eac_tr_version tr_version, const BUF_MEM *token)
{
    int rv;
    BUF_MEM *token_verify = NULL;

    if (!ka_ctx || !token) {
        log_err("Invalid arguments");
        return -1;
    }

    token_verify = compute_authentication_token(protocol, ka_ctx, ka_ctx->key,
                    bn_ctx, tr_version);
    if (!token_verify)
        return -1;

    if (token_verify->length != token->length
                || consttime_memcmp(token_verify, token) != 0)
        rv = 0;
    else
        rv = 1;

    BUF_MEM_free(token_verify);

    return rv;
}

BUF_MEM *
Comp(EVP_PKEY *key, const BUF_MEM *pub, BN_CTX *bn_ctx, EVP_MD_CTX *md_ctx)
{
    BUF_MEM *out = NULL;
    const EC_GROUP *group;
    EC_POINT *ecp = NULL;
    EC_KEY *ec = NULL;
    BIGNUM *x = NULL, *y = NULL;

    check((key && pub), "Invalid arguments");

    switch (EVP_PKEY_type(key->type)) {
        case EVP_PKEY_DH:
            out = hash(EVP_sha1(), md_ctx, NULL, pub);
            break;
        case EVP_PKEY_EC:
            ec = EVP_PKEY_get1_EC_KEY(key);
            if (!ec)
                goto err;

            group = EC_KEY_get0_group(ec);
            ecp = EC_POINT_new(group);
            x = BN_CTX_get(bn_ctx);
            y = BN_CTX_get(bn_ctx);

            if(!ecp || !x || !y
                    || !EC_POINT_oct2point(group, ecp,
                        (unsigned char *) pub->data, pub->length, bn_ctx)
                    || !EC_POINT_get_affine_coordinates_GF2m(group, ecp, x, y, bn_ctx))
                goto err;

            out = BUF_MEM_create(BN_num_bytes(x));
            if(!out || !BN_bn2bin(x, (unsigned char *) out->data))
                goto err;
            break;
        default:
            log_err("Unknown protocol");
            goto err;
    }

err:
    if (ecp)
        EC_POINT_free(ecp);
    /* Decrease the reference count, the key is still available in the EVP_PKEY
     * structure */
    if (ec)
        EC_KEY_free(ec);
    if (x)
        BN_free(x);
    if (y)
        BN_free(y);

    return out;
}

int EVP_PKEY_set_std_dp(EVP_PKEY *key, int stnd_dp) {

    DH *dh = NULL;
    EC_KEY *ec = NULL;

    if (!key) {
        log_err("Invalid arguments");
        return 0;
    }

    /* Generate key from standardized domain parameters */
    switch(stnd_dp) {
        case 0:
        case 1:
        case 2:
            if (!init_dh(&dh, stnd_dp))
                return 0;
            EVP_PKEY_set1_DH(key, dh);
            /* Decrement reference count */
            DH_free(dh);
            break;
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
            if (!init_ecdh(&ec, stnd_dp))
                return 0;
            EVP_PKEY_set1_EC_KEY(key, ec);
            /* Decrement reference count */
            EC_KEY_free(ec);
            break;
        default:
            log_err("Invalid arguments");
            return 0;
    }

    return 1;
}

void
BUF_MEM_clear_free(BUF_MEM *b)
{
    if (b) {
        OPENSSL_cleanse(b->data, b->max);
        BUF_MEM_free(b);
    }
}

int
EVP_PKEY_set_keys(EVP_PKEY *evp_pkey,
        const unsigned char *privkey, size_t privkey_len,
           const unsigned char *pubkey, size_t pubkey_len,
           BN_CTX *bn_ctx)
{
    EC_KEY *ec_key = NULL;
    DH *dh = NULL;
    EC_POINT *ec_point = NULL;
    BIGNUM *bn = NULL;
    int ok = 0;
    const EC_GROUP *group;

    check(evp_pkey, "Invalid arguments");

    switch (EVP_PKEY_type(evp_pkey->type)) {
        case EVP_PKEY_EC:
            ec_key = EVP_PKEY_get1_EC_KEY(evp_pkey);
            group = EC_KEY_get0_group(ec_key);
            ec_point = EC_POINT_new(group);
            bn = BN_bin2bn(privkey, privkey_len, bn);
            if (!ec_key || !ec_point || !bn
                    || !EC_POINT_oct2point(group, ec_point, pubkey, pubkey_len,
                        bn_ctx)
                    || !EC_KEY_set_public_key(ec_key, ec_point)
                    || !EC_KEY_set_private_key(ec_key, bn)
                    || !EVP_PKEY_set1_EC_KEY(evp_pkey, ec_key))
                goto err;
            break;
        case EVP_PKEY_DH:
            dh = EVP_PKEY_get1_DH(evp_pkey);
            dh->priv_key = BN_bin2bn(privkey, privkey_len, dh->priv_key);
            dh->pub_key = BN_bin2bn(pubkey, pubkey_len, dh->pub_key);
            if (!dh->priv_key || !dh->pub_key
                    || !EVP_PKEY_set1_DH(evp_pkey, dh))
                goto err;
            break;
        default:
            log_err("Unknown protocol");
            goto err;
            break;
    }

    ok = 1;

err:
    if (bn)
        BN_clear_free(bn);
    if (ec_key)
        EC_KEY_free(ec_key);
    if (dh)
        DH_free(dh);
    if (ec_point)
        EC_POINT_clear_free(ec_point);

    return ok;
}

BUF_MEM *
get_pubkey(EVP_PKEY *key, BN_CTX *bn_ctx)
{
    BUF_MEM *out;
    DH *dh;
    EC_KEY *ec;
    const EC_POINT *ec_pub;

    check_return(key, "invalid arguments");

    switch (EVP_PKEY_type(key->type)) {
        case EVP_PKEY_DH:
            dh = EVP_PKEY_get1_DH(key);
            if (!dh)
                return NULL;

            out = BN_bn2buf(dh->pub_key);

            DH_free(dh);
            break;

        case EVP_PKEY_EC:
            ec = EVP_PKEY_get1_EC_KEY(key);
            if (!ec)
                return NULL;

            ec_pub = EC_KEY_get0_public_key(ec);
            if (!ec_pub)
                return NULL;

            out = EC_POINT_point2buf(ec, bn_ctx, ec_pub);

            EC_KEY_free(ec);
            break;

        default:
            return NULL;
    }

    return out;
}

BUF_MEM *
convert_from_plain_sig(const BUF_MEM *plain_sig)
{
    ECDSA_SIG *ecdsa_sig = NULL;
    BUF_MEM *x962_sig = NULL;
    int l;
    unsigned char *p = NULL;

    check(plain_sig, "Invalid arguments");

    check(plain_sig->length%2 == 0, "Invalid data");

    ecdsa_sig = ECDSA_SIG_new();
    if (!ecdsa_sig)
        goto err;

    /* The first l/2 bytes of the plain signature contain the number r, the second
     * l/2 bytes contain the number s. */
    ecdsa_sig->r = BN_bin2bn((unsigned char *) plain_sig->data,
            plain_sig->length/2, ecdsa_sig->r);
    ecdsa_sig->s = BN_bin2bn((unsigned char *) plain_sig->data +
            plain_sig->length/2, plain_sig->length/2, ecdsa_sig->s);

    /* ASN.1 encode the signature*/
    l = i2d_ECDSA_SIG(ecdsa_sig, &p);
    if (l < 0)
        goto err;
    x962_sig = BUF_MEM_create_init(p, l);

err:
    if (p)
        free(p);
    if (ecdsa_sig)
        ECDSA_SIG_free(ecdsa_sig);

    return x962_sig;
}

BUF_MEM *
convert_to_plain_sig(const BUF_MEM *x962_sig)
{
    size_t r_len, s_len, rs_max;
    BUF_MEM *plain_sig_buf = NULL;
    ECDSA_SIG *tmp_sig = NULL;
    const unsigned char *tmp;
    unsigned char *r = NULL, *s = NULL;

    check_return(x962_sig, "Invalid arguments");

    /* Convert the ASN.1 data to a C structure*/
    tmp = (unsigned char*) x962_sig->data;
    tmp_sig = ECDSA_SIG_new();
    if (!tmp_sig)
        goto err;
    if (!d2i_ECDSA_SIG(&tmp_sig, &tmp, x962_sig->length))
        goto err;

    /* Extract the parameters r and s*/
    r_len = BN_num_bytes(tmp_sig->r);
    s_len = BN_num_bytes(tmp_sig->s);
    rs_max = r_len > s_len ? r_len : s_len;
    r = OPENSSL_malloc(rs_max);
    s = OPENSSL_malloc(rs_max);
    if (!r || !s)
        goto err;

    /* Convert r and s to a binary representation */
    if (!BN_bn2bin(tmp_sig->r, r + rs_max - r_len))
        goto err;
    if (!BN_bn2bin(tmp_sig->s, s + rs_max - s_len))
        goto err;
    /* r and s must be padded with leading zero bytes to ensure they have the
     * same length */
    memset(r, 0, rs_max - r_len);
    memset(s, 0, rs_max - s_len);

    /* concatenate r and s to get the plain signature format */
    plain_sig_buf = BUF_MEM_create(rs_max + rs_max);
    if (!plain_sig_buf)
        goto err;
    memcpy(plain_sig_buf->data, r, rs_max);
    memcpy(plain_sig_buf->data + rs_max, s, rs_max);

    OPENSSL_free(r);
    OPENSSL_free(s);
    ECDSA_SIG_free(tmp_sig);

    return plain_sig_buf;

err:
    if (r)
        OPENSSL_free(r);
    if (s)
        OPENSSL_free(s);
    if (tmp_sig)
        ECDSA_SIG_free(tmp_sig);
    return NULL;
}

const EVP_MD *
eac_oid2md(int protocol)
{
    switch(protocol) {
        case NID_id_TA_ECDSA_SHA_1:
        case NID_id_TA_RSA_v1_5_SHA_1:
        case NID_id_TA_RSA_PSS_SHA_1:
            return EVP_sha1();
        case NID_id_TA_ECDSA_SHA_224:
            return EVP_sha224();
        case NID_id_TA_ECDSA_SHA_256:
        case NID_id_TA_RSA_v1_5_SHA_256:
        case NID_id_TA_RSA_PSS_SHA_256:
            return EVP_sha256();
        case NID_id_TA_ECDSA_SHA_384:
            return EVP_sha384();
        case NID_id_TA_ECDSA_SHA_512:
        case NID_id_TA_RSA_v1_5_SHA_512:
        case NID_id_TA_RSA_PSS_SHA_512:
            return EVP_sha512();
        default:
            log_err("Unknown protocol");
            return NULL;
    }
}

int
EAC_verify(int protocol, EVP_PKEY *key,    const BUF_MEM *signature,
        const BUF_MEM *data)
{
    BUF_MEM *verification_data = NULL, *signature_to_verify = NULL;
    EVP_PKEY_CTX *tmp_key_ctx = NULL;
    int ret = -1;
    const EVP_MD *md = eac_oid2md(protocol);

    check((key && signature), "Invalid arguments");

    tmp_key_ctx = EVP_PKEY_CTX_new(key, NULL);
    if (!tmp_key_ctx || !md
               || EVP_PKEY_verify_init(tmp_key_ctx) <= 0
            || EVP_PKEY_CTX_set_signature_md(tmp_key_ctx, md) <= 0)
        goto err;


    switch (protocol) {
        case NID_id_TA_ECDSA_SHA_1:
        case NID_id_TA_ECDSA_SHA_224:
        case NID_id_TA_ECDSA_SHA_256:
        case NID_id_TA_ECDSA_SHA_384:
        case NID_id_TA_ECDSA_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_EC))
                goto err;

            /* EAC signatures are always in plain signature format for EC curves but
             * OpenSSL only creates X.509 format. Therefore we need to convert between
             * these formats. */
            signature_to_verify = convert_from_plain_sig(signature);
            if (!signature_to_verify)
                goto err;
            break;

        case NID_id_TA_RSA_v1_5_SHA_1:
        case NID_id_TA_RSA_v1_5_SHA_256:
        case NID_id_TA_RSA_v1_5_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_RSA))
                goto err;

            signature_to_verify = BUF_MEM_create_init(signature->data,
                    signature->length);
            if (!EVP_PKEY_CTX_set_rsa_padding(tmp_key_ctx, RSA_PKCS1_PADDING)
                    || !signature_to_verify)
                goto err;
            break;

        case NID_id_TA_RSA_PSS_SHA_1:
        case NID_id_TA_RSA_PSS_SHA_256:
        case NID_id_TA_RSA_PSS_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_RSA))
                goto err;

            signature_to_verify = BUF_MEM_create_init(signature->data, signature->length);
            if (!EVP_PKEY_CTX_set_rsa_padding(tmp_key_ctx, RSA_PKCS1_PSS_PADDING)
                    || !signature_to_verify)
                goto err;
            break;

        default:
            goto err;
    }

    /* EVP_PKEY_sign doesn't perform hashing (despite EVP_PKEY_CTX_set_signature_md).
     * Therefore we need to compute the hash ourself. */
    verification_data = hash(md, NULL, NULL, data);
    if (!verification_data)
        goto err;

    /* Actual signature verification */
    ret = EVP_PKEY_verify(tmp_key_ctx, (unsigned char*) signature_to_verify->data,
            signature_to_verify->length, (unsigned char*) verification_data->data,
            verification_data->length);

err:
    if (verification_data)
        BUF_MEM_free(verification_data);
    if (signature_to_verify)
        BUF_MEM_free(signature_to_verify);
    if (tmp_key_ctx)
        EVP_PKEY_CTX_free(tmp_key_ctx);

    return ret;
}

BUF_MEM *
EAC_sign(int protocol, EVP_PKEY *key, const BUF_MEM *data)
{
    BUF_MEM *signature = NULL, *signature_data = NULL, *plain_sig = NULL;
    EVP_PKEY_CTX *tmp_key_ctx = NULL;
    size_t len;
    const EVP_MD *md = eac_oid2md(protocol);

    check((key && data), "Invalid arguments");

    tmp_key_ctx = EVP_PKEY_CTX_new(key, NULL);
    if (!tmp_key_ctx || !md
               || EVP_PKEY_sign_init(tmp_key_ctx) <= 0
            || EVP_PKEY_CTX_set_signature_md(tmp_key_ctx, md) <= 0)
        goto err;


    switch (protocol) {
        case NID_id_TA_ECDSA_SHA_1:
        case NID_id_TA_ECDSA_SHA_224:
        case NID_id_TA_ECDSA_SHA_256:
        case NID_id_TA_ECDSA_SHA_384:
        case NID_id_TA_ECDSA_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_EC))
                goto err;

            break;

        case NID_id_TA_RSA_v1_5_SHA_1:
        case NID_id_TA_RSA_v1_5_SHA_256:
        case NID_id_TA_RSA_v1_5_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_RSA))
                goto err;

            if (!EVP_PKEY_CTX_set_rsa_padding(tmp_key_ctx, RSA_PKCS1_PADDING))
                goto err;
            break;

        case NID_id_TA_RSA_PSS_SHA_1:
        case NID_id_TA_RSA_PSS_SHA_256:
        case NID_id_TA_RSA_PSS_SHA_512:
            if (!(EVP_PKEY_type(key->type) == EVP_PKEY_RSA))
                goto err;

            if (!EVP_PKEY_CTX_set_rsa_padding(tmp_key_ctx, RSA_PKCS1_PSS_PADDING))
                goto err;
            break;

        default:
            goto err;
    }

    /* EVP_PKEY_sign doesn't perform hashing (despite EVP_PKEY_CTX_set_signature_md).
     * Therefore we need to compute the hash ourself. */
    signature_data = hash(md, NULL, NULL, data);
    if (!signature_data)
        goto err;


    /* Actual signature creation */
    if (EVP_PKEY_sign(tmp_key_ctx, NULL, &len,
               (unsigned char*) signature_data->data,
               signature_data->length) <= 0)
        goto err;
    signature = BUF_MEM_create(len);
    if (!signature)
        goto err;
    if (EVP_PKEY_sign(tmp_key_ctx,
                (unsigned char *) signature->data,
                &signature->length,
                (unsigned char*) signature_data->data,
                signature_data->length) <= 0)
        goto err;


    /* EAC signatures are always in plain signature format for EC curves but
     * OpenSSL only creates X.509 format. Therefore we need to convert between
     * these formats. */
    switch (protocol) {
        case NID_id_TA_ECDSA_SHA_1:
        case NID_id_TA_ECDSA_SHA_224:
        case NID_id_TA_ECDSA_SHA_256:
        case NID_id_TA_ECDSA_SHA_384:
        case NID_id_TA_ECDSA_SHA_512:
            plain_sig = convert_to_plain_sig(signature);
            BUF_MEM_free(signature);
            signature = plain_sig;
            break;
    }

err:
    if (tmp_key_ctx)
        EVP_PKEY_CTX_free(tmp_key_ctx);
    if (signature_data)
        BUF_MEM_free(signature_data);

    return signature;
}
