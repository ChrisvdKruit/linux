/*******************************************************************************
 * This file houses the main functions for the iSCSI CHAP support
 *
 * © Copyright 2007-2011 RisingTide Systems LLC.
 *
 * Licensed to the Linux Foundation under the General Public License (GPL) version 2.
 *
 * Author: Nicholas A. Bellinger <nab@linux-iscsi.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ******************************************************************************/

#include <linux/string.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/scatterlist.h>

#include "iscsi_target_core.h"
#include "iscsi_target_nego.h"
#include "iscsi_target_auth.h"

#ifdef DEBUG_CHAP
#define PRINT(x...)		printk(KERN_INFO x)
#else
#define PRINT(x...)
#endif

static unsigned char chap_asciihex_to_binaryhex(unsigned char val[2])
{
	unsigned char result = 0;
	/*
	 * MSB
	 */
	if ((val[0] >= 'a') && (val[0] <= 'f'))
		result = ((val[0] - 'a' + 10) & 0xf) << 4;
	else
		if ((val[0] >= 'A') && (val[0] <= 'F'))
			result = ((val[0] - 'A' + 10) & 0xf) << 4;
		else /* digit */
			result = ((val[0] - '0') & 0xf) << 4;
	/*
	 * LSB
	 */
	if ((val[1] >= 'a') && (val[1] <= 'f'))
		result |= ((val[1] - 'a' + 10) & 0xf);
	else
		if ((val[1] >= 'A') && (val[1] <= 'F'))
			result |= ((val[1] - 'A' + 10) & 0xf);
		else /* digit */
			result |= ((val[1] - '0') & 0xf);

	return result;
}

static int chap_string_to_hex(unsigned char *dst, unsigned char *src, int len)
{
	int i, j = 0;

	for (i = 0; i < len; i += 2) {
		dst[j++] = (unsigned char) chap_asciihex_to_binaryhex(&src[i]);
	}

	dst[j] = '\0';
	return j;
}

static void chap_binaryhex_to_asciihex(char *dst, char *src, int src_len)
{
	int i;

	for (i = 0; i < src_len; i++) {
		sprintf(&dst[i*2], "%02x", (int) src[i] & 0xff);
	}
}

static void chap_set_random(char *data, int length)
{
	long r;
	unsigned n;

	while (length > 0) {
		get_random_bytes(&r, sizeof(long));
		r = r ^ (r >> 8);
		r = r ^ (r >> 4);
		n = r & 0x7;

		get_random_bytes(&r, sizeof(long));
		r = r ^ (r >> 8);
		r = r ^ (r >> 5);
		n = (n << 3) | (r & 0x7);

		get_random_bytes(&r, sizeof(long));
		r = r ^ (r >> 8);
		r = r ^ (r >> 5);
		n = (n << 2) | (r & 0x3);

		*data++ = n;
		length--;
	}
}

static void chap_gen_challenge(
	struct iscsi_conn *conn,
	int caller,
	char *c_str,
	unsigned int *c_len)
{
	unsigned char challenge_asciihex[CHAP_CHALLENGE_LENGTH * 2 + 1];
	struct iscsi_chap *chap = (struct iscsi_chap *) conn->auth_protocol;

	memset(challenge_asciihex, 0, CHAP_CHALLENGE_LENGTH * 2 + 1);

	chap_set_random(chap->challenge, CHAP_CHALLENGE_LENGTH);
	chap_binaryhex_to_asciihex(challenge_asciihex, chap->challenge,
				CHAP_CHALLENGE_LENGTH);
	/*
	 * Set CHAP_C, and copy the generated challenge into c_str.
	 */
	*c_len += sprintf(c_str + *c_len, "CHAP_C=0x%s", challenge_asciihex);
	*c_len += 1;

	PRINT("[%s] Sending CHAP_C=0x%s\n\n", (caller) ? "server" : "client",
			challenge_asciihex);
}


