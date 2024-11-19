/* (C) 2018-2019 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
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


#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <talloc.h>

#include <osmocom/core/logging.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/fsm.h>

#include <osmocom/abis/ipa.h>
#include <osmocom/gsm/protocol/ipaccess.h>

#include "debug.h"
#include "asn1c_helpers.h"
#include "rspro_client_fsm.h"

#define S(x)	(1 << (x))

#define T1_WAIT_CLIENT_CONN_RES		10
#define T2_RECONNECT			10

static const int k_reestablish_delay_s[] = {
	0, 0, 0,                            // 3 immediate retries
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,       // 1 Hz for 30 seconds
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,       // 1/2 hz for 1 minute
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,       // 1/4 Hz for 2 minutes
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8,       // 1/8 Hz for 4 minutes
	16,                                 // 1/16 Hz thereafter
};

#define REESTABLISH_DELAY_COUNT ARRAY_SIZE(k_reestablish_delay_s)

/***********************************************************************
 * client-side FSM for a RSPRO connection to remsim-server
 *
 * This implements the TCP/IPA client side of an RSPRO connection between
 * a logical (TCP-level) client implementation and a remote RSPRO server.
 *
 * 'client' and 'server' here strictly refer to RSPRO protocol roles, not
 * to be confused with the remsim-client or remsim-server!
 *
 * Specifically, this RSPRO client FSM is used by both remsim-client and
 * remsim-bankd for their RSPRO control connection to remsim-server.
 ***********************************************************************/

static void push_and_send(struct ipa_client_conn *ipa, struct msgb *msg_tx)
{
	ipa_prepend_header_ext(msg_tx, IPAC_PROTO_EXT_RSPRO);
	ipa_prepend_header(msg_tx, IPAC_PROTO_OSMO);
	ipa_client_conn_send(ipa, msg_tx);
	/* msg_tx is now queued and will be freed. */
}

static int ipa_client_conn_send_rspro(struct ipa_client_conn *ipa, RsproPDU_t *rspro)
{
	struct msgb *msg = rspro_enc_msg(rspro);
	if (!msg) {
		LOGP(DRSPRO, LOGL_ERROR, "Error encoding RSPRO: %s\n", rspro_msgt_name(rspro));
		osmo_log_backtrace(DRSPRO, LOGL_ERROR);
		ASN_STRUCT_FREE(asn_DEF_RsproPDU, rspro);
		return -1;
	}
	push_and_send(ipa, msg);
	return 0;
}

static int _server_conn_send_rspro(struct rspro_server_conn *srvc, RsproPDU_t *rspro)
{
	LOGPFSML(srvc->fi, LOGL_DEBUG, "Tx RSPRO %s\n", rspro_msgt_name(rspro));
	return ipa_client_conn_send_rspro(srvc->conn, rspro);
}

int server_conn_send_rspro(struct rspro_server_conn *srvc, RsproPDU_t *rspro)
{
	if (!rspro) {
		LOGPFSML(srvc->fi, LOGL_ERROR, "Attempt to transmit NULL\n");
		osmo_log_backtrace(DRSPRO, LOGL_ERROR);
		return -EINVAL;
	}
	if (osmo_fsm_inst_dispatch(srvc->fi, SRVC_E_RSPRO_TX, rspro) < 0) {
		ASN_STRUCT_FREE(asn_DEF_RsproPDU, rspro);
		return -EPERM;
	}
	return 0;
}

enum server_conn_fsm_state {
	/* waiting for initial connection to remsim-server */
	SRVC_ST_INIT,
	/* server connection established, waiting for ClientConnectRes */
	SRVC_ST_ESTABLISHED,
	/* server connection established, ClientConnect succeeded */
	SRVC_ST_CONNECTED,
	/* connection lost, 1st step: delaying until we re-establish */
	SRVC_ST_REESTABLISH_DELAY,
	/* connection lost, 2nd step: wait for a re-establish */
	SRVC_ST_REESTABLISH,
};

