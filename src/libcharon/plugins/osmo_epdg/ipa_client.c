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

#include <collections/enumerator.h>
#include <collections/linked_list.h>
#include <collections/blocking_queue.h>
#include <processing/jobs/callback_job.h>
#include <threading/mutex.h>
#include <threading/thread.h>
#include <threading/condvar.h>

#include <osmocom/core/msgb.h>
#include <osmocom/gsm/protocol/ipaccess.h>
#include <osmocom/gsm/ipa.h>

#include <library.h>

#include <errno.h>

#include "ipa_client.h"
#include "osmo_epdg_utils.h"

#define INITIAL_BACKOFF_MS 10
#define MAX_BACKOFF_MS 15000

typedef struct private_osmo_epdg_ipa_client_t private_osmo_epdg_ipa_client_t;
struct private_osmo_epdg_ipa_client_t {
	/**
	 * Public osmo_epdg_ipa_client_t
	 */
	osmo_epdg_ipa_client_t public;

	char *uri;

	stream_t *stream;

	ipa_cb_t osmo_cb;
	void *osmo_cb_data;

	mutex_t *mutex;
	bool reconnecting;
	uint32_t reconnect_backoff_ms;
	uint32_t reconnect_backoff_max_ms;
};

static void reconnect_ipa(private_osmo_epdg_ipa_client_t *this);

METHOD(osmo_epdg_ipa_client_t, on_error, int,
        private_osmo_epdg_ipa_client_t *this, uint8_t osmo_proto, ipa_cb_t cb, void *data)
{
	/* TODO: on error ? */
	return TRUE;
}


METHOD(osmo_epdg_ipa_client_t, on_recv, int,
        private_osmo_epdg_ipa_client_t *this, uint8_t osmo_proto, ipa_cb_t cb, void *data)
{
	/* TODO: protect it by mutex?! */
	this->osmo_cb = NULL;
	this->osmo_cb_data = data;
	this->osmo_cb = cb;

	return TRUE;
}

METHOD(osmo_epdg_ipa_client_t, destroy, void,
        private_osmo_epdg_ipa_client_t *this)
{
	free(this->uri);
	free(this);
}

METHOD(osmo_epdg_ipa_client_t, disconnect, int,
        private_osmo_epdg_ipa_client_t *this)
{
	if (this->stream)
	{
		this->stream->destroy(this->stream);
	}
	this->stream = NULL;
	return 0;
}

METHOD(osmo_epdg_ipa_client_t, send_pdu, ssize_t,
        private_osmo_epdg_ipa_client_t *this, uint8_t osmo_proto, struct msgb *msg)
{
	struct ipaccess_head *head;
	int len;
	head = (struct ipaccess_head *) msgb_push(msg, sizeof(struct ipaccess_head) + 1);
	head->proto = IPAC_PROTO_OSMO;
	head->len = htons(msgb_length(msg) - (sizeof(struct ipaccess_head)));
	head->data[0] = osmo_proto;
	len = msgb_length(msg);
	if (!this->stream)
	{
		free(msg);
		return -ENOENT;
	}

	if (!this->stream->write_all(this->stream, msgb_data(msg), msgb_length(msg)))
	{
		// TODO: write error
		free(msg);
		return -EINVAL;
	}

	free(msg);
	return len;
}

static bool read_error(private_osmo_epdg_ipa_client_t *this, int read_err)
{
	DBG1(DBG_NET, "IPA client failed to read with %d. Reconnecting", read_err);
	reconnect_ipa(this);
	return FALSE;
}

/* send a simple IPA PDU with the base protocol */
static void ipa_pdu_base_send_simple(private_osmo_epdg_ipa_client_t *this, uint8_t msg_type)
{
	struct ipaccess_head *head = calloc(1, sizeof(*head) + 1);
	head->proto = IPAC_PROTO_IPACCESS;
	head->len = htons(1);
	head->data[0] = msg_type;

	if (!this->stream->write_all(this->stream, head, sizeof(*head) + 1))
	{
		/* TODO: write error */
	}

	free(head);
	return;
}

static inline void ipa_tag_put_str(struct msgb *resp, uint8_t tag, const char *value)
{
		char *buf;
		ssize_t len = strlen(value);

		msgb_put_u16(resp, len + 1);
		msgb_put_u8(resp, tag);
		buf = msgb_put(resp, len);
		memcpy(buf, value, len);
}

static void ipa_resp_tag_encode(private_osmo_epdg_ipa_client_t *this, struct msgb *resp, uint8_t tag)
{
	switch (tag)
	{
		case IPAC_IDTAG_SERNR:
		case IPAC_IDTAG_UNITNAME:
			ipa_tag_put_str(resp, tag, "SWAN-00-00-00-00-00-00");
			break;
		case IPAC_IDTAG_LOCATION1:
		case IPAC_IDTAG_LOCATION2:
		case IPAC_IDTAG_EQUIPVERS:
		case IPAC_IDTAG_SWVERSION:
			ipa_tag_put_str(resp, tag, "00:00:00:00:00:00");
			break;
		case IPAC_IDTAG_UNIT:
			ipa_tag_put_str(resp, tag, "0/0/0");
			break;
	}
}

