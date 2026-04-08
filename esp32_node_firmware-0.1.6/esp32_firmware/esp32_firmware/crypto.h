#pragma once

#include <Arduino.h>
#include "mbedtls/ecdh.h"      // mbedtls_ecdh_compute_shared
#include "mbedtls/gcm.h"       // AES-GCM encrypt / decrypt
#include "mbedtls/hkdf.h"      // HKDF key derivation
#include "mbedtls/md.h"        // mbedtls_md_info_from_type (needed by HKDF)
#include "mbedtls/entropy.h"   // Hardware entropy source
#include "mbedtls/ctr_drbg.h"  // Deterministic random bit generator (seeded from entropy)
#include "mbedtls/ecp.h"       // Elliptic curve point / group operations
#include "credentials.h"

// =============================================================================
// crypto.h  —  ECDH key agreement + AES-128-GCM encryption / decryption
//
// HOW IT WORKS (simplified):
//
//  1. Each node generates a temporary (ephemeral) Curve25519 key pair:
//       - private key  (kept secret, never transmitted)
//       - public key   (sent to the other node)
//
//  2. Both nodes do the X25519 Diffie-Hellman calculation:
//       shared_secret = own_private_key × peer_public_key
//     This produces the SAME 32-byte secret on both sides without either
//     node having to send their private key over the air.
//
//  3. The shared secret is fed into HKDF-SHA256 (a key derivation function)
//     to produce a clean 16-byte AES-128 encryption key.
//     The HKDF_INFO string ("esp32-cred-v1") acts as a domain separator —
//     even if the same shared secret were used for something else, a
//     different info string would produce a different key.
//
//  4. The credential bundle is encrypted with AES-128-GCM:
//       - A random 12-byte nonce is generated per message (never reused)
//       - GCM produces both ciphertext AND a 16-byte authentication tag
//       - The tag lets the receiver detect any tampering or corruption
//
//  5. Wire layout: [nonce 12B][ciphertext][tag 16B]
//
// NOTE: Uses mbedtls_ecp_* functions directly rather than the higher-level
// mbedtls_ecdh_context, because the internal struct layout of
// mbedtls_ecdh_context changed in mbedTLS 3.x (ESP-IDF v5) and direct
// struct access via MBEDTLS_PRIVATE() no longer works.
// =============================================================================


// Context string fed into HKDF — acts as a "domain label" so that keys
// derived for this purpose are distinct from keys derived in any other context
// even if the same Diffie-Hellman shared secret were somehow reused.
static const char* HKDF_INFO = "esp32-cred-v1";

// ── Size constants ─────────────────────────────────────────────────────────────
static const size_t CURVE25519_KEY_LEN   = 32;   // Curve25519 public / private key size (bytes)
static const size_t AES_KEY_LEN          = 16;   // AES-128 key size (bytes)
static const size_t GCM_NONCE_LEN        = 12;   // GCM nonce (IV) size — NIST recommended
static const size_t GCM_TAG_LEN          = 16;   // GCM authentication tag size (bytes)
static const size_t BUNDLE_PLAINTEXT_MAX = sizeof(CredentialBundle);
                                                  // Upper bound for plaintext buffer sizing
static const size_t ESPNOW_OVERHEAD      = CURVE25519_KEY_LEN + GCM_NONCE_LEN + GCM_TAG_LEN;
                                                  // Bytes added by ECDH pubkey + GCM wrapper
static const size_t ENCRYPTED_BUNDLE_MAX = BUNDLE_PLAINTEXT_MAX + ESPNOW_OVERHEAD;
                                                  // Maximum encrypted output size


// ── Random number generator (singleton) ───────────────────────────────────────
// The RNG is shared by all crypto operations in this file. It is seeded once
// from the ESP32's hardware entropy source (thermal noise, radio jitter, etc.)
// and then used as a deterministic RNG (CTR-DRBG) which is fast and safe.
static mbedtls_entropy_context  _entropy;     // Hardware entropy collector
static mbedtls_ctr_drbg_context _ctr_drbg;   // Pseudo-random generator fed by entropy
static bool _rngInitialised = false;