static struct iscsi_chap *chap_server_open(
	struct iscsi_conn *conn,
	struct iscsi_node_auth *auth,
	const char *a_str,
	char *aic_str,
	unsigned int *aic_len)
{
	struct iscsi_chap *chap;

	if (!(auth->naf_flags & NAF_USERID_SET) ||
	    !(auth->naf_flags & NAF_PASSWORD_SET)) {
		printk(KERN_ERR "CHAP user or password not set for"
				" Initiator ACL\n");
		return NULL;
	}

	conn->auth_protocol = kzalloc(sizeof(struct iscsi_chap), GFP_KERNEL);
	if (!conn->auth_protocol)
		return NULL;

	chap = (struct iscsi_chap *) conn->auth_protocol;
	/*
	 * We only support MD5 MDA presently.
	 */
	if (strncmp(a_str, "CHAP_A=5", 8)) {
		printk(KERN_ERR "CHAP_A is not MD5.\n");
		return NULL;
	}
	PRINT("[server] Got CHAP_A=5\n");
	/*
	 * Send back CHAP_A set to MD5.
	 */
	*aic_len = sprintf(aic_str, "CHAP_A=5");
	*aic_len += 1;
	chap->digest_type = CHAP_DIGEST_MD5;
	PRINT("[server] Sending CHAP_A=%d\n", chap->digest_type);
	/*
	 * Set Identifier.
	 */
	chap->id = ISCSI_TPG_C(conn)->tpg_chap_id++;
	*aic_len += sprintf(aic_str + *aic_len, "CHAP_I=%d", chap->id);
	*aic_len += 1;
	PRINT("[server] Sending CHAP_I=%d\n", chap->id);
	/*
	 * Generate Challenge.
	 */
	chap_gen_challenge(conn, 1, aic_str, aic_len);

	return chap;
}

static void chap_close(struct iscsi_conn *conn)
{
	kfree(conn->auth_protocol);
	conn->auth_protocol = NULL;
}