static const struct value_string server_conn_fsm_event_names[] = {
	OSMO_VALUE_STRING(SRVC_E_ESTABLISH),
	OSMO_VALUE_STRING(SRVC_E_DISCONNECT),
	OSMO_VALUE_STRING(SRVC_E_TCP_UP),
	OSMO_VALUE_STRING(SRVC_E_TCP_DOWN),
	OSMO_VALUE_STRING(SRVC_E_KA_TIMEOUT),
	OSMO_VALUE_STRING(SRVC_E_KA_TERMINATED),
	OSMO_VALUE_STRING(SRVC_E_CLIENT_CONN_RES),
	OSMO_VALUE_STRING(SRVC_E_RSPRO_TX),
	{ 0, NULL }
};

static void srvc_updown_cb(struct ipa_client_conn *conn, int up)
{
	struct rspro_server_conn *srvc = conn->data;

	LOGPFSML(srvc->fi, LOGL_NOTICE, "RSPRO link to %s:%d %s\n",
		 conn->addr, conn->port, up ? "UP" : "DOWN");

	osmo_fsm_inst_dispatch(srvc->fi, up ? SRVC_E_TCP_UP: SRVC_E_TCP_DOWN, 0);
}

static int srvc_read_cb(struct ipa_client_conn *conn, struct msgb *msg)
{
	struct ipaccess_head *hh = (struct ipaccess_head *) msg->data;
	struct ipaccess_head_ext *he = (struct ipaccess_head_ext *) msgb_l2(msg);
	struct rspro_server_conn *srvc = conn->data;
	RsproPDU_t *pdu;
	int rc;

	if (msgb_length(msg) < sizeof(*hh))
		goto invalid;
	msg->l2h = &hh->data[0];
	switch (hh->proto) {
	case IPAC_PROTO_IPACCESS:
		rc = ipaccess_bts_handle_ccm(srvc->conn, &srvc->ipa_dev, msg);
		if (rc < 0) {
			msgb_free(msg);
			break;
		}
		switch (hh->data[0]) {
		case IPAC_MSGT_PONG:
			ipa_keepalive_fsm_pong_received(srvc->keepalive_fi);
			rc = 0;
			break;
		default:
			break;
		}
		break;
	case IPAC_PROTO_OSMO:
		if (!he || msgb_l2len(msg) < sizeof(*he))
			goto invalid;
		msg->l2h = &he->data[0];
		switch (he->proto) {
		case IPAC_PROTO_EXT_RSPRO:
			LOGPFSML(srvc->fi, LOGL_DEBUG, "Received RSPRO %s\n", msgb_hexdump(msg));
			pdu = rspro_dec_msg(msg);
			if (!pdu) {
				rc = -EIO;
				break;
			}
			rc = srvc->handle_rx(srvc, pdu);
			ASN_STRUCT_FREE(asn_DEF_RsproPDU, pdu);
			break;
		default:
			goto invalid;
		}
		break;
	default:
		goto invalid;
	}

	msgb_free(msg);
	return rc;

invalid:
	LOGPFSML(srvc->fi, LOGL_ERROR, "Error decoding PDU\n");
	msgb_free(msg);
	return -1;
}

static const struct ipa_keepalive_params ka_params = {
	.interval = 30,
	.wait_for_resp = 10,
};

static int64_t get_monotonic_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	return ((1000LL * t.tv_sec) + (t.tv_nsec / 1000000));
}