static void protocol_error(private_osmo_epdg_ipa_client_t *this, char *error_msg)
{
	/* TODO: protocol error */
	DBG1(DBG_NET, error_msg);
	return;
}
/* send a IPA PDU (base protocol) ID Response message */
static void ipa_pdu_base_send_id_resp(private_osmo_epdg_ipa_client_t *this, struct msgb *req)
{
	struct msgb *resp;
	struct ipaccess_head *resp_head;

	chunk_t resp_pdu = chunk_alloc(IPA_ALLOC_SIZE);
	if (!resp_pdu.ptr)
	{
		/* TODO: alloc err */
		return;
	}
	
	resp = epdg_chunk_to_msgb(&resp_pdu);

	/* remove the ipaccess header so we can use msg_pull on the message */
	msgb_pull(req, sizeof(struct ipaccess_head));
	if (msgb_length(req) < 1)
	{
		protocol_error(this, "Invalid IPA ID Request message.");
		goto out;
	}

	/* prepare our response message */
	msgb_reserve(resp, 128);
	resp_head = (struct ipaccess_head *) msgb_put(resp, sizeof(struct ipaccess_head));
	resp->l1h = (void *)resp_head;
	resp->l2h = resp->tail;
	resp_head->proto = IPAC_PROTO_IPACCESS;
	msgb_put_u8(resp, IPAC_MSGT_ID_RESP);
	/* remove IPA message type */
	msgb_pull_u8(req);

	/* ID Request contains: a list of [0x1, tag] */
	while (msgb_length(req) >= 2)
	{
		uint8_t len = msgb_pull_u8(req);
		if (len != 1)
		{
			if (msgb_length(req) < len)
			{
				protocol_error(this, "Invalid IPA ID Request message");
				goto out;
			}
			/* ignoring this requested LValue */
			DBG1(DBG_NET, "IPA ignoring IPA ID Request tag with size != 1");
			msgb_pull(req, len);
			continue;
		}
		uint8_t tag = msgb_pull_u8(req);
		ipa_resp_tag_encode(this, resp, tag);
	}
	resp_head->len = htons(msgb_l2len(resp));
	if (!this->stream->write_all(this->stream, msgb_l1(resp), msgb_l1len(resp)))
	{
		// TODO: write error
		return;
	}

	ipa_pdu_base_send_simple(this, IPAC_MSGT_ID_ACK);
out:
	chunk_free(&resp_pdu);
	return;
}

static void on_recv_ipa_pdu(private_osmo_epdg_ipa_client_t *this, struct msgb *pdu)
{
	if (msgb_length(pdu) < sizeof(struct ipaccess_head) + 1)
	{
		/* TODO: invalid package */
		return;
	}

	struct ipaccess_head *head = (struct ipaccess_head *) msgb_data(pdu);
	uint8_t msg_type = head->data[0];
	switch (msg_type)
	{
		case IPAC_MSGT_PING:
			ipa_pdu_base_send_simple(this, IPAC_MSGT_PONG);
			break;
		case IPAC_MSGT_PONG:
			/* ignore. We don't implement an own PING/PONG timer */
			break;
		case IPAC_MSGT_ID_GET:
			ipa_pdu_base_send_id_resp(this, pdu);
			break;
		case IPAC_MSGT_ID_ACK:
			/* ignore. An ACK means everything the ID got accepted */
			break;
		default:
			DBG1(DBG_NET, "IPA client Received an unknown IPA PDU %02x", msg_type);
			break;
	}
}

CALLBACK(on_stream_read, bool,
	private_osmo_epdg_ipa_client_t *this, stream_t *stream)
{
	uint16_t len;
	ssize_t hlen;
	chunk_t req_chunk;
	struct ipaccess_head head;
	struct msgb *req;

	DBG2(DBG_NET, "on stream read!");
	hlen = stream->read(stream, &head, sizeof(head), FALSE);
	if (hlen <= 0)
	{
		if (errno == EWOULDBLOCK)
		{
			DBG2(DBG_NET, "on stream read EWOULDBLOCK!");
			return TRUE;
		}

		DBG2(DBG_NET, "on stream errno not EWOULDBLOCK %d!", hlen);
		return read_error(this, errno);
	}
	DBG2(DBG_NET, "on stream hlen %d!", hlen);
	if (hlen < sizeof(head))
	{
		if (!stream->read_all(stream, ((void*)&head) + hlen, sizeof(head) - hlen))
		{
			return read_error(this, errno);
		}
	}
	len = ntohs(head.len);
	if ((len + sizeof(head)) > IPA_ALLOC_SIZE)
	{
		/* TODO: pkg too big */
		return read_error(this, EINVAL);
	}

	req_chunk = chunk_alloc(IPA_ALLOC_SIZE + sizeof(struct msgb));
	if (!req_chunk.ptr)
	{
		/* TODO: -ENOMEM; */
		return FALSE;
	}
	req = epdg_chunk_to_msgb(&req_chunk);
	memcpy(msgb_put(req, sizeof(head)), &head, sizeof(head));
	/* TODO: either wait here with a timeout or don't care on this stream read */
	if (!stream->read_all(stream, msgb_put(req, len), len))
	{
		chunk_free(&req_chunk);
		return read_error(this, errno);
	}

	switch (head.proto)
	{
		case IPAC_PROTO_IPACCESS:
			on_recv_ipa_pdu(this, req);
			break;
		case IPAC_PROTO_OSMO:
			/* will take care of the response */
			if (msgb_length(req) < sizeof(struct ipaccess_head) + 1)
			{
				/* TODO: inval pdu */
				chunk_free(&req_chunk);
				break;
			}
			req->l1h = req->head;
			req->l2h = req->l1h + sizeof(struct ipaccess_head) + 1;
			DBG2(DBG_NET, "IPA client: pushing osmo pdu");
			if (this->osmo_cb)
			{
				this->osmo_cb(this->osmo_cb_data, &this->public, req);
			}
			else
			{
				chunk_free(&req_chunk);
			}
			break;
		default:
			DBG1(DBG_NET, "IPA client: ignoring unknown proto %02x", head.proto);
			chunk_free(&req_chunk);
			break;
	}
	return TRUE;
}