static int chap_server_compute_md5(
	struct iscsi_conn *conn,
	struct iscsi_node_auth *auth,
	char *nr_in_ptr,
	char *nr_out_ptr,
	unsigned int *nr_out_len)
{
	char *endptr;
	unsigned char id, digest[MD5_SIGNATURE_SIZE];
	unsigned char type, response[MD5_SIGNATURE_SIZE * 2 + 2];
	unsigned char identifier[10], *challenge = NULL;
	unsigned char *challenge_binhex = NULL;
	unsigned char client_digest[MD5_SIGNATURE_SIZE];
	unsigned char server_digest[MD5_SIGNATURE_SIZE];
	unsigned char chap_n[MAX_CHAP_N_SIZE], chap_r[MAX_RESPONSE_LENGTH];
	struct iscsi_chap *chap = (struct iscsi_chap *) conn->auth_protocol;
	struct crypto_hash *tfm;
	struct hash_desc desc;
	struct scatterlist sg;
	int auth_ret = -1, ret, challenge_len;

	memset(identifier, 0, 10);
	memset(chap_n, 0, MAX_CHAP_N_SIZE);
	memset(chap_r, 0, MAX_RESPONSE_LENGTH);
	memset(digest, 0, MD5_SIGNATURE_SIZE);
	memset(response, 0, MD5_SIGNATURE_SIZE * 2 + 2);
	memset(client_digest, 0, MD5_SIGNATURE_SIZE);
	memset(server_digest, 0, MD5_SIGNATURE_SIZE);

	challenge = kzalloc(CHAP_CHALLENGE_STR_LEN, GFP_KERNEL);
	if (!challenge) {
		printk(KERN_ERR "Unable to allocate challenge buffer\n");
		goto out;
	}

	challenge_binhex = kzalloc(CHAP_CHALLENGE_STR_LEN, GFP_KERNEL);
	if (!challenge_binhex) {
		printk(KERN_ERR "Unable to allocate challenge_binhex buffer\n");
		goto out;
	}
	/*
	 * Extract CHAP_N.
	 */
	if (extract_param(nr_in_ptr, "CHAP_N", MAX_CHAP_N_SIZE, chap_n,
				&type) < 0) {
		printk(KERN_ERR "Could not find CHAP_N.\n");
		goto out;
	}
	if (type == HEX) {
		printk(KERN_ERR "Could not find CHAP_N.\n");
		goto out;
	}

	if (memcmp(chap_n, auth->userid, strlen(auth->userid)) != 0) {
		printk(KERN_ERR "CHAP_N values do not match!\n");
		goto out;
	}
	PRINT("[server] Got CHAP_N=%s\n", chap_n);
	/*
	 * Extract CHAP_R.
	 */
	if (extract_param(nr_in_ptr, "CHAP_R", MAX_RESPONSE_LENGTH, chap_r,
				&type) < 0) {
		printk(KERN_ERR "Could not find CHAP_R.\n");
		goto out;
	}
	if (type != HEX) {
		printk(KERN_ERR "Could not find CHAP_R.\n");
		goto out;
	}

	PRINT("[server] Got CHAP_R=%s\n", chap_r);
	chap_string_to_hex(client_digest, chap_r, strlen(chap_r));

	tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		printk(KERN_ERR "Unable to allocate struct crypto_hash\n");
		goto out;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	ret = crypto_hash_init(&desc);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_init() failed\n");
		crypto_free_hash(tfm);
		goto out;
	}

	sg_init_one(&sg, (void *)&chap->id, 1);
	ret = crypto_hash_update(&desc, &sg, 1);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for id\n");
		crypto_free_hash(tfm);
		goto out;
	}

	sg_init_one(&sg, (void *)&auth->password, strlen(auth->password));
	ret = crypto_hash_update(&desc, &sg, strlen(auth->password));
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for password\n");
		crypto_free_hash(tfm);
		goto out;
	}

	sg_init_one(&sg, (void *)chap->challenge, CHAP_CHALLENGE_LENGTH);
	ret = crypto_hash_update(&desc, &sg, CHAP_CHALLENGE_LENGTH);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for challenge\n");
		crypto_free_hash(tfm);
		goto out;
	}

	ret = crypto_hash_final(&desc, server_digest);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_final() failed for server digest\n");
		crypto_free_hash(tfm);
		goto out;
	}
	crypto_free_hash(tfm);

	chap_binaryhex_to_asciihex(response, server_digest, MD5_SIGNATURE_SIZE);
	PRINT("[server] MD5 Server Digest: %s\n", response);

	if (memcmp(server_digest, client_digest, MD5_SIGNATURE_SIZE) != 0) {
		PRINT("[server] MD5 Digests do not match!\n\n");
		goto out;
	} else
		PRINT("[server] MD5 Digests match, CHAP connetication"
				" successful.\n\n");
	/*
	 * One way authentication has succeeded, return now if mutual
	 * authentication is not enabled.
	 */
	if (!auth->authenticate_target) {
		kfree(challenge);
		kfree(challenge_binhex);
		return 0;
	}
	/*
	 * Get CHAP_I.
	 */
	if (extract_param(nr_in_ptr, "CHAP_I", 10, identifier, &type) < 0) {
		printk(KERN_ERR "Could not find CHAP_I.\n");
		goto out;
	}

	if (type == HEX)
		id = (unsigned char)simple_strtoul((char *)&identifier[2],
					&endptr, 0);
	else
		id = (unsigned char)simple_strtoul(identifier, &endptr, 0);
	/*
	 * RFC 1994 says Identifier is no more than octet (8 bits).
	 */
	PRINT("[server] Got CHAP_I=%d\n", id);
	/*
	 * Get CHAP_C.
	 */
	if (extract_param(nr_in_ptr, "CHAP_C", CHAP_CHALLENGE_STR_LEN,
			challenge, &type) < 0) {
		printk(KERN_ERR "Could not find CHAP_C.\n");
		goto out;
	}

	if (type != HEX) {
		printk(KERN_ERR "Could not find CHAP_C.\n");
		goto out;
	}
	PRINT("[server] Got CHAP_C=%s\n", challenge);
	challenge_len = chap_string_to_hex(challenge_binhex, challenge,
				strlen(challenge));
	if (!challenge_len) {
		printk(KERN_ERR "Unable to convert incoming challenge\n");
		goto out;
	}
	/*
	 * Generate CHAP_N and CHAP_R for mutual authentication.
	 */
	tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		printk(KERN_ERR "Unable to allocate struct crypto_hash\n");
		goto out;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	ret = crypto_hash_init(&desc);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_init() failed\n");
		crypto_free_hash(tfm);
		goto out;
	}

	sg_init_one(&sg, (void *)&id, 1);
	ret = crypto_hash_update(&desc, &sg, 1);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for id\n");
		crypto_free_hash(tfm);
		goto out;
	}

	sg_init_one(&sg, (void *)auth->password_mutual,
				strlen(auth->password_mutual));
	ret = crypto_hash_update(&desc, &sg, strlen(auth->password_mutual));
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for"
				" password_mutual\n");
		crypto_free_hash(tfm);
		goto out;
	}
	/*
	 * Convert received challenge to binary hex.
	 */
	sg_init_one(&sg, (void *)challenge_binhex, challenge_len);
	ret = crypto_hash_update(&desc, &sg, challenge_len);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_update() failed for ma challenge\n");
		crypto_free_hash(tfm);
		goto out;
	}

	ret = crypto_hash_final(&desc, digest);
	if (ret < 0) {
		printk(KERN_ERR "crypto_hash_final() failed for ma digest\n");
		crypto_free_hash(tfm);
		goto out;
	}
	crypto_free_hash(tfm);
	/*
	 * Generate CHAP_N and CHAP_R.
	 */
	*nr_out_len = sprintf(nr_out_ptr, "CHAP_N=%s", auth->userid_mutual);
	*nr_out_len += 1;
	PRINT("[server] Sending CHAP_N=%s\n", auth->userid_mutual);
	/*
	 * Convert response from binary hex to ascii hext.
	 */
	chap_binaryhex_to_asciihex(response, digest, MD5_SIGNATURE_SIZE);
	*nr_out_len += sprintf(nr_out_ptr + *nr_out_len, "CHAP_R=0x%s",
			response);
	*nr_out_len += 1;
	PRINT("[server] Sending CHAP_R=0x%s\n", response);
	auth_ret = 0;
