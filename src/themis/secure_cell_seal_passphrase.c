/*
 * Copyright (c) 2020 Cossack Labs Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "themis/secure_cell_seal_passphrase.h"

#include "soter/soter_kdf.h"
#include "soter/soter_rand.h"
#include "soter/soter_wipe.h"

#include "themis/secure_cell_alg.h"
#include "themis/sym_enc_message.h"

/*
 * Themis always uses the default algorithm and parameters for encryption.
 * These may be transparently updated between library releases. However,
 * decryption code has to support all previously produces data formats.
 */

/* Must be a #define to be treated as compile-time constant */
#define DEFAULT_AUTH_TOKEN_SIZE                                                                                \
    (themis_scell_auth_token_passphrase_min_size + THEMIS_AUTH_SYM_IV_LENGTH + THEMIS_AUTH_SYM_AUTH_TAG_LENGTH \
     + themis_scell_pbkdf2_context_min_size + THEMIS_AUTH_SYM_PBKDF2_SALT_LENGTH)

static inline uint32_t soter_alg_without_kdf(uint32_t alg)
{
    /* Explicit cast for mask to have correct size before negation */
    return (alg & ~((uint32_t)SOTER_SYM_KDF_MASK)) | SOTER_SYM_NOKDF;
}

static inline size_t soter_alg_key_length(uint32_t alg)
{
    return (alg & SOTER_SYM_KEY_LENGTH_MASK) / 8;
}

static inline bool soter_alg_reserved_bits_valid(uint32_t alg)
{
    static const uint32_t used_bits = SOTER_SYM_KEY_LENGTH_MASK | SOTER_SYM_PADDING_MASK
                                      | SOTER_SYM_KDF_MASK | SOTER_SYM_ALG_MASK;
    return (alg & ~used_bits) == 0;
}

themis_status_t themis_auth_sym_encrypt_message_with_passphrase_(const uint8_t* passphrase,
                                                                 size_t passphrase_length,
                                                                 const uint8_t* message,
                                                                 size_t message_length,
                                                                 const uint8_t* user_context,
                                                                 size_t user_context_length,
                                                                 uint8_t* auth_token,
                                                                 size_t* auth_token_length,
                                                                 uint8_t* encrypted_message,
                                                                 size_t* encrypted_message_length)
{
    themis_status_t res = THEMIS_FAIL;
    uint8_t iv[THEMIS_AUTH_SYM_IV_LENGTH] = {0};
    uint8_t salt[THEMIS_AUTH_SYM_PBKDF2_SALT_LENGTH] = {0};
    uint8_t auth_tag[THEMIS_AUTH_SYM_AUTH_TAG_LENGTH] = {0};
    uint8_t derived_key[THEMIS_AUTH_SYM_KEY_LENGTH / 8] = {0};
    struct themis_scell_auth_token_passphrase hdr = {0};
    struct themis_scell_pbkdf2_context kdf = {0};
    size_t auth_tag_length = 0;

    /* Message length is currently stored as 32-bit integer, sorry */
    if (message_length > UINT32_MAX) {
        return THEMIS_INVALID_PARAMETER;
    }

    kdf.iteration_count = THEMIS_AUTH_SYM_PBKDF2_ITERATIONS;
    kdf.salt = salt;
    kdf.salt_length = sizeof(salt);

    hdr.alg = THEMIS_AUTH_SYM_PASSPHRASE_ALG;
    hdr.iv = iv;
    hdr.iv_length = sizeof(iv);
    hdr.auth_tag = auth_tag;
    hdr.auth_tag_length = sizeof(auth_tag);
    hdr.message_length = (uint32_t)message_length;
    /* KDF pointer is ignored but size is important */
    hdr.kdf_context = NULL;
    hdr.kdf_context_length = (uint32_t)themis_scell_pbkdf2_context_size(&kdf);

    res = soter_rand(salt, sizeof(salt));
    if (res != THEMIS_SUCCESS) {
        goto error;
    }

    res = soter_pbkdf2_sha256(passphrase,
                              passphrase_length,
                              kdf.salt,
                              kdf.salt_length,
                              kdf.iteration_count,
                              derived_key,
                              sizeof(derived_key));
    if (res != THEMIS_SUCCESS) {
        goto error;
    }

    res = soter_rand(iv, sizeof(iv));
    if (res != THEMIS_SUCCESS) {
        goto error;
    }

    /* We are doing KDF ourselves, ask Soter to not interfere */
    auth_tag_length = hdr.auth_tag_length;
    res = themis_auth_sym_plain_encrypt(soter_alg_without_kdf(hdr.alg),
                                        derived_key,
                                        sizeof(derived_key),
                                        hdr.iv,
                                        hdr.iv_length,
                                        user_context,
                                        user_context_length,
                                        message,
                                        message_length,
                                        encrypted_message,
                                        encrypted_message_length,
                                        auth_tag,
                                        &auth_tag_length);
    if (res != THEMIS_SUCCESS) {
        goto error;
    }
    /*
     * We should have allocated just the right amount here, but if our guess
     * was off then the header is not usable due to bothched data offsets.
     */
    if (auth_tag_length != hdr.auth_tag_length) {
        res = THEMIS_FAIL;
        goto error;
    }

    if (*auth_token_length < themis_scell_auth_token_passphrase_size(&hdr)) {
        res = THEMIS_BUFFER_TOO_SMALL;
        goto error;
    }
    res = themis_write_scell_auth_token_passphrase(&hdr, auth_token, *auth_token_length);
    if (res != THEMIS_SUCCESS) {
        goto error;
    }
    res = themis_write_scell_pbkdf2_context(&hdr, &kdf, auth_token, *auth_token_length);
    if (res != THEMIS_SUCCESS) {
        goto error;
    }
    *auth_token_length = themis_scell_auth_token_passphrase_size(&hdr);
    *encrypted_message_length = message_length;

error:
    soter_wipe(iv, sizeof(iv));
    soter_wipe(salt, sizeof(salt));
    soter_wipe(auth_tag, sizeof(auth_tag));
    soter_wipe(derived_key, sizeof(derived_key));

    return res;
}

