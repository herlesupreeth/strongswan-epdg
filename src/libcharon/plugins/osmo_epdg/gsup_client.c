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

/* TODO: check license */

#include <collections/enumerator.h>
#include <collections/linked_list.h>
#include <collections/blocking_queue.h>
#include <processing/jobs/callback_job.h>
#include <threading/mutex.h>
#include <threading/thread.h>
#include <threading/condvar.h>
#include <threading/rwlock.h>

#include <osmocom/core/msgb.h>
#include <osmocom/gsm/apn.h>
#include <osmocom/gsm/gsup.h>
#include <osmocom/gsm/protocol/ipaccess.h>

#include <library.h>
#include <errno.h>

#include "ipa_client.h"
#include "gsup_client.h"
#include "osmo_epdg_utils.h"

/* A GSUP client for osmocom.
 *
 * A request will block until it handled by the gsup or timeout.
 * So all tx function will block with a timeout of 5 seconds.
 *
 * To allow multiple request in flight, request will flow through gsup_client:
 * - send_auth_request() -> generate a gsup_request_t object
 * - it will enqueue()d into the inqueue (a blocking queue).
 * - the sender job (a differnet thread) will get a req out of inqueue, transmit it and enqueue it into **pending**.
 * - the receveier job (also a different thread?) will receive a PDU and try to find a matching gsup_request_t.
 * - if a matching gsup_request_t can be found, the thread of who is blocked in send_auth_request() will be woken and can work with the response.
 *
 * - if a timeout happen, the gsup_request_t can be at 3 different position.
 * - a) still in the *inqueue*. the requester can remove it atomic
 * - b) in the *pending* list, the requester can remove it atomic
 * - c) neither in *inqueue* and *pending*, it is current in use by the sender/receiver. The requester will use the gsup_request_t->lock() to wait for the completion.
 *
 * The c) case is the most complex to ensure the request will be cleaned. If not synchronized, the requester look into the *pending* queue, can't find it there and the
 * gsup_request_t will never been cleaned.
 */

typedef struct gsup_request_t gsup_request_t;
struct gsup_request_t {
	/**
	 * Mutex used to synchronize access to the condvar
	 */
	mutex_t *mutex;

	/**
	 * Condvar used to wait for a response
	 */
	condvar_t *condvar;

	/**
	 * Lock the object to allow working with it without an garbage collector.
	 * When a request time out and at the same time the receiver or sender is using the object,
	 * the object isn't part of the in-queue nor of the pending-list. Use the lock to synchronize
	 * this small time.
	 * After this lock is taken by enqueue(), the rx/tx gsup won't add it anymore to pending list and release it.
	 */
	rwlock_t *lock;

	/**
	 * refcounter to free the object
	 */
	refcount_t refcount;

	struct msgb *msg;
	enum osmo_gsup_message_type msg_type;
	char *imsi;
	osmo_epdg_gsup_response_t *resp;
};

typedef struct private_osmo_epdg_gsup_client_t private_osmo_epdg_gsup_client_t;
struct private_osmo_epdg_gsup_client_t {
	/**
	 * Public osmo_epdg_gsup_client_t
	 */
	osmo_epdg_gsup_client_t public;

	osmo_epdg_ipa_client_t *ipa;

	/**
	 * List of all inqueue requests
	 * The list "owns" a references by req->get().
	 */
	blocking_queue_t *inqueue;

	/**
	 * List of all pending requests (gsup_request_t).
	 * The list "owns" a references by req->get().
	 */
	linked_list_t *pending;
	mutex_t *pending_mutex;

	char *uri;

	stream_t *stream;
};

/* TODO: move into own class? */
static gsup_request_t *gsup_request_create(enum osmo_gsup_message_type msg_type, const char *imsi, struct msgb *msg)
{
	gsup_request_t *req = calloc(1, sizeof(gsup_request_t));
	if (!req)
	{
		return NULL;
	}

	req->lock = rwlock_create(RWLOCK_TYPE_DEFAULT);
	req->mutex = mutex_create(MUTEX_TYPE_DEFAULT);
	req->condvar = condvar_create(CONDVAR_TYPE_DEFAULT);
	req->msg_type = msg_type;
	req->imsi = strdup(imsi);
	req->msg = msg;
	req->refcount = 1;

	return req;
}

