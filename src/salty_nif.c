/**
 * Copyright 2017 Jan van de Molengraft <jan@artemisc.eu>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include "sodium.h"
#include "erl_nif.h"

/*******************************************************************************
 *
 * IMPORTANT NOTE
 *
 * The following macros require that every method using them defines its
 * arguments using the same names.
 *
 * (ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
 *
 ******************************************************************************/

/* SALTY_MAX_CLEAN_SIZE is the maximum amount of bytes a nif call can process
 * before we should consider invoking the dirty schedulers.
 *
 * TODO validate this number, and use it.
 * TODO can dirty schedulers be called at runtime?
 */
#define SALTY_MAX_CLEAN_SIZE (16 * 1024)

#define SALTY_NOERR 0
#define SALTY_BADARG enif_make_badarg(env)
#define SALTY_BADALLOC SALTY_BADARG /* TODO make this return an actual failed-to-alloc error */

#define SALTY_ERROR tuple_error_unknown
#define SALTY_ERROR_PAIR(err) enif_make_tuple2(env, atom_error, err)
#define SALTY_OK atom_ok
#define SALTY_OK_PAIR(a) enif_make_tuple2(env, atom_ok, a)
#define SALTY_OK_TRIPLET(a, b) enif_make_tuple3(env, atom_ok, a, b)

#define SALTY_BIN_NO_SIZE 0
#define SALTY_INPUT_BIN(index, dst, len) \
    ErlNifBinary dst; \
    if (!enif_inspect_binary(env, argv[index], &dst)) { \
        if (!enif_inspect_iolist_as_binary(env, argv[index], &dst)) { \
            return (SALTY_BADARG); \
        } \
    } \
    if (dst.size < len) { \
        return (SALTY_BADARG); \
    }

/* TODO implement this macro */
#define SALTY_INPUT_RES(index, dst) \
    ErlNifResourceType

#define SALTY_OUTPUT_BIN(dst, len) \
    ErlNifBinary dst; \
    if (!enif_alloc_binary(len, &dst)) { \
        return (SALTY_BADALLOC); \
    }

#define OUT(a) enif_make_binary(env, &a)
#define SALTY_FUNC(name, args) \
    static ERL_NIF_TERM \
    salty_##name (ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) { \
        if (argc != args) { \
            return (SALTY_BADARG); \
        }
#define DO
#define END }
#define END_OK return (SALTY_OK); END
#define END_OK_WITH(out) return (SALTY_OK_PAIR(OUT(out))); END

#define SALTY_CONST_INT64(name) \
    static ERL_NIF_TERM \
    salty_##name (ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) { \
        return enif_make_int64(env, crypto_##name); \
    }

#define SALTY_EXPORT_NAME(name) #name
#define SALTY_EXPORT_FUNC(name, args) { SALTY_EXPORT_NAME(name), args, salty_##name }
#define SALTY_EXPORT_FUNC_DIRTY(name, args) { SALTY_EXPORT_NAME(name), args, salty_##name, ERL_NIF_DIRTY_JOB_CPU_BOUND }

#define SALTY_CALL_SIMPLE(call) \
    if (( call ) != SALTY_NOERR) { \
        return (SALTY_ERROR); \
    }

#define SALTY_CALL_SIMPLE_WITHERR(call, error) \
    if (( call ) != SALTY_NOERR) { \
        return (SALTY_ERROR_PAIR(error)); \
    }

#define SALTY_CALL(call, output) \
    if (( call ) != SALTY_NOERR) { \
        enif_release_binary(&output); \
        return (SALTY_ERROR); \
    }

#define SALTY_CALL_WITHERR(call, error, output) \
    if (( call ) != SALTY_NOERR) { \
        enif_release_binary(&output); \
        return (SALTY_ERROR_PAIR(error)); \
    }

/* STATIC VALUES */
ERL_NIF_TERM atom_ok;
ERL_NIF_TERM atom_error;
ERL_NIF_TERM atom_error_no_match;
ERL_NIF_TERM atom_error_not_available;
ERL_NIF_TERM atom_error_forged;
ERL_NIF_TERM atom_error_unknown;
ERL_NIF_TERM tuple_error_unknown;