static void srvc_do_reestablish(struct osmo_fsm_inst *fi)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	const int64_t since_last_ms = get_monotonic_ms() - srvc->reestablish_last_ms;

	/* reset delay loop if it has been > 2x the longest timeout since our last attempt;
	 * this lets us revert to rapid reconnect behavior for a good connection */
	const int64_t reset_ms = 2*1000*(OSMO_MAX(OSMO_MAX(T1_WAIT_CLIENT_CONN_RES, T2_RECONNECT),
		k_reestablish_delay_s[REESTABLISH_DELAY_COUNT-1]));

	if (since_last_ms > reset_ms) {
		srvc->reestablish_delay_idx = 0;
		LOGPFSML(fi, LOGL_DEBUG, "->REESTABLISH_DELAY reset; %" PRId64 "ms since last attempt\n",
			since_last_ms);
	}

	/* determine if we need to delay reestablishment */
	const int64_t need_ms = (int64_t) k_reestablish_delay_s[srvc->reestablish_delay_idx] * 1000;
	int64_t delay_ms = need_ms - since_last_ms;

	if (delay_ms > 0) {
		LOGPFSML(fi, LOGL_DEBUG, "->REESTABLISH_DELAY delay %" PRId64 "ms; %" PRId64 "ms since last attempt [step %zu/%zu@%ds]\n",
			delay_ms, since_last_ms, srvc->reestablish_delay_idx, (REESTABLISH_DELAY_COUNT-1),
			k_reestablish_delay_s[srvc->reestablish_delay_idx]);
	} else {
		/* cheat and always use a minimum delay of 1ms to ensure a fsm timeout is triggered */
		delay_ms = 1;
	}

	osmo_fsm_inst_state_chg_ms(fi, SRVC_ST_REESTABLISH_DELAY, delay_ms, 3);
}

static void srvc_st_init(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case SRVC_E_ESTABLISH:
	default:
		OSMO_ASSERT(0);
	}
}

static void srvc_st_established_onenter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;
	RsproPDU_t *pdu;

	ipa_keepalive_fsm_start(srvc->keepalive_fi);

	if (srvc->own_comp_id.type == ComponentType_remsimClient)
		pdu = rspro_gen_ConnectClientReq(&srvc->own_comp_id, srvc->clslot);
	else
		pdu = rspro_gen_ConnectBankReq(&srvc->own_comp_id, srvc->bankd.bank_id,
					       srvc->bankd.num_slots);
	_server_conn_send_rspro(srvc, pdu);
}

static void srvc_st_established(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;
	RsproPDU_t *pdu = NULL;
	e_ResultCode res;

	switch (event) {
	case SRVC_E_TCP_DOWN:
	case SRVC_E_KA_TIMEOUT:
		srvc_do_reestablish(fi);
		break;
	case SRVC_E_CLIENT_CONN_RES:
		pdu = data;
		res = rspro_get_result(pdu);
		if (res != ResultCode_ok) {
			LOGPFSML(fi, LOGL_ERROR, "Rx RSPRO connectClientRes(result=%s), closing\n",
				 asn_enum_name(&asn_DEF_ResultCode, res));
			ipa_client_conn_close(srvc->conn);
			osmo_fsm_inst_dispatch(fi, SRVC_E_TCP_DOWN, NULL);
		} else {
			/* somehow notify the main code? */
			osmo_fsm_inst_state_chg(fi, SRVC_ST_CONNECTED, 0, 0);
		}
		break;
	default:
		OSMO_ASSERT(0);
	}
}

static void srvc_st_connected_onenter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	if (fi->proc.parent)
		osmo_fsm_inst_dispatch(fi->proc.parent, srvc->parent_conn_evt, NULL);
}

static void srvc_st_connected(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;
	RsproPDU_t *pdu = NULL;

	switch (event) {
	case SRVC_E_TCP_DOWN:
	case SRVC_E_KA_TIMEOUT:
		srvc_do_reestablish(fi);
		break;
	case SRVC_E_RSPRO_TX:
		pdu = data;
		_server_conn_send_rspro(srvc, pdu);
		break;
	default:
		OSMO_ASSERT(0);
	}
}

static void srvc_st_connected_onleave(struct osmo_fsm_inst *fi, uint32_t next_state)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	if (fi->proc.parent)
		osmo_fsm_inst_dispatch(fi->proc.parent, srvc->parent_disc_evt, NULL);
}

static int ipa_kaepalive_timeout_cb(struct osmo_fsm_inst *ka_fi, void *conn)
{
	struct osmo_fsm_inst *fi = ka_fi->proc.parent;
	osmo_fsm_inst_dispatch(fi, SRVC_E_KA_TIMEOUT, NULL);
	return 0; /* we will explicitly terminate it */
}