static void gsup_request_destroy(gsup_request_t *this)
{
	if (!this)
	{
		return;
	}

	if (this->mutex)
	{
		this->mutex->destroy(this->mutex);
	}

	if (this->condvar)
	{
		this->condvar->destroy(this->condvar);
	}

	DESTROY_IF(this->lock);

	if (this->imsi)
	{
		free(this->imsi);
	}

	if (this->msg)
	{
		free(this->msg);
	}

	if (this->resp)
	{
		free(this->resp);
	}
	free(this);
}

static void gsup_request_get(gsup_request_t *this)
{
	ref_get(&this->refcount);
}

static void gsup_request_put(gsup_request_t *this)
{
	if (ref_put(&this->refcount))
	{
		gsup_request_destroy(this);
	}
}

#define IMSI_LEN 15
int imsi_copy(void *dest, const char *imsi)
{
	if (!imsi)
	{
		return -EINVAL;
	}

	if (strlen(imsi) != IMSI_LEN)
	{
		return -EINVAL;
	}
	memcpy(dest, imsi, IMSI_LEN);

	return 0;
}

static struct msgb *encode_to_msgb(struct osmo_gsup_message *gsup_msg)
{
	chunk_t msg_chunk;
	struct msgb *msg;
	int ret;

	msg_chunk = chunk_alloc(4000);
	if (msg_chunk.ptr == NULL)
	{
		return NULL;
	}

	msg = epdg_chunk_to_msgb(&msg_chunk);
	if (!msg)
	{
		goto free_msg;
	}

	/* reserve headroom */
	msgb_reserve(msg, 64);
	ret = osmo_gsup_encode(msg, gsup_msg);
	if (ret)
	{
		DBG1(DBG_NET, "epdg: gsupc: couldn't encode gsup message %d.", ret);
		goto free_msg;
	}

	return msg;

free_msg:
	chunk_free(&msg_chunk);
	return NULL;
}

static bool remove_pending(linked_list_t *list, gsup_request_t *req)
{
	enumerator_t *enumerator;
	gsup_request_t *ele = NULL;
	bool found = false;

	enumerator = list->create_enumerator(list);
	while (enumerator->enumerate(enumerator, (void **) &ele))
	{
		if (ele == req)
		{
			list->remove_at(list, enumerator);
			found = true;
			goto out;
		}
	}
out:
	enumerator->destroy(enumerator);
	return found;
}

static gsup_request_t *get_pending(linked_list_t *list, const char *imsi, enum osmo_gsup_message_type message_type)
{
	enumerator_t *enumerator;
	gsup_request_t *req = NULL;
	message_type = message_type & ~0b11;
	enumerator = list->create_enumerator(list);
	while (enumerator->enumerate(enumerator, (void **) &req))
	{
		if (strncmp(imsi, req->imsi, IMSI_LEN) == 0 && req->msg_type == message_type)
		{
			list->remove_at(list, enumerator);
			goto out;
		}
	}
	req = NULL;
out:
	enumerator->destroy(enumerator);
	return req;
}


/**
 * enqueue a message/request to be send out and wait for the response.
 *
 * when exiting enqueue, it must be guaranteed the req isn't referenced by anything.
 * The caller must hold a ref to req via get().
 * @param timeout_ms A timeout in ms
 * @return TRUE is the request timed out.
 */