themis_status_t themis_auth_sym_encrypt_message_with_passphrase(const uint8_t* passphrase,
                                                                size_t passphrase_length,
                                                                const uint8_t* message,
                                                                size_t message_length,
                                                                const uint8_t* user_context,
                                                                size_t user_context_length,
                                                                uint8_t* auth_token,
                                                                size_t* auth_token_length,
                                                                uint8_t* encrypted_message,
                                                                size_t* encrypted_message_length)
{
    THEMIS_CHECK_PARAM(passphrase != NULL && passphrase_length != 0);
    THEMIS_CHECK_PARAM(message != NULL && message_length != 0);
    if (user_context != NULL) {
        THEMIS_CHECK_PARAM(user_context_length != 0);
    } else {
        THEMIS_CHECK_PARAM(user_context_length == 0);
    }
    THEMIS_CHECK_PARAM(auth_token_length != NULL);
    THEMIS_CHECK_PARAM(encrypted_message_length != NULL);

    if (!auth_token_length || !encrypted_message || *auth_token_length < DEFAULT_AUTH_TOKEN_SIZE
        || *encrypted_message_length < message_length) {
        *auth_token_length = DEFAULT_AUTH_TOKEN_SIZE;
        *encrypted_message_length = message_length;
        return THEMIS_BUFFER_TOO_SMALL;
    }

    return themis_auth_sym_encrypt_message_with_passphrase_(passphrase,
                                                            passphrase_length,
                                                            message,
                                                            message_length,
                                                            user_context,
                                                            user_context_length,
                                                            auth_token,
                                                            auth_token_length,
                                                            encrypted_message,
                                                            encrypted_message_length);
}

