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
 * @file misc.h
 * @brief Miscellaneous functions used in OpenPACE
 *
 * @author Frank Morgner <morgner@informatik.hu-berlin.de>
 * @author Dominik Oepen <oepen@informatik.hu-berlin.de>
 */

#ifndef MISC_H
#define MISC_H

#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/ec.h>

/**
 * @brief Creates a BUF_MEM object
 *
 * @param len required length of the buffer
 *
 * @return Initialized BUF_MEM object or NULL if an error occurred
 */
BUF_MEM *
BUF_MEM_create(size_t len);
/**
 * @brief Creates and initializes a BUF_MEM object
 *
 * @param buf Initial data
 * @param len Length of buf
 *
 * @return Initialized BUF_MEM object or NULL if an error occurred
 */
BUF_MEM *
BUF_MEM_create_init(const void *buf, size_t len);
/**
 * @brief duplicates a BUF_MEM structure
 *
 * @param in BUF_MEM to duplicate
 *
 * @return pointer to the new BUF_MEM or NULL in case of error
 */
BUF_MEM *
BUF_MEM_dup(const BUF_MEM * in);

/**
 * @brief converts an BIGNUM object to a BUF_MEM object
 *
 * @param bn bignumber to convert
 *
 * @return converted bignumber or NULL if an error occurred
 */
BUF_MEM *
BN_bn2buf(const BIGNUM *bn);

/**
 * @brief converts an EC_POINT object to a BUF_MEM object
 *
 * @param ecdh EC_KEY object
 * @param bn_ctx object (optional)
 * @param ecp elliptic curve point to convert
 *
 * @return converted elliptic curve point or NULL if an error occurred
 */
BUF_MEM *
EC_POINT_point2buf(const EC_KEY * ecdh, BN_CTX * bn_ctx, const EC_POINT * ecp);

const ECDH_METHOD *ECDH_OpenSSL_Point(void);
#endif
