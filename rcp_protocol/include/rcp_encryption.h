/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/** @file rcp_encryption.h
 *
 * @defgroup rcp_encryption
 *
 * RCP message encryption module provides function for encrypting and decrypting RCP payloads and
 * managing runtime public and private keys.
 *
 * @ingroup rcp_internal
 * @{
 */
#ifndef _RCP_ENCRYPTION_H_
#define _RCP_ENCRYPTION_H_

#include <rcp_protocol.h>
#include <stdint.h>

/** @brief Initialize RCP message encryption module
 *
 * @return true in case of success or false.
 */
bool rcp_encryption_init(void);

/** @brief Copy peer key to current RCP encryption context and derive a shared key.
 *
 * @param peer_key Peer public key.
 * @param peer_key_len Length of the peer public key.
 *
 * @return true in case of success or false.
 */
bool rcp_enc_set_peer_key(uint8_t *peer_key, uint16_t peer_key_len);

/** @brief Get current public key.
 *
 * Get current public key for sharing with peer.
 *
 * @param pub_key Pointer to public key buffer.
 *
 * @return Length of the public key.
 */
uint16_t rcp_enc_get_pub_key(uint8_t **pub_key);

/** @brief Encrypt data using current shared key.
 *
 * @param buf Data for encryption
 * @param buf_len Lenght of data for encryption
 * @param out buffer for encrypted data
 * @param out_size size of buffer for encrypted data
 *
 * @return Size of encrypted data or 0 in case of a failure.
 */
uint16_t rcp_encrypt_buf(uint8_t *buf, uint16_t buf_len, uint8_t *out, uint16_t out_size);

/** @brief Decrypt data using current shared key.
 *
 * @param buf Data for decryption
 * @param buf_len Lenght of data for decryption
 * @param out buffer for decrypted data
 * @param out_size size of buffer for decrypted data
 *
 * @return Size of decrypted data or 0 in case of a failure.
 */
uint16_t rcp_decrypt_buf(uint8_t *buf, uint16_t buf_len, uint8_t *out, uint16_t out_size);

/** @} */
#endif /* _RCP_ENCRYPTION_H_ */
