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

// TODO: check license

#include <daemon.h>
#include <plugins/plugin.h>
#include <errno.h>
#include <unistd.h>

#include <osmocom/gsm/apn.h>

#include "osmo_epdg_listener.h"
#include "osmo_epdg_db.h"
#include "osmo_epdg_utils.h"

typedef struct private_osmo_epdg_listener_t private_osmo_epdg_listener_t;

/**
 * Private data of an osmo_epdg_listener_t object.
 */
struct private_osmo_epdg_listener_t {
	/**
	 * Public osmo_epdg_listener_t interface.
	 */
	osmo_epdg_listener_t public;
	osmo_epdg_gsup_client_t *gsup;
	osmo_epdg_db_t *db;
};

METHOD(listener_t, eap_authorize, bool,
	private_osmo_epdg_listener_t *this, ike_sa_t *ike_sa,
	identification_t *id, bool final, bool *success)
{
	char imsi[16] = {0};
	osmo_epdg_ue_t *ue = NULL;
	osmo_epdg_gsup_response_t *resp = NULL;

	if (!id)
	{
		DBG1(DBG_NET, "epdg: authorize: no id given. Failing.");
		goto err;
	}
	if (epdg_get_imsi(id, imsi, sizeof(imsi) - 1))
	{
		DBG1(DBG_NET, "epdg: authorize: Can't find IMSI in EAP identity.");
		goto err;
	}

	ue = this->db->create_subscriber(this->db, ike_sa);
	if (!ue)
	{
		DBG1(DBG_NET, "epdg: authorize: Could not create subscriber via db! Rejecting.");
		goto err;
	}

	resp = this->gsup->update_location(this->gsup, imsi, OSMO_GSUP_CN_DOMAIN_PS);
	if (!resp)
	{
		DBG1(DBG_NET, "epdg: GSUP: couldn't send Update Location.");
		this->db->remove_subscriber(this->db, imsi);
		goto err;
	}

	if (resp->gsup.message_type != OSMO_GSUP_MSGT_UPDATE_LOCATION_RESULT)
	{
		DBG1(DBG_NET, "epdg_listener: Update Location Error! Cause: %02x", resp->gsup.cause);
		goto err;
	}
	ue->set_state(ue, UE_LOCATION_UPDATED);
	ue->put(ue);
	osmo_epdg_gsup_resp_free(resp);
	return TRUE;

err:
	*success = FALSE;
	if (ue)
	{
		ue->set_state(ue, UE_FAIL);
		ue->put(ue);
	}

	osmo_epdg_gsup_resp_free(resp);
	/* keep still subscribed */
	return TRUE;
}

