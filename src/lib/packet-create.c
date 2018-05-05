/*
 * Copyright (c) 2017, [Ribose Inc](https://www.ribose.com).
 * Copyright (c) 2009 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is originally derived from software contributed to
 * The NetBSD Foundation by Alistair Crooks (agc@netbsd.org), and
 * carried further by Ribose Inc (https://www.ribose.com).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) 2005-2008 Nominet UK (www.nic.uk)
 * All rights reserved.
 * Contributors: Ben Laurie, Rachel Willmer. The Contributors have asserted
 * their moral rights under the UK Copyright Design and Patents Act 1988 to
 * be recorded as the authors of this copyright work.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** \file
 */
#include "config.h"

#ifdef HAVE_SYS_CDEFS_H
#include <sys/cdefs.h>
#endif

#if defined(__NetBSD__)
__COPYRIGHT("@(#) Copyright (c) 2009 The NetBSD Foundation, Inc. All rights reserved.");
__RCSID("$NetBSD: create.c,v 1.38 2010/11/15 08:03:39 agc Exp $");
#endif

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <assert.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <rnp/rnp_def.h>
#include <rnp/rnp_sdk.h>

#include "crypto/common.h"
#include "crypto/s2k.h"
#include "signature.h"
#include "packet-create.h"
#include "memory.h"
#include "fingerprint.h"
#include "pgp-key.h"
#include "utils.h"
#include "writer.h"

/**
 * \ingroup Core_Create
 * \param length
 * \param type
 * \param output
 * \return 1 if OK, otherwise 0
 */

unsigned
pgp_write_ss_header(pgp_output_t *output, unsigned length, pgp_content_enum type)
{
    // add 1 here since length includes the 1-octet subpacket type
    return pgp_write_length(output, length + 1) &&
           pgp_write_scalar(
             output, (unsigned) (type - (unsigned) PGP_PTAG_SIG_SUBPKT_BASE), 1);
}

/**
 * \ingroup Core_WritePackets
 * \brief Writes a User Id packet
 * \param id
 * \param output
 * \return 1 if OK, otherwise 0
 */
unsigned
pgp_write_struct_userid(pgp_output_t *output, const uint8_t *id)
{
    return pgp_write_ptag(output, PGP_PTAG_CT_USER_ID) &&
           pgp_write_length(output, (unsigned) strlen((const char *) id)) &&
           pgp_write(output, id, (unsigned) strlen((const char *) id));
}

static bool
ec_serialize_pubkey(pgp_output_t *output, const pgp_ec_key_t *key)
{
    const ec_curve_desc_t *curve = get_curve_desc(key->curve);
    if (!curve) {
        return false;
    }

    return pgp_write_scalar(output, curve->OIDhex_len, 1) &&
           pgp_write(output, curve->OIDhex, curve->OIDhex_len) &&
           pgp_write_mpi(output, &key->p);
}

static size_t
mpi_length(const pgp_mpi_t *val)
{
    size_t res = mpi_bits(val);
    return 2 + ((res + 7) >> 3);
}

