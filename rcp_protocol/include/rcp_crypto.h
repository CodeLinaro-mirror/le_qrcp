/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/** @file rcp_crypto.h
 *
 * @defgroup rcp_crypto RCP cryptography module
 *
 * RCP crypto module provides cryptography primitives for key handling and data encryption /
 * decryption. Normally the API wraps PSA Crypto API and requires an implementation to be available
 * - for example mbedTLS TF PSA Crypto.
 *
 * @ingroup rcp_internal
 * @{
 */
#ifndef RCP_CRYPTO_H
#define RCP_CRYPTO_H

#include <stdint.h>

#define NONCE_SIZE    12u /**< 96-bit nonce — GCM standard */
#define TAG_SIZE      16u /**< 128-bit authentication tag */
#define AES_KEY_BITS  128u
#define HKDF_INFO     "rcp-protocol-v1" /**< Application and version specific string */
#define HKDF_INFO_LEN (sizeof(HKDF_INFO) - 1)

#define AEAD_OVERHEAD (NONCE_SIZE + TAG_SIZE)
#define RCP_ENC_ENCRYPTION_OVERHEAD                                                                   \
	AEAD_OVERHEAD /**< Extra amount of data present in the encrypted message */

#define RCP_PUBKEY_SIZE 65 /**< Size of the public key */

/** RCP encryption status code. */
enum rcp_enc_status {
	RCP_ENC_OK = 0,
	RCP_ENC_ERR,         /**< Generic error code */
	RCP_ENC_ERR_INVALID, /**< Invalid usage */
	RCP_ENC_ERR_MEMORY,  /**< Insufficient buffer size */
};

/**
 * @brief Initialize the crypto module. Must be called once before any other function.
 *
 * @return RCP_ENC_OK on success or error code (see enum rcp_enc_status).
 */
enum rcp_enc_status rcp_crypto_init(void);

/**
 * @brief Generate an ephemeral ECDH key pair.
 *
 * @param out_id      Key ID of the generated private key
 * @param pubkey      Buffer for the public key
 * @param pubkey_len  Length of the public key in bytes
 *
 * @return RCP_ENC_OK on success or error code (see enum rcp_enc_status).
 */
enum rcp_enc_status rcp_crypto_gen_keypair(uint32_t *out_id, uint8_t *pubkey, uint16_t *pubkey_len);

/**
 * @brief Derive a shared AES key from our private key and the peer public key.
 *        The private key is destroyed afterwards (ephemeral).
 *
 * @param key_id           Key ID of private key.
 * @param out_key_id       Key ID of the shared key.
 * @param peer_pubkey      Peer public key.
 * @param peer_pubkey_len  Lenght of peer public key.
 *
 * @return RCP_ENC_OK on success or error code (see enum rcp_enc_status).
 */
enum rcp_enc_status rcp_crypto_get_shared_key(uint32_t key_id, uint32_t *out_key_id,
					      const uint8_t *peer_pubkey, uint16_t peer_pubkey_len);

/**
 * @brief Encrypt data using the derived shared key. Uses AES-GCM encryption.
 *
 * @param key_id    Key ID of the shared key
 * @param data      Plaintext data
 * @param data_len  Length of data
 * @param out       Output buffer
 * @param out_size  Length of output buffer >= (data_len + RCP_ENC_ENCRYPTION_OVERHEAD)
 * @param out_len   Length of output (RCP_ENC_ENCRYPTION_OVERHEAD + ciphertext)
 *
 * @return RCP_ENC_OK on success or error code (see enum rcp_enc_status).
 */
enum rcp_enc_status rcp_crypto_encrypt_data(uint32_t key_id, const uint8_t *data, uint16_t data_len,
					    uint8_t *out, uint16_t out_size, uint16_t *out_len);

/**
 * @brief Decrypt AES-GCM encrypted data using the derived shared key.
 *
 * @param key_id    Key ID of the shared key
 * @param data      Encrypted data
 * @param data_len  Length of encrypted data >= RCP_ENC_ENCRYPTION_OVERHEAD + 1
 * @param out       Output buffer
 * @param out_size  Length of output buffer >= data_len - RCP_ENC_ENCRYPTION_OVERHEAD
 * @param out_len   Length of plaintext output
 *
 * @return RCP_ENC_OK on success or error code (see enum rcp_enc_status).
 */
enum rcp_enc_status rcp_crypto_decrypt_data(uint32_t key_id, const uint8_t *data, uint16_t data_len,
					    uint8_t *out, uint16_t out_size, uint16_t *out_len);

/** @} */
#endif /* RCP_CRYPTO_H */