/*
TODO is useful to create this through the nif code?
ERL_NIF_TERM atom_primitive_auth;
ERL_NIF_TERM atom_primitive_box;
ERL_NIF_TERM atom_primitive_secretbox;
ERL_NIF_TERM atom_primitive_sign;*/

/* erl_nif code */
static int
salty_onload(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info) {
    /* register the safe resource types for keys and private data */
    
    /* cache atom values */
    atom_ok                  = enif_make_atom(env, "ok");
    atom_error               = enif_make_atom(env, "error");
    atom_error_no_match      = enif_make_atom(env, "no_match");
    atom_error_not_available = enif_make_atom(env, "not_available");
    atom_error_forged        = enif_make_atom(env, "forged");
    atom_error_unknown       = enif_make_atom(env, "salty_error_unknown");
    tuple_error_unknown      = enif_make_tuple2(env, atom_error, atom_error_unknown);

    return 0;
}

/**
 * Sodium internal
 */

/* sodium_init */
SALTY_FUNC(init, 0) DO
    SALTY_CALL_SIMPLE(sodium_init());
END_OK

/* sodium_memcmp */
SALTY_FUNC(memcmp, 2) DO
    SALTY_INPUT_BIN(0, a, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, b, SALTY_BIN_NO_SIZE);

    if (a.size != b.size) {
        return (SALTY_ERROR);
    }

    SALTY_CALL_SIMPLE(sodium_memcmp(a.data, b.data, a.size));
END_OK

/* safe key-gen (using locked memory binary resource) */

/**
 * AEAD aes256gcm
 */
SALTY_CONST_INT64(aead_aes256gcm_KEYBYTES);
SALTY_CONST_INT64(aead_aes256gcm_NSECBYTES);
SALTY_CONST_INT64(aead_aes256gcm_NPUBBYTES);
SALTY_CONST_INT64(aead_aes256gcm_ABYTES);

SALTY_FUNC(aead_aes256gcm_is_available, 0) DO
    if (crypto_aead_aes256gcm_is_available() == 0) {
        return (SALTY_ERROR_PAIR(atom_error_not_available));
    }
END_OK


SALTY_FUNC(aead_aes256gcm_encrypt, 5) DO
    SALTY_INPUT_BIN(0, plain, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, ad,    SALTY_BIN_NO_SIZE);
    /*SALTY_INPUT_BIN(2, nsec,  crypto_aead_aes256gcm_NSECBYTES);*/
    SALTY_INPUT_BIN(3, npub,  crypto_aead_aes256gcm_NPUBBYTES);
    SALTY_INPUT_BIN(4, key,   crypto_aead_aes256gcm_KEYBYTES);

    SALTY_OUTPUT_BIN(cipher, crypto_aead_aes256gcm_ABYTES + plain.size);

    SALTY_CALL(crypto_aead_aes256gcm_encrypt(
                cipher.data, NULL, plain.data, plain.size, ad.data, ad.size,
                NULL, npub.data, key.data), cipher);
END_OK_WITH(cipher)

