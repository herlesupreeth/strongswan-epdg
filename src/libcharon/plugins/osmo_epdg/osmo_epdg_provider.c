/*
 * Copyright (C) 2023 sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 * Author: Alexander Couzens <acouzens@sysmocom.de>
 *
 * SPDX-License-Identifier: GPL-2.0+
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
 *
 */

#include "osmo_epdg_provider.h"
#include "osmo_epdg_utils.h"
#include "gsup_client.h"
#include "osmo_epdg_db.h"
#include "osmo_epdg_ue.h"

#include <daemon.h>
#include <collections/linked_list.h>
#include <credentials/keys/shared_key.h>
#include <osmocom/gsm/apn.h>
#include <osmocom/gsm/protocol/gsm_04_08_gprs.h>

#define AKA_SQN_LEN		 6
#define AKA_K_LEN		16
#define AKA_OPC_LEN		16
#define AKA_MAC_LEN		 8
#define AKA_AK_LEN		 6
#define AKA_AMF_LEN		 2
#define AKA_RES_LEN		 8

typedef struct private_osmo_epdg_provider_t private_osmo_epdg_provider_t;

/**
 * Private data of an osmo_epdg_provider_t object.
 */
struct private_osmo_epdg_provider_t {

	/**
	 * Public osmo_epdg_provider_t interface.
	 */
	osmo_epdg_provider_t public;

	osmo_epdg_gsup_client_t *gsup;

	osmo_epdg_db_t *db;
};

/**
 * Get a shared key K from the credential database
 */
bool osmo_epdg_get_k(identification_t *id, char k[AKA_K_LEN])
{
	shared_key_t *shared;
	chunk_t key;

	shared = lib->credmgr->get_shared(lib->credmgr, SHARED_EAP, id, NULL);
	if (shared == NULL)
	{
		return FALSE;
	}
	key = shared->get_key(shared);
	memset(k, '\0', AKA_K_LEN);
	memcpy(k, key.ptr, min(key.len, AKA_K_LEN));
	shared->destroy(shared);
	return TRUE;
}

METHOD(simaka_provider_t, get_quintuplet, bool,
	private_osmo_epdg_provider_t *this, identification_t *id,
	char rand[AKA_RAND_LEN], char xres[AKA_RES_MAX], int *xres_len,
	char ck[AKA_CK_LEN], char ik[AKA_IK_LEN], char autn[AKA_AUTN_LEN])
{
	char apn[APN_MAXLEN];
	char imsi[17] = {0};
	ike_sa_t *ike_sa;

	if (epdg_get_imsi(id, imsi, sizeof(imsi) - 1))
	{
		DBG1(DBG_NET, "epdg: get_quintuplet: Can't find IMSI in EAP identity.");
		return FALSE;
	}

	ike_sa = charon->bus->get_sa(charon->bus);
	if (!ike_sa)
	{
		DBG1(DBG_NET, "epdg: get_quintuplet: Can't get ike_sa.");
		return FALSE;
	}

	if (epdg_get_apn(ike_sa, apn, APN_MAXLEN))
	{
		DBG1(DBG_NET, "epdg: get_quintuplet: Can't get APN.");
		return FALSE;
	}

	/* Always use IPv4 PDP type for auth requests - GSUP communication is always IPv4 */
	uint8_t pdp_type = PDP_TYPE_N_IETF_IPv4;

	osmo_epdg_gsup_response_t *resp = this->gsup->send_auth_request(
			this->gsup, imsi, OSMO_GSUP_CN_DOMAIN_PS, NULL, NULL, apn, pdp_type);
	if (!resp)
	{
		DBG1(DBG_NET, "epdg_provider: Failed to send auth request.");
		return FALSE;
	}

	if (resp->gsup.message_type != OSMO_GSUP_MSGT_SEND_AUTH_INFO_RESULT)
	{
		DBG1(DBG_NET, "epdg_provider: SendAuthInfo Error! Cause: %02x", resp->gsup.cause);
		goto err;
	}

	struct osmo_auth_vector *auth = &resp->gsup.auth_vectors[0];
	if (resp->gsup.num_auth_vectors == 0)
	{
		/* TODO: invalid auth data received */
		DBG1(DBG_NET, "epdg_provider: SendAuthInfo Invalid Auth Received!");
		goto err;
	}

	memcpy(rand, auth->rand, AKA_RAND_LEN);
	memcpy(ck, auth->ck, AKA_CK_LEN);
	memcpy(ik, auth->ik, AKA_IK_LEN);
	memcpy(autn, auth->autn, AKA_AUTN_LEN);
	memcpy(xres, auth->res, auth->res_len);
	*xres_len = auth->res_len;
	
	osmo_epdg_gsup_resp_free(resp);
	return TRUE;
err:
	osmo_epdg_gsup_resp_free(resp);
	return FALSE;
}