static bool initRng() {
    if (_rngInitialised) return true;   // Only initialise once per boot

    mbedtls_entropy_init(&_entropy);
    mbedtls_ctr_drbg_init(&_ctr_drbg);

    // "pers" is a personalisation string — it's hashed into the RNG seed to
    // make this device's RNG distinct from a factory-default RNG state.
    const char* pers = "esp32cred";
    int ret = mbedtls_ctr_drbg_seed(&_ctr_drbg,         // RNG to initialise
                                     mbedtls_entropy_func, // entropy source callback
                                     &_entropy,            // entropy context
                                     (const uint8_t*)pers, // personalisation string
                                     strlen(pers));
    _rngInitialised = (ret == 0);
    return _rngInitialised;
}


// ── Key pair holder ────────────────────────────────────────────────────────────
// Caller creates one of these, passes it to cryptoGenKeypair(), sends the
// publicKey to the peer, then calls cryptoDeriveKey() to derive the AES key.
// After cryptoDeriveKey() the privateKey field is zeroed — it is single-use.
struct EcdhContext {
    uint8_t publicKey[CURVE25519_KEY_LEN]  = {0};   // Send this to the peer
    uint8_t privateKey[CURVE25519_KEY_LEN] = {0};   // KEEP SECRET — zeroed after use
};


// ── cryptoGenKeypair ───────────────────────────────────────────────────────────
// Generates a fresh ephemeral Curve25519 key pair and stores it in `ctx`.
// Call this once per transaction — never reuse a keypair across multiple
// encrypt/decrypt operations.
// Returns false if the RNG or mbedTLS call fails.
bool cryptoGenKeypair(EcdhContext& ctx) {
    if (!initRng()) return false;

    // mbedTLS objects for the elliptic curve group, private scalar, and public point
    mbedtls_ecp_group grp;    // Defines which curve to use (Curve25519 in our case)
    mbedtls_mpi       d;      // Private key — a random large integer (scalar)
    mbedtls_ecp_point Q;      // Public key — a point on the curve = d × base_point

    // Initialise all structures before use (mbedTLS requires this)
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    do {
        // Load the Curve25519 parameters into grp
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;

        // Generate a random private scalar d and compute public point Q = d × G
        // mbedtls_ctr_drbg_random is the callback that feeds our RNG
        if (mbedtls_ecp_gen_keypair(&grp, &d, &Q,
                                    mbedtls_ctr_drbg_random, &_ctr_drbg) != 0) break;

        // Export private key as raw 32 big-endian bytes into ctx.privateKey
        if (mbedtls_mpi_write_binary(&d, ctx.privateKey, CURVE25519_KEY_LEN) != 0) break;

        // For Curve25519, the public key exchanged is just the X coordinate of Q.
        // (The Y coordinate is not needed for X25519 Diffie-Hellman.)
        // Q.X is accessed via MBEDTLS_PRIVATE() because it is a "private" struct member
        // in mbedTLS 3.x — this is the approved way to access it.
        if (mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X),
                                     ctx.publicKey, CURVE25519_KEY_LEN) != 0) break;
        ok = true;
    } while (false);   // do-while(false) used as a structured goto: break = cleanup + return

    // Always free mbedTLS objects even if we failed partway through
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    return ok;
}