SALTY_FUNC(aead_aes256gcm_decrypt_detached, 6) DO
    /*SALTY_INPUT_BIN(0, nsec,   crypto_aead_aes256gcm_NSECBYTES);*/
    SALTY_INPUT_BIN(1, cipher, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, mac,    crypto_aead_aes256gcm_ABYTES);
    SALTY_INPUT_BIN(3, ad,     SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(4, npub,   crypto_aead_aes256gcm_NPUBBYTES);
    SALTY_INPUT_BIN(5, key,    crypto_aead_aes256gcm_KEYBYTES);

    SALTY_OUTPUT_BIN(plain, cipher.size);

    SALTY_CALL_WITHERR(crypto_aead_aes256gcm_decrypt_detached(
                plain.data, NULL, cipher.data, cipher.size, mac.data,
                ad.data, ad.size, npub.data, key.data),
                atom_error_forged, plain);
END_OK_WITH(plain)

/**
 * AEAD chacha20poly1305
 */
SALTY_CONST_INT64(aead_chacha20poly1305_KEYBYTES);
SALTY_CONST_INT64(aead_chacha20poly1305_NSECBYTES);
SALTY_CONST_INT64(aead_chacha20poly1305_NPUBBYTES);
SALTY_CONST_INT64(aead_chacha20poly1305_ABYTES);

SALTY_FUNC(aead_chacha20poly1305_encrypt, 5) DO
    SALTY_INPUT_BIN(0, plain, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, ad,    SALTY_BIN_NO_SIZE);
    /*SALTY_INPUT_BIN(2, nsec,  crypto_aead_chacha20poly1305_NSECBYTES);*/
    SALTY_INPUT_BIN(3, npub,  crypto_aead_chacha20poly1305_NPUBBYTES);
    SALTY_INPUT_BIN(4, key,   crypto_aead_chacha20poly1305_KEYBYTES);

    SALTY_OUTPUT_BIN(cipher, crypto_aead_chacha20poly1305_ABYTES + plain.size);

    SALTY_CALL(crypto_aead_chacha20poly1305_encrypt(
                cipher.data, NULL, plain.data, plain.size, ad.data, ad.size,
                NULL, npub.data, key.data), cipher);
END_OK_WITH(cipher)

SALTY_FUNC(aead_chacha20poly1305_decrypt_detached, 6) DO
    /*SALTY_INPUT_BIN(0, nsec,   crypto_aead_chacha20poly1305_NSECBYTES);*/
    SALTY_INPUT_BIN(1, cipher, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, mac,    crypto_aead_chacha20poly1305_ABYTES);
    SALTY_INPUT_BIN(3, ad,     SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(4, npub,   crypto_aead_chacha20poly1305_NPUBBYTES);
    SALTY_INPUT_BIN(5, key,    crypto_aead_chacha20poly1305_KEYBYTES);

    SALTY_OUTPUT_BIN(plain, cipher.size);

    SALTY_CALL_WITHERR(crypto_aead_chacha20poly1305_decrypt_detached(
                plain.data, NULL, cipher.data, cipher.size, mac.data,
                ad.data, ad.size, npub.data, key.data),
                atom_error_forged, plain);
END_OK_WITH(plain)

/**
 * AEAD xchacha20poly1305_ietf
 */
SALTY_CONST_INT64(aead_xchacha20poly1305_ietf_KEYBYTES);
SALTY_CONST_INT64(aead_xchacha20poly1305_ietf_NSECBYTES);
SALTY_CONST_INT64(aead_xchacha20poly1305_ietf_NPUBBYTES);
SALTY_CONST_INT64(aead_xchacha20poly1305_ietf_ABYTES);

SALTY_FUNC(aead_xchacha20poly1305_ietf_encrypt, 5) DO
    SALTY_INPUT_BIN(0, plain, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, ad,    SALTY_BIN_NO_SIZE);
    /*SALTY_INPUT_BIN(2, nsec,  crypto_aead_xchacha20poly1305_ietf_NSECBYTES);*/
    SALTY_INPUT_BIN(3, npub,  crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    SALTY_INPUT_BIN(4, key,   crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    SALTY_OUTPUT_BIN(cipher, crypto_aead_xchacha20poly1305_ietf_ABYTES + plain.size);

    SALTY_CALL(crypto_aead_xchacha20poly1305_ietf_encrypt(
                cipher.data, NULL, plain.data, plain.size, ad.data, ad.size,
                NULL, npub.data, key.data), cipher);
END_OK_WITH(cipher)

SALTY_FUNC(aead_xchacha20poly1305_ietf_decrypt_detached, 6) DO
    /*SALTY_INPUT_BIN(0, nsec,   crypto_aead_xchacha20poly1305_ietf_NSECBYTES);*/
    SALTY_INPUT_BIN(1, cipher, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, mac,    crypto_aead_xchacha20poly1305_ietf_ABYTES);
    SALTY_INPUT_BIN(3, ad,     SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(4, npub,   crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    SALTY_INPUT_BIN(5, key,    crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    SALTY_OUTPUT_BIN(plain, cipher.size);

    SALTY_CALL_WITHERR(crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
                plain.data, NULL, cipher.data, cipher.size, mac.data,
                ad.data, ad.size, npub.data, key.data),
                atom_error_forged, plain);
END_OK_WITH(plain)

/**
 * AUTH hmacsha256
 */
SALTY_CONST_INT64(auth_hmacsha256_BYTES);
SALTY_CONST_INT64(auth_hmacsha256_KEYBYTES);

SALTY_FUNC(auth_hmacsha256, 2) DO
    SALTY_INPUT_BIN(0, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, key, crypto_auth_hmacsha256_KEYBYTES);

    SALTY_OUTPUT_BIN(mac, crypto_auth_hmacsha256_BYTES);

    SALTY_CALL(crypto_auth_hmacsha256(mac.data, msg.data, msg.size, key.data), mac);
END_OK_WITH(mac);

SALTY_FUNC(auth_hmacsha256_verify, 3) DO
    SALTY_INPUT_BIN(0, mac, crypto_auth_hmacsha256_BYTES);
    SALTY_INPUT_BIN(1, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, key, crypto_auth_hmacsha256_KEYBYTES);

    SALTY_CALL_SIMPLE_WITHERR(crypto_auth_hmacsha256_verify(
                mac.data, msg.data, msg.size, key.data),
            atom_error_no_match);
END_OK

/**
 * AUTH hmacsha512
 */
SALTY_CONST_INT64(auth_hmacsha512_BYTES);
SALTY_CONST_INT64(auth_hmacsha512_KEYBYTES);

SALTY_FUNC(auth_hmacsha512, 2) DO
    SALTY_INPUT_BIN(0, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, key, crypto_auth_hmacsha512_KEYBYTES);

    SALTY_OUTPUT_BIN(mac, crypto_auth_hmacsha512_BYTES);

    SALTY_CALL(crypto_auth_hmacsha512(mac.data, msg.data, msg.size, key.data), mac);
END_OK_WITH(mac);

SALTY_FUNC(auth_hmacsha512_verify, 3) DO
    SALTY_INPUT_BIN(0, mac, crypto_auth_hmacsha512_BYTES);
    SALTY_INPUT_BIN(1, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, key, crypto_auth_hmacsha512_KEYBYTES);

    SALTY_CALL_SIMPLE_WITHERR(crypto_auth_hmacsha512_verify(
                mac.data, msg.data, msg.size, key.data),
            atom_error_no_match);
END_OK

/**
 * AUTH hmacsha512256
 */
SALTY_CONST_INT64(auth_hmacsha512256_BYTES);
SALTY_CONST_INT64(auth_hmacsha512256_KEYBYTES);

SALTY_FUNC(auth_hmacsha512256, 2) DO
    SALTY_INPUT_BIN(0, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(1, key, crypto_auth_hmacsha512256_KEYBYTES);

    SALTY_OUTPUT_BIN(mac, crypto_auth_hmacsha512256_BYTES);

    SALTY_CALL(crypto_auth_hmacsha512256(mac.data, msg.data, msg.size, key.data), mac);
END_OK_WITH(mac)

SALTY_FUNC(auth_hmacsha512256_verify, 3) DO
    SALTY_INPUT_BIN(0, mac, crypto_auth_hmacsha512256_BYTES);
    SALTY_INPUT_BIN(1, msg, SALTY_BIN_NO_SIZE);
    SALTY_INPUT_BIN(2, key, crypto_auth_hmacsha512256_KEYBYTES);

    SALTY_CALL_SIMPLE_WITHERR(crypto_auth_hmacsha512256_verify(
                mac.data, msg.data, msg.size, key.data),
            atom_error_no_match);
END_OK

/**
 * CORE hchacha20
 */
SALTY_FUNC(core_hchacha20, 3) DO
    SALTY_INPUT_BIN(0, in, crypto_core_hchacha20_INPUTBYTES);
    SALTY_INPUT_BIN(1, key, crypto_core_hchacha20_KEYBYTES);
    SALTY_INPUT_BIN(2, con, crypto_core_hchacha20_CONSTBYTES);

    SALTY_OUTPUT_BIN(out, crypto_core_hchacha20_OUTPUTBYTES);

    SALTY_CALL(crypto_core_hchacha20(out.data, in.data, key.data, con.data), out);
END_OK_WITH(out)
/**
 * CORE hsalsa20
 */
SALTY_FUNC(core_hsalsa20, 3) DO
    SALTY_INPUT_BIN(0, in, crypto_core_hsalsa20_INPUTBYTES);
    SALTY_INPUT_BIN(1, key, crypto_core_hsalsa20_KEYBYTES);
    SALTY_INPUT_BIN(2, con, crypto_core_hsalsa20_CONSTBYTES);

    SALTY_OUTPUT_BIN(out, crypto_core_hsalsa20_OUTPUTBYTES);

    SALTY_CALL(crypto_core_hsalsa20(out.data, in.data, key.data, con.data), out);
END_OK_WITH(out)


/***********************************************
 * export
 ***********************************************/

static ErlNifFunc
salty_exports[] = {
    SALTY_EXPORT_FUNC(init, 0),
    SALTY_EXPORT_FUNC(memcmp, 2),

    SALTY_EXPORT_FUNC(aead_aes256gcm_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(aead_aes256gcm_NSECBYTES, 0),
    SALTY_EXPORT_FUNC(aead_aes256gcm_NPUBBYTES, 0),
    SALTY_EXPORT_FUNC(aead_aes256gcm_ABYTES, 0),
    SALTY_EXPORT_FUNC(aead_aes256gcm_is_available, 0),
    SALTY_EXPORT_FUNC(aead_aes256gcm_encrypt, 5),
    SALTY_EXPORT_FUNC(aead_aes256gcm_decrypt_detached, 6),

    SALTY_EXPORT_FUNC(aead_chacha20poly1305_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(aead_chacha20poly1305_NSECBYTES, 0),
    SALTY_EXPORT_FUNC(aead_chacha20poly1305_NPUBBYTES, 0),
    SALTY_EXPORT_FUNC(aead_chacha20poly1305_ABYTES, 0),
    SALTY_EXPORT_FUNC(aead_chacha20poly1305_encrypt, 5),
    SALTY_EXPORT_FUNC(aead_chacha20poly1305_decrypt_detached, 6),
    
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_NSECBYTES, 0),
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_NPUBBYTES, 0),
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_ABYTES, 0),
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_encrypt, 5),
    SALTY_EXPORT_FUNC(aead_xchacha20poly1305_ietf_decrypt_detached, 6),

    SALTY_EXPORT_FUNC(auth_hmacsha256_BYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha256_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha256, 2),
    SALTY_EXPORT_FUNC(auth_hmacsha256_verify, 3),

    SALTY_EXPORT_FUNC(auth_hmacsha512_BYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha512_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha512, 2),
    SALTY_EXPORT_FUNC(auth_hmacsha512_verify, 3),

    SALTY_EXPORT_FUNC(auth_hmacsha512256_BYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha512256_KEYBYTES, 0),
    SALTY_EXPORT_FUNC(auth_hmacsha512256, 2),
    SALTY_EXPORT_FUNC(auth_hmacsha512256_verify, 3),

    SALTY_EXPORT_FUNC(core_hchacha20, 3),
    SALTY_EXPORT_FUNC(core_hsalsa20, 3),
};

ERL_NIF_INIT(Elixir.Salty.Nif, salty_exports, salty_onload, NULL, NULL, NULL)