METHOD(simaka_provider_t, resync, bool,
	private_osmo_epdg_provider_t *this, identification_t *id,
	char rand[AKA_RAND_LEN], char auts[AKA_AUTS_LEN])
{
	/* TODO: invalid auth data received */
	/* prepare and fill up the struct */
	/* send pdu blocking */
	return FALSE;
}

#ifndef container_of
#define container_of(ptr, type, member) ({          \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type, member) );})
#endif


METHOD(attribute_provider_t, acquire_address, host_t*,
	private_osmo_epdg_provider_t *this, linked_list_t *pools, ike_sa_t *ike_sa,
	host_t *requested)
{
	/* yes this hurts. We can either move the attribute provider out of this class or do some pointer arithmetic to get the right this object */
	this = container_of((void *) this, private_osmo_epdg_provider_t, public.attribute);
	
	/* Add null check for requested host */
	if (!requested)
	{
		DBG1(DBG_NET, "epdg_provider: acquire_address: requested host is NULL");
		return NULL;
	}
	
	/* For now, only support IPv4 to avoid crashes */
	if (requested->get_family(requested) != AF_INET)
	{
		DBG1(DBG_NET, "epdg_provider: acquire_address: only IPv4 supported for now, got family: %d", requested->get_family(requested));
		return NULL;
	}

	osmo_epdg_ue_t *ue = this->db->get_subscriber_ike(this->db, ike_sa);
	host_t *address = NULL;
	/* TODO: check if we want to limit the pool here as well to "epdg" similar what dhcp does */

	if (!ue)
	{
		DBG1(DBG_NET, "epdg_provider: acquire_address: Failed to get the UE by IKE");
		return NULL;
	}

	/* TODO: check for IPv4/IPv6 */
	address = ue->get_address(ue);
	ue->put(ue);

	return address;
}

METHOD(attribute_provider_t, release_address, bool,
	private_osmo_epdg_provider_t *this, linked_list_t *pools, host_t *address,
	ike_sa_t *ike_sa)
{
	this = container_of((void *) this, private_osmo_epdg_provider_t, public.attribute);
	osmo_epdg_ue_t *ue = this->db->get_subscriber_ike(this->db, ike_sa);
	host_t *ue_address = ue->get_address(ue);
	bool found = FALSE;

	if (!ue)
	{
		DBG1(DBG_NET, "epdg_provider: release_address: Failed to get the UE by IKE");
		return FALSE;
	}

	found = address->equals(address, ue_address);
	ue_address->destroy(ue_address);
	ue->put(ue);

	return found;
}

/* see attr_provider for similar usage */
CALLBACK(attribute_enum_filter, bool,
        void *data, enumerator_t *orig, va_list args)
{
	osmo_epdg_attribute_t *entry;
	configuration_attribute_type_t *type;
	chunk_t *value;

	VA_ARGS_VGET(args, type, value);
	while (orig->enumerate(orig, &entry))
	{
		if (entry->valid)
		{
			*type = entry->type;
			*value = entry->value;
			return TRUE;
		}
	}

	return FALSE;
}

METHOD(attribute_provider_t, create_attribute_enumerator, enumerator_t*,
	private_osmo_epdg_provider_t *this, linked_list_t *pools, ike_sa_t *ike_sa,
	linked_list_t *vips)
{
	this = container_of((void *) this, private_osmo_epdg_provider_t, public.attribute);
	enumerator_t *enumerator = NULL;
	linked_list_t *attributes = NULL;
	osmo_epdg_ue_t *ue = this->db->get_subscriber_ike(this->db, ike_sa);

	/* create an iterator based on the llist */
	if (!ue)
	{
		return enumerator_create_empty();
	}

	/* this ref is giving into the enumerator */
	ue->get(ue);
	attributes = ue->get_attributes(ue);
	enumerator = enumerator_create_cleaner(
			enumerator_create_filter(
				attributes->create_enumerator(attributes),
				attribute_enum_filter, NULL, NULL),
			(void *)ue->put, ue);

	/* this ref was taken by get_subscriber */
	ue->put(ue);
	return enumerator;
}

METHOD(osmo_epdg_provider_t, destroy, void,
	private_osmo_epdg_provider_t *this)
{
	free(this);
}

/**
 * See header
 */
osmo_epdg_provider_t *osmo_epdg_provider_create(osmo_epdg_db_t *db, osmo_epdg_gsup_client_t *gsup)
{
	private_osmo_epdg_provider_t *this;

	INIT(this,
		.public = {
			.attribute = {
				.acquire_address = _acquire_address,
				.release_address = _release_address,
				.create_attribute_enumerator = _create_attribute_enumerator,
			},
			.simaka = {
				.get_triplet = (void*)return_false,
				.get_quintuplet = _get_quintuplet,
				.resync = _resync,
				.is_pseudonym = (void*)return_null,
				.gen_pseudonym = (void*)return_null,
				.is_reauth = (void*)return_null,
				.gen_reauth = (void*)return_null,
			},
			.destroy = _destroy,
		},
		.db = db,
		.gsup = gsup,
	);

	return &this->public;
}