themis_status_t themis_auth_sym_decrypt_message_with_passphrase_(const uint8_t* passphrase,
                                                                 size_t passphrase_length,
                                                                 const uint8_t* user_context,
                                                                 size_t user_context_length,
                                                                 const uint8_t* auth_token,
                                                                 size_t auth_token_length,
                                                                 const uint8_t* encrypted_message,
                                                                 size_t encrypted_message_length,
                                                                 uint8_t* message,
                                                                 size_t* message_length)
{
    themis_status_t res = THEMIS_FAIL;
    struct themis_scell_auth_token_passphrase hdr = {0};
    struct themis_scell_pbkdf2_context kdf = {0};
    /* Use maximum possible length, not the default one */
    uint8_t derived_key[THEMIS_AUTH_SYM_MAX_KEY_LENGTH / 8] = {0};
    size_t derived_key_length = 0;

    res = themis_read_scell_auth_token_passphrase(auth_token, auth_token_length, &hdr);
    if (res != THEMIS_SUCCESS) {
        return res;
    }

    /* Check that message length is consistent with header */
    if (hdr.message_length != encrypted_message_length) {
        return THEMIS_FAIL;
    }

    /*
     * Verify that algorithm contains a KDF function we support. In particular,
     * it must not contain SOTER_SYM_NOKDF which is used by master key API.
     */
    switch (hdr.alg & SOTER_SYM_KDF_MASK) {
    case SOTER_SYM_PBKDF2:
        break;
    case SOTER_SYM_NOKDF:
        return THEMIS_FAIL;
    default:
        return THEMIS_FAIL;
    }
    /* The algorithm also defines length of the derived key we need */
    derived_key_length = soter_alg_key_length(hdr.alg);
    switch (derived_key_length) {
    case SOTER_SYM_256_KEY_LENGTH / 8:
    case SOTER_SYM_192_KEY_LENGTH / 8:
    case SOTER_SYM_128_KEY_LENGTH / 8:
        break;
    default:
        return THEMIS_FAIL;
    }
    /* Algorithm field contains unused bits that must be set to zero */
    if (!soter_alg_reserved_bits_valid(hdr.alg)) {
        return THEMIS_FAIL;
    }

    res = themis_read_scell_pbkdf2_context(&hdr, &kdf);
    if (res != THEMIS_SUCCESS) {
        return res;
    }

    res = soter_pbkdf2_sha256(passphrase,
                              passphrase_length,
                              kdf.salt,
                              kdf.salt_length,
                              kdf.iteration_count,
                              derived_key,
                              derived_key_length);
    if (res != THEMIS_SUCCESS) {
        goto error;
    }

    /* We are doing KDF ourselves, ask Soter to not interfere */
    res = themis_auth_sym_plain_decrypt(soter_alg_without_kdf(hdr.alg),
                                        derived_key,
                                        derived_key_length,
                                        hdr.iv,
                                        hdr.iv_length,
                                        user_context,
                                        user_context_length,
                                        encrypted_message,
                                        encrypted_message_length,
                                        message,
                                        message_length,
                                        hdr.auth_tag,
                                        hdr.auth_tag_length);
    /* Sanity check of resulting message length */
    if (*message_length != encrypted_message_length) {
        res = THEMIS_FAIL;
        goto error;
    }

error:
    soter_wipe(derived_key, sizeof(derived_key));

    return res;
}

themis_status_t themis_auth_sym_decrypt_message_with_passphrase(const uint8_t* passphrase,
                                                                size_t passphrase_length,
                                                                const uint8_t* user_context,
                                                                size_t user_context_length,
                                                                const uint8_t* auth_token,
                                                                size_t auth_token_length,
                                                                const uint8_t* encrypted_message,
                                                                size_t encrypted_message_length,
                                                                uint8_t* message,
                                                                size_t* message_length)
{
    themis_status_t res = THEMIS_FAIL;
    uint32_t expected_message_length = 0;

    THEMIS_CHECK_PARAM(passphrase != NULL && passphrase_length != 0);
    if (user_context != NULL) {
        THEMIS_CHECK_PARAM(user_context_length != 0);
    } else {
        THEMIS_CHECK_PARAM(user_context_length == 0);
    }
    THEMIS_CHECK_PARAM(auth_token != NULL && auth_token_length != 0);
    THEMIS_CHECK_PARAM(message_length != NULL);

    /* Do a quick guess without parsing the message too deeply here */
    res = themis_scell_auth_token_message_size(auth_token, auth_token_length, &expected_message_length);
    if (res != THEMIS_SUCCESS) {
        return res;
    }
    if (!message || *message_length < expected_message_length) {
        *message_length = expected_message_length;
        return THEMIS_BUFFER_TOO_SMALL;
    }

    /* encrypted_message may be omitted when only querying plaintext size */
    THEMIS_CHECK_PARAM(encrypted_message != NULL && encrypted_message_length != 0);

    return themis_auth_sym_decrypt_message_with_passphrase_(passphrase,
                                                            passphrase_length,
                                                            user_context,
                                                            user_context_length,
                                                            auth_token,
                                                            auth_token_length,
                                                            encrypted_message,
                                                            encrypted_message_length,
                                                            message,
                                                            message_length);
}