out:
	kfree(challenge);
	kfree(challenge_binhex);
	return auth_ret;
}

static int chap_got_response(
	struct iscsi_conn *conn,
	struct iscsi_node_auth *auth,
	char *nr_in_ptr,
	char *nr_out_ptr,
	unsigned int *nr_out_len)
{
	struct iscsi_chap *chap = (struct iscsi_chap *) conn->auth_protocol;

	switch (chap->digest_type) {
	case CHAP_DIGEST_MD5:
		if (chap_server_compute_md5(conn, auth, nr_in_ptr,
				nr_out_ptr, nr_out_len) < 0)
			return -1;
		return 0;
	default:
		printk(KERN_ERR "Unknown CHAP digest type %d!\n",
				chap->digest_type);
		return -1;
	}
}

u32 chap_main_loop(
	struct iscsi_conn *conn,
	struct iscsi_node_auth *auth,
	char *in_text,
	char *out_text,
	int *in_len,
	int *out_len)
{
	struct iscsi_chap *chap = (struct iscsi_chap *) conn->auth_protocol;

	if (!chap) {
		chap = chap_server_open(conn, auth, in_text, out_text, out_len);
		if (!chap)
			return 2;
		chap->chap_state = CHAP_STAGE_SERVER_AIC;
		return 0;
	} else if (chap->chap_state == CHAP_STAGE_SERVER_AIC) {
		convert_null_to_semi(in_text, *in_len);
		if (chap_got_response(conn, auth, in_text, out_text,
				out_len) < 0) {
			chap_close(conn);
			return 2;
		}
		if (auth->authenticate_target)
			chap->chap_state = CHAP_STAGE_SERVER_NR;
		else
			*out_len = 0;
		chap_close(conn);
		return 1;
	}

	return 2;
}