// ── cryptoDeriveKey ────────────────────────────────────────────────────────────
// Performs X25519 Diffie-Hellman with our private key and the peer's public key,
// then runs HKDF-SHA256 to produce a 16-byte AES-128 key in `outKey`.
//
// After this call, ctx.privateKey is zeroed — the ephemeral key is discarded.
// `peerPubKey` is the 32-byte X coordinate received from the other node.
// Returns false on any mbedTLS error.
bool cryptoDeriveKey(EcdhContext& ctx, const uint8_t peerPubKey[CURVE25519_KEY_LEN],
                     uint8_t outKey[AES_KEY_LEN]) {
    if (!initRng()) return false;

    mbedtls_ecp_group grp;    // Curve25519 group parameters
    mbedtls_mpi       d;      // Our own private key scalar
    mbedtls_ecp_point Qp;     // Peer's public key point (reconstructed from X coordinate)
    mbedtls_mpi       shared; // Result of scalar multiplication — the shared secret

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&shared);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;

        // Load our private key from the context
        if (mbedtls_mpi_read_binary(&d, ctx.privateKey, CURVE25519_KEY_LEN) != 0) break;

        // Reconstruct the peer's public point from just the X coordinate.
        // Curve25519 uses only the X coordinate for key exchange (the curve is
        // a Montgomery curve, and Y is not needed for the computation).
        // Z=1 means the point is in affine (non-projective) coordinates.
        if (mbedtls_mpi_read_binary(&Qp.MBEDTLS_PRIVATE(X),
                                    peerPubKey, CURVE25519_KEY_LEN) != 0) break;
        if (mbedtls_mpi_lset(&Qp.MBEDTLS_PRIVATE(Z), 1) != 0) break;  // Z=1 → affine coords

        // Compute shared = d × Qp (scalar point multiplication).
        // Both sides get the same result because:
        //   our_private × their_public == their_private × our_public
        //   (both equal: our_private × their_private × base_point)
        if (mbedtls_ecdh_compute_shared(&grp, &shared, &Qp, &d,
                                        mbedtls_ctr_drbg_random, &_ctr_drbg) != 0) break;

        // Export the shared secret as 32 raw bytes
        uint8_t sharedBytes[CURVE25519_KEY_LEN];
        if (mbedtls_mpi_write_binary(&shared, sharedBytes, CURVE25519_KEY_LEN) != 0) break;

        // Run HKDF-SHA256 to stretch/clean the shared secret into a 16-byte AES key.
        // HKDF is used instead of the raw secret because:
        //   - It removes any bias from the DH output
        //   - The info string domain-separates this key from any other derived key
        //   - It makes the output look like uniform random bytes regardless of input shape
        int ret = mbedtls_hkdf(
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),  // Hash function to use
            nullptr, 0,                                     // No salt (use HKDF default)
            sharedBytes, CURVE25519_KEY_LEN,                // Input key material
            (const uint8_t*)HKDF_INFO, strlen(HKDF_INFO),  // Domain separator
            outKey, AES_KEY_LEN);                           // Output: 16-byte AES key
        memset(sharedBytes, 0, sizeof(sharedBytes));        // Wipe shared secret from RAM
        if (ret != 0) break;
        ok = true;
    } while (false);

    // Zero the private key — it must never be used again after key derivation
    memset(ctx.privateKey, 0, CURVE25519_KEY_LEN);

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&shared);
    return ok;
}