static bool enqueue(private_osmo_epdg_gsup_client_t *this, gsup_request_t *req, u_int timeout_ms)
{
	bool ret = FALSE;
	DBG1(DBG_NET, "epdg: gsupc: Enqueuing message. Waiting %d ms for an answer", timeout_ms);

	req->mutex->lock(req->mutex);
	/* take a ref to have for the in/pending queue */
	gsup_request_get(req);
	this->inqueue->enqueue(this->inqueue, req);
	ret = req->condvar->timed_wait(req->condvar, req->mutex, timeout_ms);

	/* take owner ship / allow garbage free release.
	 * The owner ship isn't giving back. The rx/tx path will fail on try_write_lock() */
	req->lock->write_lock(req->lock);
	if (ret)
	{
		/* timed out */
		DBG1(DBG_NET, "epdg: gsupc: %s/%d Message timedout!", req->imsi, req->msg_type);
		void *found = this->inqueue->remove(this->inqueue, req);
		if (found)
		{
			/* give back the ref we took for the pending queue */
			gsup_request_put(req);
			return ret;
		}
		this->pending_mutex->lock(this->pending_mutex);
		bool found2 = remove_pending(this->pending, req);
		this->pending_mutex->unlock(this->pending_mutex);
		if (found2)
		{
			/* give back the ref we took for the pending queue */
			gsup_request_put(req);
			return ret;
		}
	}

	return ret;
}

METHOD(osmo_epdg_gsup_client_t, tunnel_request, osmo_epdg_gsup_response_t*,
        private_osmo_epdg_gsup_client_t *this, const char *imsi)
{
	struct osmo_gsup_message gsup_msg = {0};
	struct msgb *msg;
	/* PCO per 3GPP TS 24.008: header + {container ID (2) + length (1)}*
	 * 0x80   = ext=1, configuration protocol=0
	 * 0x000d = DNS Server IPv4 Address Request
	 * 0x0003 = DNS Server IPv6 Address Request
	 * 0x000c = P-CSCF IPv4 Address Request
	 * 0x0001 = P-CSCF IPv6 Address Request
	 */
	const char *pco = "\x80\x00\x0d\x00\x00\x03\x00\x00\x0c\x00\x00\x01\x00";
	bool timedout;

	DBG1(DBG_NET, "epdg: gsupc: Tunnel Request Request for %s", imsi);
	gsup_msg.message_type = OSMO_GSUP_MSGT_EPDG_TUNNEL_REQUEST;
	gsup_msg.current_rat_type = OSMO_RAT_EUTRAN_SGS;
	gsup_msg.message_class = OSMO_GSUP_MESSAGE_CLASS_IPSEC_EPDG;
	if (imsi_copy(gsup_msg.imsi, imsi))
	{
		/* TODO: inval imsi! */
		return NULL;
	}

	gsup_msg.pco = pco;
	gsup_msg.pco_len = 13;

	msg = encode_to_msgb(&gsup_msg);
	if (!msg)
	{
		DBG1(DBG_NET, "epdg: gsupc: Couldn't alloc/encode gsup message.");
		return NULL;
	}

	gsup_request_t *req = gsup_request_create(OSMO_GSUP_MSGT_EPDG_TUNNEL_REQUEST, imsi, msg);
	osmo_epdg_gsup_response_t *resp = NULL;
	timedout = enqueue(this, req, 5000);
	if (timedout)
	{
		gsup_request_put(req);
		return NULL;
	}

	resp = req->resp;
	req->resp = NULL;
	gsup_request_put(req);
	return resp;
}

