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

#include <daemon.h>
#include <plugins/plugin.h>
#include <collections/hashtable.h>
#include <threading/rwlock.h>
#include <unistd.h>

#include <osmocom/gsm/apn.h>

#include "osmo_epdg_plugin.h"
#include "osmo_epdg_db.h"
#include "osmo_epdg_utils.h"

typedef struct private_osmo_epdg_db_t private_osmo_epdg_db_t;

/**
 * Private data of an osmo_epdg_db_t object.
 */
struct private_osmo_epdg_db_t {
	/**
	 * Public osmo_epdg_db_t interface.
	 */
	osmo_epdg_db_t public;

	/**
	 * subscriber hash by imsi (how to handle multiple?)
	 */
	hashtable_t *subscribers_imsi;

	/**
	 * rwlock to lock access for changes
	 */
	rwlock_t *lock;
};

METHOD(osmo_epdg_db_t, create_subscriber, osmo_epdg_ue_t *,
	private_osmo_epdg_db_t *this, ike_sa_t *ike_sa)
{
	osmo_epdg_ue_t *ue;
	char imsi[16] = {0};
	char apn[APN_MAXLEN];
	uint32_t unique = ike_sa->get_unique_id(ike_sa);

	if (epdg_get_imsi_ike(ike_sa, imsi, sizeof(imsi) - 1))
	{
		return NULL;
	}

	if (epdg_get_apn(ike_sa, apn, APN_MAXLEN))
	{
		DBG1(DBG_NET, "epdg: get_quintuplet: Can't get APN.");
		return FALSE;
	}

	this->lock->write_lock(this->lock);
	ue = this->subscribers_imsi->remove(this->subscribers_imsi, imsi);
	if (ue)
	{
		/* Handle duplicates/reconnections: 
		 * Clean up old UE state and create fresh one with new unique_id */
		uint32_t old_id = ue->get_id(ue);
		DBG1(DBG_NET, "epdg_db: removing old subscriber %s (old_id=%u, new_id=%u)", 
		     imsi, old_id, unique);
		ue->put(ue);
	}

	ue = osmo_epdg_ue_create(unique, imsi, apn);
	if (!ue)
	{
		DBG1(DBG_NET, "epdg_db: failed to create UE!");
		this->lock->unlock(this->lock);
		return NULL;
	}

	/* UE comes with refcount = 1 */
	this->subscribers_imsi->put(this->subscribers_imsi, ue->get_imsi(ue), ue);
	ue->get(ue);
	this->lock->unlock(this->lock);
	return ue;
}

METHOD(osmo_epdg_db_t, get_subscriber, osmo_epdg_ue_t *,
       private_osmo_epdg_db_t *this, char *imsi)
{
	osmo_epdg_ue_t *ue;
	this->lock->read_lock(this->lock);
	ue = this->subscribers_imsi->get(this->subscribers_imsi, imsi);
	if (ue)
	{
		ue->get(ue);
	}
	this->lock->unlock(this->lock);
	return ue;
}

METHOD(osmo_epdg_db_t, get_subscriber_ike, osmo_epdg_ue_t *,
       private_osmo_epdg_db_t *this, ike_sa_t *ike_sa)
{
	char imsi[16] = {0};

	if (epdg_get_imsi_ike(ike_sa, imsi, sizeof(imsi)))
	{
		return NULL;
	}

	return this->public.get_subscriber(&this->public, imsi);
}

METHOD(osmo_epdg_db_t, get_subscriber_id, osmo_epdg_ue_t *,
       private_osmo_epdg_db_t *this, uint32_t unique_id)
{
	/* This could be optimize, but keep it is for now */
	osmo_epdg_ue_t *ue = NULL;
	enumerator_t *enumerator;

	this->lock->read_lock(this->lock);
	enumerator = this->subscribers_imsi->create_enumerator(this->subscribers_imsi);
	while (enumerator->enumerate(enumerator, NULL, ue))
	{
		if (!ue)
		{
			continue;
		}
		if (ue->get_id(ue) == unique_id)
		{
			ue->get(ue);
			goto out;
		}
	}
	ue = NULL;
out:
	this->lock->unlock(this->lock);
	enumerator->destroy(enumerator);
	return ue;
}

METHOD(osmo_epdg_db_t, remove_subscriber, void,
	private_osmo_epdg_db_t *this, const char *imsi)
{
	osmo_epdg_ue_t *ue;

	this->lock->write_lock(this->lock);
	ue = this->subscribers_imsi->remove(this->subscribers_imsi, imsi);
	this->lock->unlock(this->lock);

	if (ue)
	{
		ue->put(ue);
	}
}

CALLBACK(destroy_ue, void,
	osmo_epdg_ue_t *ue, const void *key)
{
	ue->put(ue);
}

METHOD(osmo_epdg_db_t, destroy, void,
	private_osmo_epdg_db_t *this)
{
	this->subscribers_imsi->destroy_function(this->subscribers_imsi, destroy_ue);
	this->lock->destroy(this->lock);
	free(this);
}

/**
 * See header
 */
osmo_epdg_db_t *osmo_epdg_db_create()
{
	private_osmo_epdg_db_t *this;

	INIT(this,
		.public = {
			.create_subscriber = _create_subscriber,
			.get_subscriber = _get_subscriber,
			.get_subscriber_ike = _get_subscriber_ike,
			.get_subscriber_id = _get_subscriber_id,
			.remove_subscriber = _remove_subscriber,
			.destroy = _destroy,
		},
                .subscribers_imsi = hashtable_create(hashtable_hash_str, hashtable_equals_str, 128),
                .lock = rwlock_create(RWLOCK_TYPE_DEFAULT),
	);

	return &this->public;
}

