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
 * @defgroup osmo_epdg_listener osmo_epdg_listener
 * @{ @ingroup osmo_epdg
 */

#ifndef OSMO_EPDG_LISTENER_H_
#define OSMO_EPDG_LISTENER_H_

#include <bus/listeners/listener.h>
#include "osmo_epdg_db.h"
#include "gsup_client.h"

typedef struct osmo_epdg_listener_t osmo_epdg_listener_t;

/**
 * SIM listener implementation using a set of AKA functions.
 */
struct osmo_epdg_listener_t {

	/**
	 * Implements listener_t interface.
	 */
	listener_t listener;

	/**
	 * Destroy a osmo_epdg_listener_t.
	 */
	void (*destroy)(osmo_epdg_listener_t *this);
};

/**
 * Create a osmo_epdg_listener instance.
 */
osmo_epdg_listener_t *osmo_epdg_listener_create(osmo_epdg_db_t *db, osmo_epdg_gsup_client_t *gsup);

#endif /** OSMO_EPDG_LISTENER_H_ @}*/