METHOD(osmo_epdg_gsup_client_t, send_auth_request, osmo_epdg_gsup_response_t*,
        private_osmo_epdg_gsup_client_t *this, const char *imsi, uint8_t cn_domain,
	chunk_t *auts, chunk_t *auts_rand, const char *apn, uint8_t pdp_type)
{
	struct osmo_gsup_message gsup_msg = {0};
	struct msgb *msg;
	bool timedout;
	char apn_enc[APN_MAXLEN];
	size_t apn_enc_len = 0;
	int ret;

	DBG1(DBG_NET, "epdg: gsupc: Send Auth Request for %s", imsi);
	gsup_msg.message_type = OSMO_GSUP_MSGT_SEND_AUTH_INFO_REQUEST;
	gsup_msg.message_class = OSMO_GSUP_MESSAGE_CLASS_IPSEC_EPDG;
	gsup_msg.num_auth_vectors = 1;
	gsup_msg.current_rat_type = OSMO_RAT_EUTRAN_SGS;

	if (imsi_copy(gsup_msg.imsi, imsi))
	{
		/* TODO: inval imsi! */
		DBG1(DBG_NET, "epdg: gsupc: SAR: Invalid IMSI.");
		return NULL;
	}

	if (!apn || strlen(apn) == 0)
	{
		DBG1(DBG_NET, "epdg: gsupc: SAR: Invalid APN.");
		return NULL;
	}

	switch (cn_domain)
	{
		case 0:
			/* empty cn_domain */
			break;
		case OSMO_GSUP_CN_DOMAIN_PS:
		case OSMO_GSUP_CN_DOMAIN_CS:
			gsup_msg.cn_domain = cn_domain;
			break;
		default:
			DBG1(DBG_NET, "epdg: gsupc: SAR: Ignoring invalid cn_domain message.");
			break;
	}

	if (auts && auts->ptr && auts->len != 0)
	{
		if (auts->len != 14)
		{
			/* TODO: inval auts */
			return NULL;
		}

		gsup_msg.auts = auts->ptr;
	}

	/* TODO check for other sizes */
	if (auts_rand && auts_rand->ptr && auts_rand->len != 0)
	{
		if (auts_rand->len != 16)
		{
			/* TODO: inval auts */
			return NULL;
		}

		gsup_msg.rand = auts_rand->ptr;
	}

	/* Set context id to any value > 0 since libosmogsm doesn't encode APN if its 0. */
	gsup_msg.pdp_infos[0].context_id = 1;
	gsup_msg.pdp_infos[0].have_info = 1;
	gsup_msg.pdp_infos[0].pdp_type_nr = pdp_type;
	gsup_msg.pdp_infos[0].pdp_type_org = PDP_TYPE_ORG_IETF;

	ret = osmo_apn_from_str(apn_enc, APN_MAXLEN, apn);
	if (ret < 0)
	{
		DBG1(DBG_NET, "epdg: gsupc: Couldn't encode APN %s!", apn);
		return NULL;
	}
	apn_enc_len = ret;
	gsup_msg.pdp_infos[0].apn_enc = apn_enc;
	gsup_msg.pdp_infos[0].apn_enc_len = apn_enc_len;
	gsup_msg.num_pdp_infos = 1;

	msg = encode_to_msgb(&gsup_msg);
	if (!msg)
	{
		DBG1(DBG_NET, "epdg: gsupc: Couldn't alloc/encode gsup message.");
		return NULL;
	}

	gsup_request_t *req = gsup_request_create(OSMO_GSUP_MSGT_SEND_AUTH_INFO_REQUEST, imsi, msg);
	osmo_epdg_gsup_response_t *resp = NULL;
	timedout = enqueue(this, req, 5000);
	if (timedout)
	{
		DBG1(DBG_NET, "epdg: gsupc: Timeout request.");
		gsup_request_put(req);
		return NULL;
	}

	resp = req->resp;
	req->resp = NULL;
	gsup_request_put(req);

	return resp;
}

METHOD(osmo_epdg_gsup_client_t, update_location, osmo_epdg_gsup_response_t *,
        private_osmo_epdg_gsup_client_t *this, const char *imsi, uint8_t cn_domain)
{
	struct osmo_gsup_message gsup_msg = {0};
	struct msgb *msg;
	bool timedout;

	gsup_msg.message_type = OSMO_GSUP_MSGT_UPDATE_LOCATION_REQUEST;
	gsup_msg.message_class = OSMO_GSUP_MESSAGE_CLASS_IPSEC_EPDG;
	gsup_msg.current_rat_type = OSMO_RAT_EUTRAN_SGS;

	if (imsi_copy(gsup_msg.imsi, imsi))
	{
		/* TODO: inval imsi! */
		return NULL;
	}

	switch (cn_domain)
	{
		case 0:
			/* empty cn_domain */
			break;
		case OSMO_GSUP_CN_DOMAIN_PS:
		case OSMO_GSUP_CN_DOMAIN_CS:
			gsup_msg.cn_domain = cn_domain;
			break;
		default:
			DBG1(DBG_NET, "epdg: gsupc: ULR: Ignoring invalid cn_domain message.");
			break;
	}

	msg = encode_to_msgb(&gsup_msg);
	if (!msg)
	{
		DBG1(DBG_NET, "epdg: gsupc: ULR: Couldn't alloc/encode gsup message.");
		return NULL;
	}

	gsup_request_t *req = gsup_request_create(OSMO_GSUP_MSGT_UPDATE_LOCATION_REQUEST, imsi, msg);
	osmo_epdg_gsup_response_t *resp = NULL;
	timedout = enqueue(this, req, 5000);
	if (timedout)
	{
		gsup_request_put(req);
		return NULL;
	}

	resp = req->resp;
	req->resp = NULL;
	gsup_request_put(req);

	return resp;
}