static unsigned
pubkey_length(const pgp_pubkey_t *key)
{
    switch (key->pkt.alg) {
    case PGP_PKA_ELGAMAL:
        return mpi_length(&key->pkt.material.eg.p) + mpi_length(&key->pkt.material.eg.g) +
               mpi_length(&key->pkt.material.eg.y);
    case PGP_PKA_DSA:
        return mpi_length(&key->pkt.material.dsa.p) + mpi_length(&key->pkt.material.dsa.q) +
               mpi_length(&key->pkt.material.dsa.g) + mpi_length(&key->pkt.material.dsa.y);
    case PGP_PKA_RSA:
        return mpi_length(&key->pkt.material.rsa.n) + mpi_length(&key->pkt.material.rsa.e);
    case PGP_PKA_ECDH: {
        const ec_curve_desc_t *c = get_curve_desc(key->pkt.material.ec.curve);
        if (!c) {
            RNP_LOG("Unknown curve");
            return 0;
        }
        /* OID len, OID, mpi, and 4 bytes of additional data (2 const, and kdf/hash algs) */
        return 1 + c->OIDhex_len + mpi_length(&key->pkt.material.ec.p) + 4;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2: {
        const ec_curve_desc_t *c = get_curve_desc(key->pkt.material.ec.curve);
        if (!c) {
            RNP_LOG("Unknown curve");
            return 0;
        }
        return 1 + c->OIDhex_len + mpi_length(&key->pkt.material.ec.p);
    }
    default:
        RNP_LOG("unknown key algorithm");
    }
    return 0;
}

static unsigned
seckey_length(const pgp_seckey_t *key)
{
    const pgp_key_material_t *kmaterial = &key->pubkey.pkt.material;
    switch (key->pubkey.pkt.alg) {
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2:
        return mpi_length(&kmaterial->ec.x) + pubkey_length(&key->pubkey);
    case PGP_PKA_DSA:
        return mpi_length(&kmaterial->dsa.x) + pubkey_length(&key->pubkey);
    case PGP_PKA_RSA:
        return mpi_length(&kmaterial->rsa.d) + mpi_length(&kmaterial->rsa.p) +
               mpi_length(&kmaterial->rsa.q) + mpi_length(&kmaterial->rsa.u) +
               pubkey_length(&key->pubkey);
    case PGP_PKA_ELGAMAL:
        return mpi_length(&kmaterial->eg.x) + pubkey_length(&key->pubkey);
    default:
        RNP_LOG("unknown key algorithm");
    }
    return 0;
}

/*
 * Note that we support v3 keys here because they're needed for for
 * verification - the writer doesn't allow them, though
 */

static bool
write_pubkey_body(const pgp_pubkey_t *key, pgp_output_t *output)
{
    if (!(pgp_write_scalar(output, (unsigned) key->pkt.version, 1) &&
          pgp_write_scalar(output, (unsigned) key->pkt.creation_time, 4))) {
        return false;
    }

    switch (key->pkt.version) {
    case PGP_V2:
    case PGP_V3:
        if (!pgp_write_scalar(output, (unsigned) key->pkt.v3_days, 2)) {
            return false;
        }
        break;
    case PGP_V4:
        break;
    default:
        RNP_LOG("invalid pubkey version: %d", (int) key->pkt.version);
        return false;
    }
    if (!pgp_write_scalar(output, (unsigned) key->pkt.alg, 1)) {
        return false;
    }

    switch (key->pkt.alg) {
    case PGP_PKA_DSA:
        return pgp_write_mpi(output, &key->pkt.material.dsa.p) &&
               pgp_write_mpi(output, &key->pkt.material.dsa.q) &&
               pgp_write_mpi(output, &key->pkt.material.dsa.g) &&
               pgp_write_mpi(output, &key->pkt.material.dsa.y);
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2:
        return ec_serialize_pubkey(output, &key->pkt.material.ec);
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        return pgp_write_mpi(output, &key->pkt.material.rsa.n) &&
               pgp_write_mpi(output, &key->pkt.material.rsa.e);
    case PGP_PKA_ECDH:
        return ec_serialize_pubkey(output, &key->pkt.material.ec) &&
               pgp_write_scalar(output, 3 /*size of following attributes*/, 1) &&
               pgp_write_scalar(output, 1 /*reserved*/, 1) &&
               pgp_write_scalar(output, (unsigned) key->pkt.material.ec.kdf_hash_alg, 1) &&
               pgp_write_scalar(output, (unsigned) key->pkt.material.ec.key_wrap_alg, 1);
    case PGP_PKA_ELGAMAL:
        return pgp_write_mpi(output, &key->pkt.material.eg.p) &&
               pgp_write_mpi(output, &key->pkt.material.eg.g) &&
               pgp_write_mpi(output, &key->pkt.material.eg.y);
    default:
        RNP_LOG("bad algorithm");
        break;
    }
    return false;
}

/* This starts writing right after the s2k usage. */
static bool
write_protected_seckey_body(pgp_output_t *output, pgp_seckey_t *seckey, const char *password)
{
    uint8_t     sesskey[PGP_MAX_KEY_SIZE];
    size_t      sesskey_size = pgp_key_size(seckey->pubkey.pkt.sec_protection.symm_alg);
    unsigned    block_size = pgp_block_size(seckey->pubkey.pkt.sec_protection.symm_alg);
    pgp_crypt_t crypt = {{{0}}, 0};
    pgp_hash_t  hash = {0};
    unsigned    writers_pushed = 0;
    bool        ret = false;
    pgp_key_protection_t *prot = &seckey->pubkey.pkt.sec_protection;

    // sanity checks
    if (prot->s2k.usage != PGP_S2KU_ENCRYPTED_AND_HASHED) {
        RNP_LOG("s2k usage");
        goto done;
    }
    if (prot->s2k.specifier != PGP_S2KS_SIMPLE && prot->s2k.specifier != PGP_S2KS_SALTED &&
        prot->s2k.specifier != PGP_S2KS_ITERATED_AND_SALTED) {
        RNP_LOG("invalid/unsupported s2k specifier %d", prot->s2k.specifier);
        goto done;
    }
    if (!sesskey_size || !block_size) {
        RNP_LOG("unknown encryption algorithm");
        goto done;
    }

    // start writing
    if (!pgp_write_scalar(output, (unsigned) prot->symm_alg, 1) ||
        !pgp_write_scalar(output, (unsigned) prot->s2k.specifier, 1) ||
        !pgp_write_scalar(output, (unsigned) prot->s2k.hash_alg, 1)) {
        RNP_LOG("writes failed");
        goto done;
    }

    // salt
    if (prot->s2k.specifier != PGP_S2KS_SIMPLE) {
        if (!rng_generate(prot->s2k.salt, PGP_SALT_SIZE)) {
            RNP_LOG("rng_generate failed");
            goto done;
        }
        if (!pgp_write(output, prot->s2k.salt, PGP_SALT_SIZE)) {
            goto done;
        }
    }

    // iterations
    if (prot->s2k.specifier == PGP_S2KS_ITERATED_AND_SALTED) {
        uint8_t enc_it = pgp_s2k_encode_iterations(prot->s2k.iterations);
        if (!pgp_write_scalar(output, enc_it, 1)) {
            RNP_LOG("write failed");
            goto done;
        }
    }

    // derive key
    if (!pgp_s2k_derive_key(&prot->s2k, password, sesskey, sesskey_size)) {
        RNP_LOG("failed to derive key");
        goto done;
    }

    // randomize and write IV
    if (!rng_generate(prot->iv, block_size)) {
        goto done;
    }
    if (!pgp_write(output, prot->iv, block_size)) {
        goto done;
    }

    // use the session key to encrypt
    if (!pgp_cipher_cfb_start(&crypt, prot->symm_alg, sesskey, prot->iv)) {
        goto done;
    }
    // debugging
    if (rnp_get_debug(__FILE__)) {
        hexdump(stderr, "writing: iv=", prot->iv, pgp_block_size(prot->symm_alg));
        hexdump(stderr, "key= ", sesskey, sesskey_size);
        (void) fprintf(stderr, "\nturning encryption on...\n");
    }
    if (!pgp_push_enc_crypt(output, &crypt)) {
        goto done;
    }
    writers_pushed++;

    // compute checkhash
    if (!pgp_hash_create(&hash, PGP_HASH_SHA1)) {
        goto done;
    }
    if (!pgp_writer_push_hash(output, &hash)) {
        goto done;
    }
    writers_pushed++;
    // write key material
    if (!pgp_write_secret_mpis(output, seckey)) {
        goto done;
    }
    pgp_writer_pop(output); // hash
    writers_pushed--;
    // write checkhash
    pgp_hash_finish(&hash, seckey->checkhash);
    if (!pgp_write(output, seckey->checkhash, PGP_CHECKHASH_SIZE)) {
        goto done;
    }

    ret = true;
done:
    // pop any remaining writers we pushed
    for (unsigned i = 0; i < writers_pushed; i++) {
        pgp_writer_pop(output);
    }
    pgp_cipher_cfb_finish(&crypt);
    return ret;
}

/*
 * Note that we support v3 keys here because they're needed for
 * verification.
 */
static bool
write_seckey_body(pgp_output_t *output, pgp_seckey_t *seckey, const char *password)
{
    /* RFC4880 Section 5.5.3 Secret-Key Packet Formats */

    if (!write_pubkey_body(&seckey->pubkey, output)) {
        RNP_LOG("failed to write pubkey body");
        return false;
    }
    if (!pgp_write_scalar(output, (unsigned) seckey->pubkey.pkt.sec_protection.s2k.usage, 1)) {
        RNP_LOG("failed tow rite s2k usage");
        return false;
    }
    switch (seckey->pubkey.pkt.sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
        if (!pgp_writer_push_sum16(output)) {
            RNP_LOG("failed to push checksum calculator");
            return false;
        }
        if (!pgp_write_secret_mpis(output, seckey)) {
            pgp_writer_pop(output); // sum16
            RNP_LOG("failed to write secret MPIs");
            return false;
        }
        seckey->checksum = pgp_writer_pop_sum16(output);
        if (!pgp_write_scalar(output, seckey->checksum, 2)) {
            RNP_LOG("failed to write checksum");
            return false;
        }
        break;
    case PGP_S2KU_ENCRYPTED_AND_HASHED:
        if (!write_protected_seckey_body(output, seckey, password)) {
            RNP_LOG("failed to write protected secret key body");
            return false;
        }
        break;
    default:
        RNP_LOG("unsupported s2k usage");
        return false;
    }
    return true;
}

/**
 * \ingroup Core_WritePackets
 * \brief Writes a Public Key packet
 * \param key
 * \param output
 * \return 1 if OK, otherwise 0
 */
bool
pgp_write_struct_pubkey(pgp_output_t *output, pgp_content_enum tag, const pgp_pubkey_t *key)
{
    return pgp_write_ptag(output, tag) &&
           pgp_write_length(output, 1 + 4 + 1 + pubkey_length(key)) &&
           write_pubkey_body(key, output);
}

static bool
packet_matches(const pgp_rawpacket_t *pkt, const pgp_content_enum tags[], size_t tag_count)
{
    for (size_t i = 0; i < tag_count; i++) {
        if (pkt->tag == tags[i]) {
            return true;
        }
    }
    return false;
}

static bool
write_matching_packets(pgp_output_t *         output,
                       const pgp_key_t *      key,
                       const rnp_key_store_t *keyring,
                       const pgp_content_enum tags[],
                       size_t                 tag_count)
{
    for (unsigned i = 0; i < key->packetc; i++) {
        pgp_rawpacket_t *pkt = &key->packets[i];

        if (!packet_matches(pkt, tags, tag_count)) {
            RNP_LOG("skipping packet with tag: %d", pkt->tag);
            continue;
        }
        if (!pgp_write(output, pkt->raw, (unsigned) pkt->length)) {
            return false;
        }
    }

    if (!keyring) {
        return true;
    }

    // Export subkeys
    pgp_io_t io = {.errs = stderr, .res = stdout, .outs = stdout};
    for (list_item *grip = list_front(key->subkey_grips); grip; grip = list_next(grip)) {
        const pgp_key_t *subkey =
          rnp_key_store_get_key_by_grip(&io, keyring, (uint8_t *) grip);
        if (!write_matching_packets(output, subkey, NULL, tags, tag_count)) {
            RNP_LOG("Error occured when exporting a subkey");
            return false;
        }
    }

    return true;
}

/**
   \ingroup HighLevel_KeyWrite

   \brief Writes a transferable PGP public key to the given output stream.

   \param key Key to be written
   \param armored Flag is set for armored output
   \param output Output stream

*/

unsigned
pgp_write_xfer_pubkey(pgp_output_t *         output,
                      const pgp_key_t *      key,
                      const rnp_key_store_t *keyring,
                      const unsigned         armored)
{
    static const pgp_content_enum permitted_tags[] = {PGP_PTAG_CT_PUBLIC_KEY,
                                                      PGP_PTAG_CT_PUBLIC_SUBKEY,
                                                      PGP_PTAG_CT_USER_ID,
                                                      PGP_PTAG_CT_SIGNATURE};
    if (armored) {
        pgp_writer_push_armored(output, PGP_PGP_PUBLIC_KEY_BLOCK);
    }
    if (!write_matching_packets(
          output, key, keyring, permitted_tags, ARRAY_SIZE(permitted_tags))) {
        return false;
    }
    if (armored) {
        pgp_writer_info_finalise(&output->errors, &output->writer);
        pgp_writer_pop(output);
    }
    return true;
}

/**
   \ingroup HighLevel_KeyWrite

   \brief Writes a transferable PGP secret key to the given output stream.

   \param key Key to be written
   \param password
   \param pplen
   \param armored Flag is set for armored output
   \param output Output stream

*/

bool
pgp_write_xfer_seckey(pgp_output_t *         output,
                      const pgp_key_t *      key,
                      const rnp_key_store_t *keyring,
                      unsigned               armored)
{
    static const pgp_content_enum permitted_tags[] = {PGP_PTAG_CT_SECRET_KEY,
                                                      PGP_PTAG_CT_SECRET_SUBKEY,
                                                      PGP_PTAG_CT_USER_ID,
                                                      PGP_PTAG_CT_SIGNATURE};

    if (!key->packetc || !key->packets) {
        return false;
    }

    if (armored) {
        pgp_writer_push_armored(output, PGP_PGP_PRIVATE_KEY_BLOCK);
    }
    if (!write_matching_packets(
          output, key, keyring, permitted_tags, ARRAY_SIZE(permitted_tags))) {
        return false;
    }
    if (armored) {
        pgp_writer_info_finalise(&output->errors, &output->writer);
        pgp_writer_pop(output);
    }
    return true;
}

/**
 * \ingroup Core_Create
 * \param out
 * \param key
 * \param make_packet
 */
bool
pgp_build_pubkey(pgp_memory_t *out, const pgp_pubkey_t *key, unsigned make_packet)
{
    pgp_output_t *output = NULL;
    bool          ret = false;

    output = pgp_output_new();
    if (output == NULL) {
        fprintf(stderr, "Can't allocate memory\n");
        goto done;
    }
    pgp_memory_init(out, 128);
    pgp_writer_set_memory(output, out);
    if (!write_pubkey_body(key, output)) {
        goto done;
    }
    if (make_packet) {
        pgp_memory_make_packet(out, PGP_PTAG_CT_PUBLIC_KEY);
    }
    ret = true;

done:
    if (!ret) {
        pgp_memory_release(out);
    }
    pgp_output_delete(output);
    return ret;
}

/**
 * \ingroup Core_WritePackets
 * \brief Writes a Secret Key packet.
 * \param key The secret key
 * \param password The password
 * \param pplen Length of password
 * \param output
 * \return 1 if OK; else 0
 */
unsigned
pgp_write_struct_seckey(pgp_output_t *   output,
                        pgp_content_enum tag,
                        pgp_seckey_t *   seckey,
                        const char *     password)
{
    unsigned length = 0;

    if (seckey->pubkey.pkt.version != 4) {
        (void) fprintf(stderr, "pgp_write_struct_seckey: public key version\n");
        return false;
    }

    /* Ref: RFC4880 Section 5.5.3 */

    /* pubkey, excluding MPIs */
    length += 1 + 4 + 1;

    /* s2k usage */
    length += 1;

    switch (seckey->pubkey.pkt.sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
        /* nothing to add */
        break;

    case PGP_S2KU_ENCRYPTED_AND_HASHED: /* 254 */
    case PGP_S2KU_ENCRYPTED:            /* 255 */

        /* Ref: RFC4880 Section 3.7 */
        length += 1; /* symm alg */
        length += 1; /* s2k_specifier */

        switch (seckey->pubkey.pkt.sec_protection.s2k.specifier) {
        case PGP_S2KS_SIMPLE:
            length += 1; /* hash algorithm */
            break;

        case PGP_S2KS_SALTED:
            length += 1 + 8; /* hash algorithm + salt */
            break;

        case PGP_S2KS_ITERATED_AND_SALTED:
            length += 1 + 8 + 1; /* hash algorithm, salt +
                                  * count */
            break;

        default:
            (void) fprintf(stderr, "pgp_write_struct_seckey: s2k spec\n");
            return false;
        }
        break;

    default:
        (void) fprintf(stderr, "pgp_write_struct_seckey: s2k usage\n");
        return false;
    }

    /* IV */
    if (seckey->pubkey.pkt.sec_protection.s2k.usage) {
        length += pgp_block_size(seckey->pubkey.pkt.sec_protection.symm_alg);
    }
    /* checksum or hash */
    switch (seckey->pubkey.pkt.sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
    case PGP_S2KU_ENCRYPTED:
        length += 2;
        break;

    case PGP_S2KU_ENCRYPTED_AND_HASHED:
        length += PGP_CHECKHASH_SIZE;
        break;

    default:
        (void) fprintf(stderr, "pgp_write_struct_seckey: s2k cksum usage\n");
        return false;
    }

    /* secret key and public key MPIs */
    length += (unsigned) seckey_length(seckey);

    return pgp_write_ptag(output, tag) && pgp_write_length(output, (unsigned) length) &&
           write_seckey_body(output, seckey, password);
}

/**
 * \ingroup Core_Create
 *
 * \brief Create a new pgp_output_t structure.
 *
 * \return the new structure.
 * \note It is the responsiblity of the caller to call pgp_output_delete().
 * \sa pgp_output_delete()
 */
pgp_output_t *
pgp_output_new(void)
{
    return calloc(1, sizeof(pgp_output_t));
}

/**
 * \ingroup Core_Create
 * \brief Delete an pgp_output_t strucut and associated resources.
 *
 * Delete an pgp_output_t structure. If a writer is active, then
 * that is also deleted.
 *
 * \param info the structure to be deleted.
 */
void
pgp_output_delete(pgp_output_t *output)
{
    if (!output) {
        return;
    }
    pgp_writer_info_delete(&output->writer);
    free(output);
}

bool
pgp_write_selfsig_cert(pgp_output_t *               output,
                       const pgp_seckey_t *         seckey,
                       const pgp_hash_alg_t         hash_alg,
                       const rnp_selfsig_cert_info *cert)
{
    pgp_create_sig_t *sig = NULL;
    bool              ok = false;
    uint8_t           keyid[PGP_KEY_ID_SIZE];
    rng_t             rng = {0};

    if (!output || !seckey || !cert) {
        RNP_LOG("invalid parameters");
        return false;
    }

    if (!rng_init(&rng, RNG_SYSTEM)) {
        RNP_LOG("RNG init failed");
        return false;
    }

    if (pgp_keyid(keyid, sizeof(keyid), &seckey->pubkey)) {
        RNP_LOG("failed to calculate keyid");
        goto end;
    }

    sig = pgp_create_sig_new();
    if (!sig) {
        RNP_LOG("create sig failed");
        goto end;
    }
    if (!pgp_sig_start_key_sig(
          sig, &seckey->pubkey, cert->userid, PGP_CERT_POSITIVE, hash_alg)) {
        RNP_LOG("failed to start key sig");
        goto end;
    }
    if (!pgp_sig_add_time(sig, (int64_t) time(NULL), PGP_PTAG_SS_CREATION_TIME)) {
        RNP_LOG("failed to add creation time");
        goto end;
    }
    if (cert->key_expiration &&
        !pgp_sig_add_time(sig, cert->key_expiration, PGP_PTAG_SS_KEY_EXPIRY)) {
        RNP_LOG("failed to add key expiration time");
        goto end;
    }
    if (cert->key_flags && !pgp_sig_add_key_flags(sig, &cert->key_flags, 1)) {
        RNP_LOG("failed to add key flags");
        goto end;
    }
    if (cert->primary) {
        pgp_sig_add_primary_userid(sig, 1);
    }
    const pgp_user_prefs_t *prefs = &cert->prefs;
    if (!DYNARRAY_IS_EMPTY(prefs, symm_alg) &&
        !pgp_sig_add_pref_symm_algs(sig, prefs->symm_algs, prefs->symm_algc)) {
        RNP_LOG("failed to add symm alg prefs");
        goto end;
    }
    if (!DYNARRAY_IS_EMPTY(prefs, hash_alg) &&
        !pgp_sig_add_pref_hash_algs(sig, prefs->hash_algs, prefs->hash_algc)) {
        RNP_LOG("failed to add hash alg prefs");
        goto end;
    }
    if (!DYNARRAY_IS_EMPTY(prefs, compress_alg) &&
        !pgp_sig_add_pref_compress_algs(sig, prefs->compress_algs, prefs->compress_algc)) {
        RNP_LOG("failed to add compress alg prefs");
        goto end;
    }
    if (!DYNARRAY_IS_EMPTY(prefs, key_server_pref) &&
        !pgp_sig_add_key_server_prefs(sig, prefs->key_server_prefs, prefs->key_server_prefc)) {
        RNP_LOG("failed to add key server prefs");
        goto end;
    }
    if (prefs->key_server && !pgp_sig_add_preferred_key_server(sig, prefs->key_server)) {
        RNP_LOG("failed to add preferred key server");
        goto end;
    }
    if (!pgp_sig_add_issuer_keyid(sig, keyid)) {
        RNP_LOG("failed to add issuer key id");
        goto end;
    }
    if (!pgp_sig_end_hashed_subpkts(sig)) {
        RNP_LOG("failed to finalize hashed subpkts");
        goto end;
    }

    if (!pgp_sig_write(&rng, output, sig, &seckey->pubkey, seckey)) {
        RNP_LOG("failed to write signature");
        goto end;
    }
    ok = true;
end:
    rng_destroy(&rng);
    pgp_create_sig_delete(sig);
    return ok;
}

bool
pgp_write_selfsig_binding(pgp_output_t *                  output,
                          const pgp_seckey_t *            primary_sec,
                          const pgp_hash_alg_t            hash_alg,
                          const pgp_pubkey_t *            subkey,
                          const rnp_selfsig_binding_info *binding)
{
    pgp_create_sig_t *sig = NULL;
    bool              ok = false;
    uint8_t           keyid[PGP_KEY_ID_SIZE];
    rng_t             rng = {0};

    if (!output || !primary_sec || !subkey || !binding) {
        RNP_LOG("invalid parameters");
        goto end;
    }

    if (!rng_init(&rng, RNG_SYSTEM)) {
        RNP_LOG("RNG init failed");
        return false;
    }

    if (pgp_keyid(keyid, sizeof(keyid), &primary_sec->pubkey)) {
        RNP_LOG("failed to calculate keyid");
        goto end;
    }

    sig = pgp_create_sig_new();
    if (!sig) {
        RNP_LOG("create sig failed");
        goto end;
    }
    if (!pgp_sig_start_subkey_sig(
          sig, &primary_sec->pubkey, subkey, PGP_SIG_SUBKEY, hash_alg)) {
        RNP_LOG("failed to start subkey sig");
        goto end;
    }
    if (!pgp_sig_add_time(sig, (int64_t) time(NULL), PGP_PTAG_SS_CREATION_TIME)) {
        RNP_LOG("failed to add creation time");
        goto end;
    }
    if (binding->key_expiration &&
        !pgp_sig_add_time(sig, binding->key_expiration, PGP_PTAG_SS_KEY_EXPIRY)) {
        RNP_LOG("failed to add key expiration time");
        goto end;
    }
    if (binding->key_flags && !pgp_sig_add_key_flags(sig, &binding->key_flags, 1)) {
        RNP_LOG("failed to add key flags");
        goto end;
    }
    if (!pgp_sig_add_issuer_keyid(sig, keyid)) {
        RNP_LOG("failed to add issuer key id");
        goto end;
    }
    if (!pgp_sig_end_hashed_subpkts(sig)) {
        RNP_LOG("failed to finalize hashed subpkts");
        goto end;
    }

    if (!pgp_sig_write(&rng, output, sig, &primary_sec->pubkey, primary_sec)) {
        RNP_LOG("failed to write signature");
        goto end;
    }
    ok = true;
end:
    rng_destroy(&rng);
    pgp_create_sig_delete(sig);
    return ok;
}

bool
pgp_write_secret_mpis(pgp_output_t *output, const pgp_seckey_t *seckey)
{
    bool ok = false;

    switch (seckey->pubkey.pkt.alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        ok = pgp_write_mpi(output, &seckey->pubkey.pkt.material.rsa.d) &&
             pgp_write_mpi(output, &seckey->pubkey.pkt.material.rsa.p) &&
             pgp_write_mpi(output, &seckey->pubkey.pkt.material.rsa.q) &&
             pgp_write_mpi(output, &seckey->pubkey.pkt.material.rsa.u);
        break;

    case PGP_PKA_DSA:
        ok = pgp_write_mpi(output, &seckey->pubkey.pkt.material.dsa.x);
        break;

    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2:
    case PGP_PKA_ECDH:
        ok = pgp_write_mpi(output, &seckey->pubkey.pkt.material.ec.x);
        break;

    case PGP_PKA_ELGAMAL:
        ok = pgp_write_mpi(output, &seckey->pubkey.pkt.material.eg.x);
        break;

    default:
        RNP_LOG("unsupported pk alg %d", seckey->pubkey.pkt.alg);
        break;
    }
    if (!ok) {
        RNP_LOG("failed to write MPIs for pk alg %d", seckey->pubkey.pkt.alg);
    }
    return ok;
}