static void srvc_st_reestablish_delay_onenter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	if (srvc->keepalive_fi) {
		ipa_keepalive_fsm_stop(srvc->keepalive_fi);
		osmo_fsm_inst_term(srvc->keepalive_fi, OSMO_FSM_TERM_REGULAR, NULL);
		srvc->keepalive_fi = NULL;
	}

	if (srvc->conn) {
		LOGPFSML(fi, LOGL_INFO, "Destroying existing connection to server\n");
		ipa_client_conn_close(srvc->conn);
		ipa_client_conn_destroy(srvc->conn);
		srvc->conn = NULL;
	}

	/* saturate timeout at last (longest) entry */
	if (srvc->reestablish_delay_idx < REESTABLISH_DELAY_COUNT-1) {
		srvc->reestablish_delay_idx++;
	}
}

static void srvc_st_reestablish_delay(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	default:
		OSMO_ASSERT(0);
	}
}
static void srvc_st_reestablish_onenter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;
	int rc;

	srvc->reestablish_last_ms = get_monotonic_ms();

	LOGPFSML(fi, LOGL_INFO, "Creating TCP connection to server at %s:%u\n",
		 srvc->server_host, srvc->server_port);
	srvc->conn = ipa_client_conn_create2(fi, NULL, 0, NULL, 0, srvc->server_host, srvc->server_port,
						srvc_updown_cb, srvc_read_cb, NULL, srvc);
	if (!srvc->conn) {
		LOGPFSML(fi, LOGL_FATAL, "Unable to create socket: %s\n", strerror(errno));
		exit(1);
	}

	srvc->keepalive_fi = ipa_client_conn_alloc_keepalive_fsm(srvc->conn, &ka_params, fi->id);
	if (!srvc->keepalive_fi) {
		LOGPFSM(fi, "Unable to create keepalive FSM\n");
		exit(1);
	}
	ipa_keepalive_fsm_set_timeout_cb(srvc->keepalive_fi, ipa_kaepalive_timeout_cb);
	/* ensure parent is notified once keepalive FSM instance is dying */
	osmo_fsm_inst_change_parent(srvc->keepalive_fi, srvc->fi, SRVC_E_KA_TERMINATED);

	/* Attempt to connect TCP socket */
	rc = ipa_client_conn_open(srvc->conn);
	if (rc < 0) {
		LOGPFSML(fi, LOGL_FATAL, "Unable to connect RSPRO to %s:%u - %s\n",
			srvc->server_host, srvc->server_port, strerror(errno));
		/* FIXME: retry? Timer? Abort? */
		OSMO_ASSERT(0);
	}
}

static void srvc_st_reestablish(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case SRVC_E_TCP_UP:
		osmo_fsm_inst_state_chg(fi, SRVC_ST_ESTABLISHED, T1_WAIT_CLIENT_CONN_RES, 1);
		break;
	case SRVC_E_TCP_DOWN:
		/* wait for normal T2 call-back */
		break;
	default:
		OSMO_ASSERT(0);
	}
}

static void srvc_allstate_action(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	switch (event) {
	case SRVC_E_ESTABLISH:
		/* reset delay connect immediately on our first connection */
		srvc->reestablish_delay_idx = 0;
		srvc->reestablish_last_ms = 0;
		srvc_do_reestablish(fi);
		break;
	case SRVC_E_DISCONNECT:
		if (srvc->keepalive_fi) {
			ipa_keepalive_fsm_stop(srvc->keepalive_fi);
			osmo_fsm_inst_term(srvc->keepalive_fi, OSMO_FSM_TERM_REGULAR, NULL);
			srvc->keepalive_fi = NULL;
		}
		if (srvc->conn) {
			LOGPFSML(fi, LOGL_INFO, "Destroying existing connection to server\n");
			ipa_client_conn_close(srvc->conn);
			ipa_client_conn_destroy(srvc->conn);
			srvc->conn = NULL;
		}
		osmo_fsm_inst_state_chg(fi, SRVC_ST_INIT, 0, 0);
		break;
	default:
		OSMO_ASSERT(0);
	}
}

