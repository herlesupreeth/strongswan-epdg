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

#include <errno.h>
#include <ctype.h>

#include <osmocom/core/msgb.h>
#include <sa/ike_sa.h>
#include <utils/utils.h>
#include <utils/debug.h>

#include "osmo_epdg_utils.h"

struct msgb *epdg_chunk_to_msgb(chunk_t *chunk)
{
	struct msgb *msg;
	if (chunk->len < sizeof(*msg))
	{
		return NULL;
	}

	msg = (struct msgb *) chunk->ptr;
	memset(msg, 0x00, sizeof(*msg));
	msg->data_len = chunk->len - sizeof(*msg);
	msg->len = 0;
	msg->data = msg->_data;
	msg->head = msg->_data;
	msg->tail = msg->_data;
	return msg;
}

int epdg_get_imsi_ike(ike_sa_t *ike_sa, char *imsi, size_t imsi_len)
{
	identification_t *imsi_id = ike_sa->get_other_id(ike_sa);
	if (!imsi_id)
	{
		return -1;
	}

	return epdg_get_imsi(imsi_id, imsi, imsi_len);
}

int epdg_get_imsi(identification_t *id, char *imsi, size_t imsi_len)
{
	chunk_t nai;
	
	if (!id)
	{
		return -EINVAL;
	}
	
	nai = id->get_encoding(id);
	
	/* Validate chunk pointer - may be NULL during IKE_SA destruction */
	if (!nai.ptr || nai.len == 0)
	{
		DBG1(DBG_NET, "epdg: Invalid NAI chunk (NULL pointer or zero length)");
		return -EINVAL;
	}
	
	/* TODO: maybe use regex? */
	/* 099942123456789@mnc042.mcc999.3gpp... */
	if (nai.len < 17)
	{
		DBG1(DBG_NET, "epdg: Invalid NAI length %zu (expected >= 17)", nai.len);
		return -EINVAL;
	}

	if (nai.ptr[0] != '0')
	{
		DBG1(DBG_NET, "epdg: Invalid NAI. Only support IMSI (starting with 0). %s.",
		 nai);
		return -EINVAL;
	}

	strncpy(imsi, nai.ptr + 1, min(15, imsi_len));
	return 0;
}

int epdg_validate_imsi(const char *imsi)
{
	if (!imsi)
		return 1;

	if (strlen(imsi) != 15)
		return 1;

	for (int i=0; i<strlen(imsi); i++)
	{
		if (!isdigit(imsi[i]))
		{
			return 1;
		}
	}

	return 0;
}

int epdg_validate_apn(const char *apn)
{
	/* don't support empty apn */
	if (!apn)
		return 1;

	if (!strlen(apn))
		return 1;

	return 0;
}

int epdg_get_apn(ike_sa_t *sa, char *apn, size_t apn_len)
{
	identification_t* apn_id;
	chunk_t apn_chunk;

	if (!sa)
	{
		return -EINVAL;
	}

	apn_id = sa->get_my_id(sa);
	if (!apn_id)
	{
		return -EINVAL;
	}

	apn_chunk = apn_id->get_encoding(apn_id);
	
	/* Validate chunk pointer - may be NULL during IKE_SA destruction */
	if (!apn_chunk.ptr || apn_chunk.len == 0)
	{
		DBG1(DBG_NET, "epdg: Invalid APN chunk (NULL pointer or zero length)");
		return -EINVAL;
	}
	
	if (apn_chunk.len >= apn_len)
	{
		return -ENOMEM;
	}

	memcpy(apn, apn_chunk.ptr, apn_chunk.len);
	apn[apn_chunk.len] = 0;
	return 0;
}