// ── cryptoEncrypt ──────────────────────────────────────────────────────────────
// Encrypts `plaintext` with AES-128-GCM using `key`.
// Output buffer layout:  [nonce 12B] [ciphertext len=plaintextLen] [tag 16B]
// Caller must allocate outBuf with at least plaintextLen + 28 bytes.
// `outLen` is set to the total bytes written.
// Returns false on any error — outBuf contents are undefined on failure.
bool cryptoEncrypt(const uint8_t key[AES_KEY_LEN],
                   const uint8_t* plaintext, size_t plaintextLen,
                   uint8_t* outBuf, size_t& outLen) {
    if (!initRng()) return false;
    // Sanity check: make sure the output would fit our pre-calculated maximum
    if (plaintextLen + GCM_NONCE_LEN + GCM_TAG_LEN > ENCRYPTED_BUNDLE_MAX) return false;

    // Lay out pointers into outBuf for the three sections
    uint8_t* nonce      = outBuf;                           // First 12 bytes: nonce
    uint8_t* ciphertext = outBuf + GCM_NONCE_LEN;           // Middle section: encrypted data
    uint8_t* tag        = outBuf + GCM_NONCE_LEN + plaintextLen; // Last 16 bytes: auth tag

    // Generate a fresh random nonce for this message.
    // IMPORTANT: The nonce must be unique for every message encrypted with the
    // same key. Because we use ephemeral ECDH keys (new key per message), the
    // key itself is always different, making nonce reuse impossible in practice.
    // The random nonce is additional safety.
    if (mbedtls_ctr_drbg_random(&_ctr_drbg, nonce, GCM_NONCE_LEN) != 0) return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    do {
        // Load the AES key (key size must be in bits, not bytes)
        if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                               key, AES_KEY_LEN * 8) != 0) break;

        // Encrypt and compute authentication tag in one call.
        // The nullptr / 0 arguments mean "no additional authenticated data (AAD)".
        // AAD would let us authenticate packet headers without encrypting them;
        // we don't need it here because the entire packet is encrypted.
        if (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                      plaintextLen,          // plaintext length
                                      nonce, GCM_NONCE_LEN,  // nonce
                                      nullptr, 0,            // no AAD
                                      plaintext, ciphertext, // input → output
                                      GCM_TAG_LEN, tag)      // tag length + where to write tag
                                      != 0) break;
        outLen = GCM_NONCE_LEN + plaintextLen + GCM_TAG_LEN; // total bytes written
        ok = true;
    } while (false);

    mbedtls_gcm_free(&gcm);
    return ok;
}


// ── cryptoDecrypt ──────────────────────────────────────────────────────────────
// Decrypts an AES-128-GCM message and verifies its authentication tag.
// Input buffer layout:  [nonce 12B] [ciphertext] [tag 16B]
// Caller must allocate outBuf with at least inLen - 28 bytes.
// `outLen` is set to the plaintext length on success.
//
// IMPORTANT: Returns false if the GCM tag does not match.
// This means either the key is wrong, the data was tampered with, or the
// nonce was reused. In all cases the caller must discard the output.
bool cryptoDecrypt(const uint8_t key[AES_KEY_LEN],
                   const uint8_t* inBuf, size_t inLen,
                   uint8_t* outBuf, size_t& outLen) {
    // Must have at least nonce + tag bytes to be a valid GCM message
    if (inLen <= GCM_NONCE_LEN + GCM_TAG_LEN) return false;

    // Parse the three sections from the flat input buffer
    const uint8_t* nonce      = inBuf;                               // First 12 bytes
    size_t         ctLen      = inLen - GCM_NONCE_LEN - GCM_TAG_LEN; // Ciphertext length
    const uint8_t* ciphertext = inBuf + GCM_NONCE_LEN;               // After nonce
    const uint8_t* tag        = inBuf + GCM_NONCE_LEN + ctLen;       // Last 16 bytes

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    do {
        if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                               key, AES_KEY_LEN * 8) != 0) break;

        // Decrypt AND verify the authentication tag atomically.
        // mbedtls_gcm_auth_decrypt returns non-zero if the tag does not match —
        // in that case outBuf should be treated as garbage and discarded.
        if (mbedtls_gcm_auth_decrypt(&gcm, ctLen,
                                     nonce, GCM_NONCE_LEN,  // nonce used during encrypt
                                     nullptr, 0,            // no AAD
                                     tag, GCM_TAG_LEN,      // expected auth tag
                                     ciphertext, outBuf)    // input → output
                                     != 0) break;           // non-zero = auth failure
        outLen = ctLen;   // plaintext length equals ciphertext length for GCM (stream cipher)
        ok = true;
    } while (false);

    mbedtls_gcm_free(&gcm);
    return ok;
}
