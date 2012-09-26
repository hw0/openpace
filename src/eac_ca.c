/*
 * Copyright (c) 2010-2012 Dominik Oepen and Frank Morgner
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
 * @file eac_ca.c
 * @brief Chip Authentication implementation
 *
 * @author Frank Morgner <morgner@informatik.hu-berlin.de>
 * @author Dominik Oepen <oepen@informatik.hu-berlin.de>
 */

#include "eac_asn1.h"
#include "eac_err.h"
#include "eac_lib.h"
#include "eac_util.h"
#include <eac/ca.h>
#include <eac/pace.h>
#include <openssl/crypto.h>
#include <openssl/dh.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pkcs7.h>
#include <string.h>

BUF_MEM *
CA_STEP1_get_pubkey(const EAC_CTX *ctx)
{
    check_return(ctx && ctx->ca_ctx && ctx->ca_ctx->ka_ctx,
            "Invalid arguments");

    return asn1_pubkey(ctx->ca_ctx->protocol, ctx->ca_ctx->ka_ctx->key,
            ctx->bn_ctx, ctx->tr_version);
}

BUF_MEM *
CA_STEP2_get_eph_pubkey(const EAC_CTX *ctx)
{
    check_return(ctx && ctx->ca_ctx && ctx->ca_ctx->ka_ctx,
            "Invalid arguments");

    return get_pubkey(ctx->ca_ctx->ka_ctx->key, ctx->bn_ctx);
}

int
CA_STEP3_check_pcd_pubkey(const EAC_CTX *ctx,
        const BUF_MEM *comp_pubkey, const BUF_MEM *pubkey)
{
    BUF_MEM *my_comp_pubkey = NULL;
    int r = -1;

    check((ctx && ctx->ca_ctx && comp_pubkey && ctx->ca_ctx->ka_ctx),
           "Invalid arguments");

    /* Compress own public key */
    my_comp_pubkey = Comp(ctx->ca_ctx->ka_ctx->key, pubkey, ctx->bn_ctx, ctx->md_ctx);
    if (!my_comp_pubkey)
        goto err;

    /* Check whether or not the received data fits the own data */
    if (my_comp_pubkey->length != comp_pubkey->length
            || memcmp(my_comp_pubkey->data, comp_pubkey->data, comp_pubkey->length) != 0) {
        log_err("Wrong public key");
        r = 0;
    } else
        r = 1;

err:
    if (my_comp_pubkey)
        BUF_MEM_free(my_comp_pubkey);

    return r;
}

int
CA_STEP4_compute_shared_secret(const EAC_CTX *ctx, const BUF_MEM *pubkey)
{
    if (!ctx || !ctx->ca_ctx
            || !KA_CTX_compute_key(ctx->ca_ctx->ka_ctx, pubkey, ctx->bn_ctx)) {
        log_err("Invalid arguments");
        return 0;
    }

    return 1;
}

/* TODO verify signature */
/* TODO check and initialize a given EAC_CTX */
BUF_MEM *
CA_get_pubkey(const unsigned char *ef_cardsecurity, size_t ef_cardsecurity_len)
{
	PKCS7 *p7 = NULL, *signed_data;
    ASN1_OCTET_STRING *os;
    BUF_MEM *pubkey = NULL;
    EAC_CTX *signed_ctx = NULL;

    if (!d2i_PKCS7(&p7, &ef_cardsecurity, ef_cardsecurity_len)
            || !PKCS7_type_is_signed(p7))
        goto err;

    signed_data = p7->d.sign->contents;
    if (OBJ_obj2nid(signed_data->type) != NID_id_SecurityObject
            || ASN1_TYPE_get(signed_data->d.other) != V_ASN1_OCTET_STRING)
        goto err;
    os = signed_data->d.other->value.octet_string;

    signed_ctx = EAC_CTX_new();

    if (!EAC_CTX_init_ef_cardaccess(os->data, os->length, signed_ctx)
            || !signed_ctx || !signed_ctx->ca_ctx
            || !signed_ctx->ca_ctx->ka_ctx)
        goto err;

    pubkey = get_pubkey(signed_ctx->ca_ctx->ka_ctx->key, signed_ctx->bn_ctx);

err:
    EAC_CTX_clear_free(signed_ctx);
    if (p7)
        PKCS7_free(p7);

    return pubkey;
}

/* Nonce for CA is always 8 bytes long */
#define CA_NONCE_SIZE 8
int
CA_STEP5_derive_keys(const EAC_CTX *ctx, const BUF_MEM *pub,
                   BUF_MEM **nonce, BUF_MEM **token)
{
    BUF_MEM *r = NULL;
    BUF_MEM *authentication_token = NULL;

    check((ctx && ctx->ca_ctx && ctx->ca_ctx->ka_ctx && nonce && token),
            "Invalid arguments");

    /* Generate nonce  and derive k_mac and k_enc*/
    r = randb(CA_NONCE_SIZE);
    if (!r || !KA_CTX_derive_keys(ctx->ca_ctx->ka_ctx, r, ctx->md_ctx))
        goto err;

    /* Compute authentication token */
    authentication_token = get_authentication_token(ctx->ca_ctx->protocol,
            ctx->ca_ctx->ka_ctx, ctx->bn_ctx, ctx->tr_version,
            pub);
    if (!authentication_token)
        goto err;

    *nonce = r;
    *token = authentication_token;

    return 1;

err:
    BUF_MEM_clear_free(r);
    if (authentication_token) {
        BUF_MEM_free(authentication_token);
    }

    return 0;
}

int
CA_STEP6_derive_keys(EAC_CTX *ctx, const BUF_MEM *nonce, const BUF_MEM *token)
{
    int rv = -1;

    check((ctx && ctx->ca_ctx), "Invalid arguments");

    if (!KA_CTX_derive_keys(ctx->ca_ctx->ka_ctx, nonce, ctx->md_ctx))
        goto err;

    rv = verify_authentication_token(ctx->ca_ctx->protocol,
            ctx->ca_ctx->ka_ctx,
            ctx->bn_ctx, ctx->tr_version, token);
    if (rv < 0)
        goto err;

    /* PACE, TA and CA were successful. Update the trust anchor! */
    if (rv) {
        if (ctx->ta_ctx->new_trust_anchor) {
            CVC_CERT_free(ctx->ta_ctx->trust_anchor);
            ctx->ta_ctx->trust_anchor = ctx->ta_ctx->new_trust_anchor;
            ctx->ta_ctx->new_trust_anchor = NULL;
        }
    }

err:
    return rv;
}