METHOD(listener_t, authorize, bool,
	private_osmo_epdg_listener_t *this, ike_sa_t *ike_sa,
	bool final, bool *success)
{
	identification_t* imsi_id;
	char imsi[16] = {0};
	osmo_epdg_ue_t *ue = NULL;
	host_t *address = NULL;
	struct osmo_gsup_pdp_info *pdp_info;
	osmo_epdg_gsup_response_t *resp = NULL;


	DBG1(DBG_NET, "Authorized: uniq 0x%08x, name %s final: %d, eap: %d!",
		ike_sa->get_unique_id(ike_sa),
                ike_sa->get_name(ike_sa),
		final,
		ike_sa->has_condition(ike_sa, COND_EAP_AUTHENTICATED));

	if (!final)
	{
		return TRUE;
	}

	imsi_id = ike_sa->get_other_id(ike_sa);
	if (!imsi_id)
	{
		DBG1(DBG_NET, "epdg: authorize: Can't get EAP identity.");
		goto err;
	}

	if (epdg_get_imsi(imsi_id, imsi, sizeof(imsi) - 1))
	{
		DBG1(DBG_NET, "epdg: authorize: Can't find IMSI in EAP identity.");
		goto err;
	}

	ue = this->db->get_subscriber(this->db, imsi);
	if (!ue)
	{
		DBG1(DBG_NET, "epdg: authorize: Can't find match UE for imsi %s via EAP identity.", imsi);
		goto err;
	}

	ue->set_state(ue, UE_WAIT_TUNNEL);
	
	/* Always request IPv4 PDP type - GSUP communication is always IPv4 */
	/* The manager will decide the actual PDP type in the response */
	uint8_t pdp_type = PDP_TYPE_N_IETF_IPv4;
	DBG1(DBG_NET, "epdg_listener: Requesting IPv4 PDP type for tunnel request");
	
	resp = this->gsup->tunnel_request(this->gsup, imsi, pdp_type);
	if (!resp)
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Request: GSUP: couldn't send.");
		goto err;
	}

	if (resp->gsup.message_type == OSMO_GSUP_MSGT_EPDG_TUNNEL_ERROR)
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Error! Cause: %02x", resp->gsup.cause);
		goto err;
	}
	else if (resp->gsup.message_type != OSMO_GSUP_MSGT_EPDG_TUNNEL_RESULT)
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: unexpected message type: %02x", resp->gsup.message_type);
		goto err;
	}

	/* validate Tunnel Response */
	if ((resp->gsup.num_pdp_infos != 1) ||
	    (!resp->gsup.pdp_infos[0].have_info) ||
	    (resp->gsup.pdp_infos[0].pdp_type_org != PDP_TYPE_ORG_IETF))
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: received incomplete message/wrong content", imsi);
		goto err;
	}

	pdp_info = &resp->gsup.pdp_infos[0];
	
	/* Validate PDP type and address family */
	if (pdp_info->pdp_type_nr == PDP_TYPE_N_IETF_IPv4)
	{
		/* IPv4 tunnel */
		if (pdp_info->pdp_address[0].u.sa.sa_family != AF_INET)
		{
			DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: IPv4 PDP type but wrong address family", imsi);
			goto err;
		}
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: IPv4 tunnel established", imsi);
	}
	else if (pdp_info->pdp_type_nr == PDP_TYPE_N_IETF_IPv6)
	{
		/* IPv6 tunnel */
		if (pdp_info->pdp_address[0].u.sa.sa_family != AF_INET6)
		{
			DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: IPv6 PDP type but wrong address family", imsi);
			goto err;
		}
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: IPv6 tunnel established", imsi);
	}
	else
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: unsupported PDP type: 0x%02x", imsi, pdp_info->pdp_type_nr);
		goto err;
	}

	address = host_create_from_sockaddr(&pdp_info->pdp_address[0].u.sa);
	if (!address)
	{
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: couldn't convert PDP info to host_address", imsi);
		goto err;
	}

	ue->set_address(ue, address);
	/* Set attributes based on APCO contents */
	if (resp->gsup.pco && resp->gsup.pco_len > 0)
	{
		/* Print PCO octets for debugging */
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: decoding PCO (len=%zu)", imsi, resp->gsup.pco_len);
		char pco_hex[resp->gsup.pco_len * 2 + 1];
		for (size_t i = 0; i < resp->gsup.pco_len; i++)
		{
			snprintf(&pco_hex[i * 2], 3, "%02x", resp->gsup.pco[i]);
		}
		DBG1(DBG_NET, "epdg_listener: Tunnel Response: IMSI %s: PCO: %s", imsi, pco_hex);

		/* APCO (Additional Protocol Configuration Options) definitions */
		#define APCO_CONFIG_PROTOCOL_PPP    0x00

		/* Common APCO protocol IDs (same as PCO) */
		#define APCO_PID_DNS_SERVER_IPV4    0x000D
		#define APCO_PID_P_CSCF_IPV4        0x000C

		/* Decode APCO content as per 3GPP TS 24.008 */
		uint8_t config_protocol = resp->gsup.pco[0] & 0x07; /* Bits 0-2 */
		bool ext = resp->gsup.pco[0] & 0x80; /* Bit 8 */

		DBG1(DBG_NET, "APCO: Configuration protocol: %s (0x%02x), ext: %d", 
			config_protocol == APCO_CONFIG_PROTOCOL_PPP ? "PPP" : "Unknown",
			config_protocol, ext);

		/* APCO parsing starts at offset 1 */
		size_t offset = 1;
		/* +3 for container ID (2) and length field (1) */
		while (offset + 3 < resp->gsup.pco_len)
		{
			uint16_t container_id = (resp->gsup.pco[offset] << 8) | resp->gsup.pco[offset + 1];
			offset += 2;
			
			if (offset >= resp->gsup.pco_len)
			{
				DBG1(DBG_NET, "APCO: Truncated at container ID");
				break;
			}
			
			uint8_t length = resp->gsup.pco[offset++];
			
			if (offset + length > resp->gsup.pco_len)
			{
				DBG1(DBG_NET, "APCO: Truncated container data");
				break;
			}
			
			/* Process based on container ID */
			switch (container_id)
			{
				case APCO_PID_DNS_SERVER_IPV4:
				{
					if (length == 4)
					{
						osmo_epdg_attribute_t *entry;
						char dns_addr[16]; /* IPv4 address in string format */
						snprintf(dns_addr, sizeof(dns_addr), "%d.%d.%d.%d",
							resp->gsup.pco[offset], resp->gsup.pco[offset + 1],
							resp->gsup.pco[offset + 2], resp->gsup.pco[offset + 3]);
						DBG1(DBG_NET, "APCO: DNS Server IPv4 (0x%04x): %s",
							container_id, dns_addr);
						host_t *host = host_create_from_string_and_family(dns_addr, AF_INET, 0);
						INIT(entry,
							.type = INTERNAL_IP4_DNS,
							.value = chunk_clone(host->get_address(host)),
							.valid = TRUE,
						);
						ue->insert_attribute(ue, entry);
						host->destroy(host);
					}
					else
					{
						DBG1(DBG_NET, "APCO: DNS Server IPv4 (0x%04x), invalid length: %d", 
							container_id, length);
					}
				}
				break;
				case APCO_PID_P_CSCF_IPV4:
				{
					if (length == 4)
					{
						osmo_epdg_attribute_t *entry;
						char p_cscf_addr[16]; /* IPv4 address in string format */
						snprintf(p_cscf_addr, sizeof(p_cscf_addr), "%d.%d.%d.%d",
							resp->gsup.pco[offset], resp->gsup.pco[offset + 1],
							resp->gsup.pco[offset + 2], resp->gsup.pco[offset + 3]);
						DBG1(DBG_NET, "APCO: P-CSCF IPv4 (0x%04x): %s",
							container_id, p_cscf_addr);
						host_t *host = host_create_from_string_and_family(p_cscf_addr, AF_INET, 0);
						INIT(entry,
							.type = P_CSCF_IP4_ADDRESS,
							.value = chunk_clone(host->get_address(host)),
							.valid = TRUE,
						);
						ue->insert_attribute(ue, entry);
						host->destroy(host);
					}
					else
					{
						DBG1(DBG_NET, "APCO: P-CSCF IPv4 (0x%04x), invalid length: %d",
							container_id, length);
					}
				}
				break;
				default:
					DBG1(DBG_NET, "APCO: Unknown container ID (0x%04x), length: %d", container_id, length);
					break;
			}
			offset += length;
		}
	}
	ue->set_state(ue, UE_TUNNEL_READY);
	ue->put(ue);

	address->destroy(address);
	osmo_epdg_gsup_resp_free(resp);
	return TRUE;

