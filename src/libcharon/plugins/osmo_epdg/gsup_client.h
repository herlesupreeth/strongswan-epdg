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

/**
 * @defgroup osmo_epdg_gsup_client osmo_epdg_gsup_client
 * @{ @ingroup eap_simaka_sql
 */

#ifndef OSMO_EPDG_GSUP_CLIENT_H_
#define OSMO_EPDG_GSUP_CLIENT_H_

#include <utils/chunk.h>

#include <osmocom/gsm/gsup.h>

struct osmo_epdg_gsup_response_t {
	struct osmo_gsup_message gsup;
	/* keep pdu around because gsup takes ownership of data out of pdu */
	struct msgb *pdu;
};
typedef struct osmo_epdg_gsup_response_t osmo_epdg_gsup_response_t;

static inline void osmo_epdg_gsup_resp_free(osmo_epdg_gsup_response_t *resp)
{
	if (!resp)
	{
		return;
	}

	if (resp->pdu)
	{
		free(resp->pdu);
	}

	free(resp);
}

typedef struct osmo_epdg_gsup_client_t osmo_epdg_gsup_client_t;

/**
 * foo
 */
struct osmo_epdg_gsup_client_t {
	/**
	 * Send Authentication Info Request
	 *
	 * @param imsi IMSI encoded as human-readable string (IMSI MAX = 15)
	 * @param auts (optional)
	 * @param auts_rand (optional)
	 * @return		NULL or the osmo_epdg_gsup_response_t
	 */
	osmo_epdg_gsup_response_t *(*send_auth_request)(osmo_epdg_gsup_client_t *this,
			const char *imsi, uint8_t cn_domain, chunk_t *auts, chunk_t *auts_rand,
			const char *apn, uint8_t pdp_type);

	/**
	 * Update Location Request
	 *
	 * @return		NULL or the osmo_gsup_message
	 */
	osmo_epdg_gsup_response_t *(*update_location)(osmo_epdg_gsup_client_t *this,
			const char *imsi,
			uint8_t cn_domain);

	/**
	 * Tunnel Request
	 *
	 * @return		NULL or the osmo_gsup_message
	 */
	osmo_epdg_gsup_response_t *(*tunnel_request)(osmo_epdg_gsup_client_t *this,
			const char *imsi);

	/**
	 * Destroy a osmo_epdg_gsup_client_t.
	 */
	void (*destroy)(osmo_epdg_gsup_client_t *this);
};

/**
 * Create a osmo_epdg_gsup_client instance.
 *
 * @param address   the address of the gsup server */
osmo_epdg_gsup_client_t *osmo_epdg_gsup_client_create(char *addr);

#endif /** OSMO_EPDG_GSUP_CLIENT_H_ @}*/

