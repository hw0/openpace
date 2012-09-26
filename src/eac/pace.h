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
 * @file pace.h
 * @brief Interface for Password Authenticated Connection Establishment
 *
 * PACE is a protocol used to establish strong session keys based
 * on a weak shared secret (password). The result of a PACE run
 * are two symmetric keys, one for MAC computation and one for
 * encryption. It was specified for Extended Access Control (EAC)
 * in Machine Readable Travel Documents (MRTD), but can also be
 * used for securing any other communication channel.
 * PACE can be used with different suites of algorithms and is not
 * subject to any patents.
 *
 * @author Dominik Oepen <oepen@informatik.hu-berlin.de>
 * @author Frank Morgner <morgner@informatik.hu-berlin.de>
 */

#ifndef PACE_H_
#define PACE_H_

#include "eac.h"
#include <openssl/bn.h>
#include <openssl/buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Enum for the type of the secret used. */
enum s_type {
    /** MRZ is the Machine Readable Zone, printed on the card, encoding
     * the personal information of the user */
    PACE_MRZ = 1,
    /** CAN is the Card access number printed on the card */
    PACE_CAN,
    /** PIN is the Personal Identification Number, a secret known only
     * to the user and not printed on the card */
    PACE_PIN,
    /** PUK is the Personal Unblocking key. This type of secret is used
     * when the card is suspended due to too many incorrect PACE runs */
    PACE_PUK,
    /** This type of secret is not defined in BSI TR-03110. We use it as
     * a generic type, so we can use PACE independent from a ID card */
    PACE_RAW,
    /** Undefined type, if nothing else matches */
    PACE_SEC_UNDEF
};

/** @brief Shared secret for PACE */
typedef struct pace_sec {
    /** @brief Type of the secret */
    enum s_type type;
    /** @brief Raw secret */
    BUF_MEM *mem;
    /** @brief Encoded secret */
    BUF_MEM *encoded;
} PACE_SEC;

/**
 * @addtogroup management
 *
 * @{ ************************************************************************/

/**
 * @brief Free a PACE secret.
 *
 * Sensitive memory is cleared with OPENSSL_cleanse().
 *
 * @param[in] s (optional) Object to free
 */
void PACE_SEC_clear_free(PACE_SEC * s);
/**
 * @brief Create and initialize a new PACE secret.
 *
 * @param[in] sec Raw secret
 * @param[in] sec_len Length of \a sec
 * @param[in] type Type of secret
 *
 * @return New PACE secret or NULL in case of an error
 */
PACE_SEC *
PACE_SEC_new(const char *sec, size_t sec_len, enum s_type type);
/** @} ***********************************************************************/

/**
 * @addtogroup paceproto
 *
 * @{ ************************************************************************/

/**
 * @brief Generates and encrypts a nonce.
 *
 * @param[in,out] ctx       EAC context. The nonce is saved in \a ctx.
 * @param[in] pi        Shared secret for PACE
 * @param[out] enc_nonce Where to save the new object containing the encrypted nonce
 *
 * @return          1 on success or 0 in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
int PACE_STEP1_enc_nonce(const EAC_CTX * ctx, const PACE_SEC * pi,
                   BUF_MEM ** enc_nonce);
/**
 * @brief Decrypt the nonce from the other party.
 *
 * @param[in,out] ctx EAC context The decrypted nonce is saved in \a ctx.
 * @param[in] pi Shared secret for PACE
 * @param[in] enc_nonce Encrypted nonce from the other party
 *
 * @return 1 on success or 0 in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
int PACE_STEP2_dec_nonce(const EAC_CTX * ctx, const PACE_SEC * pi,
        const BUF_MEM * enc_nonce);
/**
 * @brief Generate a mapping data to perform the mapping to ephemeral domain
 * parameters
 *
 * @param[in,out] ctx EAC context
 *
 * @return Mapping data to be transmitted to the other party or NULL in case of
 * an error
 *
 * @see PACE's @ref paceps describes this protocol step
 * @see @ref pacemapecdh and @ref pacemapdh describe the mappings
 */
BUF_MEM *
PACE_STEP3A_generate_mapping_data(const EAC_CTX * ctx);
/**
 * @brief Map to the ephemeral domain parameters.
 *
 * @param[in,out] ctx EAC context
 * @param[in] in Mapping data from the other party
 *
 * @return 1 on success or 0 in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 * @see @ref pacemapecdh and @ref pacemapdh describe the mappings
 */
int
PACE_STEP3A_map_generator(const EAC_CTX * ctx, const BUF_MEM * in);
/**
 * @brief Generate a keypair for key agreement
 *
 * @param[in,out] ctx EAC context
 *
 * @return Public key or NULL in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
BUF_MEM * PACE_STEP3B_generate_ephemeral_key(EAC_CTX * ctx);
/**
 * @brief Compute the shared secret for key agreement
 *
 * @param[in,out] ctx EAC context. The secret is saved in \a ctx.
 * @param[in] in Public key from the other party
 *
 * @return 1 on success 0 in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
int
PACE_STEP3B_compute_shared_secret(const EAC_CTX * ctx, const BUF_MEM * in);
/**
 * @brief Derives encryption and authentication keys
 *
 * @param[in,out] ctx EAC context. The keys are saved in \a ctx.
 *
 * @return          1 on success or 0 in case of an error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
int PACE_STEP3C_derive_keys(const EAC_CTX *ctx);
/**
 * @brief Compute the authentication token from domain parameters
 *           and public key
 *
 * @param[in] ctx EAC context
 * @param[in] pub Public key from the other party (generated in PACE step 3b)
 *
 * @return Authentication token or NULL in case of error
 *
 * @see PACE's @ref paceps describes this protocol step
 */
BUF_MEM * PACE_STEP3D_compute_authentication_token(const EAC_CTX *ctx,
        const BUF_MEM *pub);
/**
 * @brief Verifies an authentication token.
 *
 * @param[in] ctx EAC context
 * @param[in] token Authentication token from the other party
 *
 * @return 1 if the token has been verified as correct, 0 if not or -1 in case of an error
 * occurred.
 *
 * @see PACE's @ref paceps describes this protocol step
 */
int PACE_STEP3D_verify_authentication_token(const EAC_CTX * ctx,
        const BUF_MEM * token);

/** @} ***********************************************************************/


#ifdef  __cplusplus
}
#endif
#endif