err:
	osmo_epdg_gsup_resp_free(resp);

	if (ue)
	{
		ue->set_state(ue, UE_FAIL);
		ue->put(ue);
	}
	DESTROY_IF(address);

	*success = FALSE;
	/* keep still subscribed */
	return TRUE;
}

METHOD(listener_t, ike_updown, bool,
       private_osmo_epdg_listener_t *this, ike_sa_t *ike_sa, bool up)
{
	char imsi[16] = {0};
	if (epdg_get_imsi_ike(ike_sa, imsi, sizeof(imsi)))
	{
		DBG1(DBG_NET, "epdg_listener: updown: imsi UNKNOWN: IKE_SA went %s", up ? "up" : "down");
		return TRUE;
	}
	DBG1(DBG_NET, "epdg_listener: updown: imsi %s: IKE_SA went %s", imsi, up ? "up" : "down");

	return TRUE;
}

METHOD(osmo_epdg_listener_t, destroy, void,
	private_osmo_epdg_listener_t *this)
{
	free(this);
}

/**
 * See header
 */
osmo_epdg_listener_t *osmo_epdg_listener_create(osmo_epdg_db_t *db, osmo_epdg_gsup_client_t *gsup)
{
	private_osmo_epdg_listener_t *this;

	INIT(this,
		.public = {
			.listener = {
				.authorize = _authorize,
				.eap_authorize = _eap_authorize,
				.ike_updown = _ike_updown,
			},
			.destroy = _destroy,
		},
		.gsup = gsup,
		.db = db,
	);

	return &this->public;
}