METHOD(osmo_epdg_gsup_client_t, destroy, void,
        private_osmo_epdg_gsup_client_t *this)
{
	free(this->uri);
	free(this);
}

void tx_insert_data_result(private_osmo_epdg_gsup_client_t *this, const char *imsi, uint8_t cn_domain)
{
	struct osmo_gsup_message gsup_msg = {0};
	struct msgb *msg;

	gsup_msg.message_type = OSMO_GSUP_MSGT_INSERT_DATA_RESULT;
	if (imsi_copy(gsup_msg.imsi, imsi))
	{
		/* TODO: inval imsi! */
		return;
	}

	switch (cn_domain)
	{
		case 0:
			/* empty cn_domain */
			break;
		case OSMO_GSUP_CN_DOMAIN_PS:
		case OSMO_GSUP_CN_DOMAIN_CS:
			gsup_msg.cn_domain = cn_domain;
			break;
		default:
			DBG1(DBG_NET, "epdg: gsupc: ULR: Ignoring invalid cn_domain message.");
			break;
	}

	msg = encode_to_msgb(&gsup_msg);
	if (!msg)
	{
		DBG1(DBG_NET, "epdg: gsupc: ULR: Couldn't alloc/encode gsup message.");
	}
	this->ipa->send(this->ipa, IPAC_PROTO_EXT_GSUP, msg);
}

static void signal_request(gsup_request_t *req, osmo_epdg_gsup_response_t *resp)
{
	req->mutex->lock(req->mutex);
	req->resp = resp;
	req->condvar->signal(req->condvar);
	req->mutex->unlock(req->mutex);
}

static bool on_recv_pdu(void *data, osmo_epdg_ipa_client_t *client, struct msgb *pdu)
{
	private_osmo_epdg_gsup_client_t *this = data;
	gsup_request_t *req;
	osmo_epdg_gsup_response_t *resp;
	int ret;

	resp = calloc(1, sizeof(*resp));
	if (!resp)
	{
		free(pdu);
		return TRUE;
	}

	ret = osmo_gsup_decode(msgb_l2(pdu), msgb_l2len(pdu), &resp->gsup);
	if (ret) {
		// TODO: failed to decode response. Close connection
		goto out;
	}

	resp->pdu = pdu;
	DBG1(DBG_NET, "epdg: gsupc: receive gsup message %s/%d",
	     resp->gsup.imsi, resp->gsup.message_type);


	switch (resp->gsup.message_type)
	{
		case OSMO_GSUP_MSGT_INSERT_DATA_REQUEST:
			tx_insert_data_result(this, resp->gsup.imsi, resp->gsup.cn_domain);
			goto out;
		case OSMO_GSUP_MSGT_SEND_AUTH_INFO_ERROR:
		case OSMO_GSUP_MSGT_SEND_AUTH_INFO_RESULT:
		case OSMO_GSUP_MSGT_UPDATE_LOCATION_ERROR:
		case OSMO_GSUP_MSGT_UPDATE_LOCATION_RESULT:
		case OSMO_GSUP_MSGT_EPDG_TUNNEL_ERROR:
		case OSMO_GSUP_MSGT_EPDG_TUNNEL_RESULT:
			this->pending_mutex->lock(this->pending_mutex);
			req = get_pending(this->pending, resp->gsup.imsi, resp->gsup.message_type);
			if (!req)
			{
				this->pending_mutex->unlock(this->pending_mutex);
				DBG1(DBG_NET, "epdg: gsupc: receive gsup message where no matching response could be found. %s/%d",
				     resp->gsup.imsi, resp->gsup.message_type);
				goto out;
			}

			if (!req->lock->try_write_lock(req->lock))
			{
				/* Race Condition, Response came to late! */
				DBG1(DBG_NET, "epdg: gsupc: %s/%d: Can't aquire try_write_lock. Response too late",
				     resp->gsup.imsi, resp->gsup.message_type);
				this->pending_mutex->unlock(this->pending_mutex);
				gsup_request_put(req);
				goto out;
			}
			this->pending_mutex->unlock(this->pending_mutex);
			DBG1(DBG_NET, "epdg: gsupc: %s/%d: Informing requester. %p",
			     resp->gsup.imsi, resp->gsup.message_type, req);

			signal_request(req, resp);
			req->lock->unlock(req->lock);
			gsup_request_put(req);
			break;
		default:
			DBG1(DBG_NET, "epdg: gsupc: received unknown message type %02x", resp->gsup.message_type);
			goto out;
	}
	return TRUE;

out:
	free(resp);
	free(pdu);
	return TRUE;
}