static int connect_ipa(private_osmo_epdg_ipa_client_t *this)
{
	
	DBG1(DBG_NET, "IPA client connecting to %s", this->uri);
	if (this->stream != NULL)
	{
		DBG1(DBG_NET, "closing old ipa conncetion %s", this->uri);
		this->public.disconnect(&this->public);
	}

	this->stream = lib->streams->connect(lib->streams, this->uri);
	if (!this->stream)
	{
		DBG1(DBG_NET, "failed to connect the ipa %s", this->uri);
		reconnect_ipa(this);
		return -EINVAL;
	}
	DBG1(DBG_NET, "IPA client connected");

	/* TODO: check if we need this
	 * ensure we get the first bytes after the connect
	 * on_stream_read(this, this->stream); */

	/* TODO: can a race happen here when data arrives between those 2 calls? */
	this->stream->on_read(this->stream, on_stream_read, this);
	on_stream_read(this, this->stream);

	/* might want to move the reset of the backoff even later */
	this->reconnect_backoff_ms = INITIAL_BACKOFF_MS;

	return 0;
}

static job_requeue_t reconnect_job(private_osmo_epdg_ipa_client_t *this)
{
	DBG1(DBG_NET, "IPA: Reconnect job. %s %d", this->uri, this->reconnect_backoff_max_ms);
	this->mutex->lock(this->mutex);

	/* hopefully this doesn't lock too long */
	if (connect_ipa(this))
	{
		this->reconnect_backoff_ms = this->reconnect_backoff_ms * 2;
		if (this->reconnect_backoff_ms > this->reconnect_backoff_max_ms)
		{
			this->reconnect_backoff_ms = this->reconnect_backoff_max_ms;
		}
		DBG1(DBG_NET, "failed to re-connect the ipa %s. Reconnecting in %d ms", this->uri, this->reconnect_backoff_ms);
		lib->scheduler->schedule_job_ms(lib->scheduler, (job_t*)
						callback_job_create((callback_job_cb_t)reconnect_job,
						this, NULL, NULL), this->reconnect_backoff_ms);
		this->mutex->unlock(this->mutex);
		return JOB_REQUEUE_NONE;
	}

	this->reconnecting = FALSE;
	this->mutex->unlock(this->mutex);
	return JOB_REQUEUE_NONE;
}

static void reconnect_ipa(private_osmo_epdg_ipa_client_t *this)
{
	DBG1(DBG_NET, "IPA: Reconnect_IPA. %s %d", this->uri, this->reconnect_backoff_max_ms);
	this->mutex->lock(this->mutex);
	if (this->reconnecting)
	{
		this->mutex->unlock(this->mutex);
		return;
	}

	this->reconnecting = TRUE;
	lib->scheduler->schedule_job_ms(lib->scheduler, (job_t*)
					callback_job_create((callback_job_cb_t)reconnect_job,
					this, NULL, NULL), this->reconnect_backoff_ms);
	this->mutex->unlock(this->mutex);
}

osmo_epdg_ipa_client_t *osmo_epdg_ipa_client_create(char *uri)
{
	private_osmo_epdg_ipa_client_t *this;

	INIT(this,
			.public = {
				.on_recv = _on_recv,
				.on_error = _on_error,
				.send = _send_pdu,
				.disconnect = _disconnect,
				.destroy = _destroy,
			},
			.uri = strdup(uri),
			.stream = NULL,
			.mutex = mutex_create(MUTEX_TYPE_RECURSIVE),
			.reconnect_backoff_max_ms = MAX_BACKOFF_MS,
			.reconnect_backoff_ms = INITIAL_BACKOFF_MS,
		);
	
	connect_ipa(this);
	return &this->public;
}
