#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize security manager: load ECDH P-256 keypair from NVS,
 * or generate a new one on first boot.
 *
 * This is the easiest port from Arduino — already uses mbedTLS directly.
 * Same ECDH P-256, AES-256-GCM, HKDF-SHA256 as Arduino version.
 */
esp_err_t security_manager_init(void);

/**
 * Clean up: zero private key from RAM.
 */
void security_manager_cleanup(void);

/**
 * Decrypt a server-sent ECDH envelope.
 *
 * Input JSON format:
 *   {"encrypted": b64, "ephemeral_public_key": PEM, "iv": b64, "tag": b64, "salt": b64}
 *
 * Process:
 *   1. ECDH key agreement (device private key + server ephemeral public key)
 *   2. HKDF-SHA256 key derivation with salt
 *   3. AES-256-GCM decryption
 *
 * Returns heap-allocated plaintext string (caller must free), or NULL on error.
 */
char *security_manager_decrypt(const char *envelope_json);

/**
 * Get PEM-encoded public key for display/upload.
 * Returns pointer to static buffer (valid until next call).
 */
const char *security_manager_get_public_key_pem(void);

/**
 * Upload public key to server via HTTP POST.
 */
esp_err_t security_manager_upload_public_key(const char *server_host,
                                              const char *auth_token,
                                              const char *device_id);

/**
 * Check if public key has been uploaded to server (persisted in NVS).
 */
bool security_manager_is_key_uploaded(void);

/**
 * Reset key uploaded flag (force re-upload after re-pairing).
 */
void security_manager_reset_key_uploaded(void);

/**
 * Check if encryption is enabled and keypair is loaded.
 */
bool security_manager_is_enabled(void);

#ifdef __cplusplus
}
#endif