static int disconnect_gsup(private_osmo_epdg_gsup_client_t *this)
{
	this->ipa->disconnect(this->ipa);
	return 0;
}

static void add_pending(private_osmo_epdg_gsup_client_t *this, gsup_request_t *req)
{
	this->pending_mutex->lock(this->pending_mutex);
	this->pending->insert_last(this->pending, req);
	this->pending_mutex->unlock(this->pending_mutex);
}

/* TODO: worker thread which sends out enqueue'd message ! */
static job_requeue_t queue_worker(private_osmo_epdg_gsup_client_t *this)
{
	int ret;
	gsup_request_t *req;

	/* should we multiple queue it? */
	/* TODO: replace pending with a thread safe queue, but non-blocking */
	req = this->inqueue->dequeue(this->inqueue);
	if (!req)
	{
		return JOB_REQUEUE_NONE;
	}

	if (!req->lock->try_write_lock(req->lock))
	{
		/* request is about to be released */
		gsup_request_put(req);
		return JOB_REQUEUE_NONE;
	}

	ret = this->ipa->send(this->ipa, IPAC_PROTO_EXT_GSUP, req->msg);
	req->msg = NULL;
	if (ret < 0)
	{
		/* TODO: disconnect & reconnect, but request is lost for now */
		/* TODO: wake up request */
		req->lock->unlock(req->lock);
		signal_request(req, NULL);
		gsup_request_put(req);
	} else {
		/* add to pending */
		add_pending(this, req);
		req->lock->unlock(req->lock);
	}

	return JOB_REQUEUE_FAIR;
}

osmo_epdg_gsup_client_t *osmo_epdg_gsup_client_create(char *uri)
{
	private_osmo_epdg_gsup_client_t *this;
	DBG1(DBG_NET, "epdg: gsupc: Starting");

	INIT(this,
			.public = {
				.send_auth_request = _send_auth_request,
				.update_location = _update_location,
				.tunnel_request = _tunnel_request,
				.destroy = _destroy,
			},
			.uri = strdup(uri),
			.inqueue = blocking_queue_create(),
			.pending = linked_list_create(),
			.pending_mutex = mutex_create(MUTEX_TYPE_DEFAULT),
			.ipa = osmo_epdg_ipa_client_create(uri),
		);
	this->ipa->on_recv(this->ipa, IPAC_PROTO_EXT_GSUP, on_recv_pdu, this);
	/* I would more like to have either an internal event which unblocks the queue. */
	/* src/libipsec/ipsec_event_relay.c */
        lib->processor->queue_job(lib->processor,
                (job_t*)callback_job_create_with_prio((callback_job_cb_t)queue_worker,
                        this, NULL, (callback_job_cancel_t)return_false, JOB_PRIO_CRITICAL));
	return &this->public;
}