static int server_conn_fsm_timer_cb(struct osmo_fsm_inst *fi)
{
	struct rspro_server_conn *srvc = (struct rspro_server_conn *) fi->priv;

	switch (fi->T) {
	case 3:
		/* delay has expired; let's re-establish */
		osmo_fsm_inst_state_chg(fi, SRVC_ST_REESTABLISH, T2_RECONNECT, 2);
		break;
	case 2:
		/* TCP reconnect failed: retry after wait */
		srvc_do_reestablish(fi);
		break;
	case 1:
		/* no ClientConnectRes received: disconnect + reconnect */
		ipa_client_conn_close(srvc->conn);
		osmo_fsm_inst_dispatch(fi, SRVC_E_TCP_DOWN, NULL);
		break;
	default:
		OSMO_ASSERT(0);
	}

	return 0;
}

static const struct osmo_fsm_state server_conn_fsm_states[] = {
	[SRVC_ST_INIT] = {
		.name = "INIT",
		.in_event_mask = 0, /* S(SRVC_E_ESTABLISH) via allstate */
		.out_state_mask = S(SRVC_ST_INIT) | S(SRVC_ST_REESTABLISH_DELAY),
		.action = srvc_st_init,
	},
	[SRVC_ST_ESTABLISHED] = {
		.name = "ESTABLISHED",
		.in_event_mask = S(SRVC_E_TCP_DOWN) | S(SRVC_E_KA_TIMEOUT) | S(SRVC_E_CLIENT_CONN_RES),
		.out_state_mask = S(SRVC_ST_CONNECTED) | S(SRVC_ST_REESTABLISH_DELAY) | S(SRVC_ST_INIT),
		.action = srvc_st_established,
		.onenter = srvc_st_established_onenter,
	},
	[SRVC_ST_CONNECTED] = {
		.name = "CONNECTED",
		.in_event_mask = S(SRVC_E_TCP_DOWN) | S(SRVC_E_KA_TIMEOUT) | S(SRVC_E_RSPRO_TX),
		.out_state_mask = S(SRVC_ST_REESTABLISH_DELAY) | S(SRVC_ST_INIT),
		.action = srvc_st_connected,
		.onenter = srvc_st_connected_onenter,
		.onleave = srvc_st_connected_onleave,
	},
	[SRVC_ST_REESTABLISH_DELAY] = {
		.name = "REESTABLISH_DELAY",
		.in_event_mask = 0,
		.out_state_mask = S(SRVC_ST_REESTABLISH) | S(SRVC_ST_INIT),
		.action = srvc_st_reestablish_delay,
		.onenter = srvc_st_reestablish_delay_onenter,
	},
	[SRVC_ST_REESTABLISH] = {
		.name = "REESTABLISH",
		.in_event_mask = S(SRVC_E_TCP_UP) | S(SRVC_E_TCP_DOWN),
		.out_state_mask = S(SRVC_ST_ESTABLISHED) | S(SRVC_ST_REESTABLISH_DELAY) | S(SRVC_ST_INIT),
		.action = srvc_st_reestablish,
		.onenter = srvc_st_reestablish_onenter,
	},
};

struct osmo_fsm rspro_client_server_fsm = {
	.name = "RSPRO_CLIENT",
	.states = server_conn_fsm_states,
	.num_states = ARRAY_SIZE(server_conn_fsm_states),
	.allstate_event_mask = S(SRVC_E_ESTABLISH) | S(SRVC_E_DISCONNECT),
	.allstate_action = srvc_allstate_action,
	.timer_cb = server_conn_fsm_timer_cb,
	.log_subsys = DRSPRO,
	.event_names = server_conn_fsm_event_names,
};

int server_conn_fsm_alloc(void *ctx, struct rspro_server_conn *srvc)
{
	struct osmo_fsm_inst *fi;

	fi = osmo_fsm_inst_alloc(&rspro_client_server_fsm, ctx, srvc, LOGL_DEBUG, "server");
	if (!fi)
		return -1;

	srvc->fi = fi;
	srvc->reestablish_delay_idx = 0;
	srvc->reestablish_last_ms = 0;

	return 0;
}

static __attribute__((constructor)) void on_dso_load(void)
{
	OSMO_ASSERT(osmo_fsm_register(&rspro_client_server_fsm) == 0);
}
