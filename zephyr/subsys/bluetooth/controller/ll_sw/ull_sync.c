/*
 * Copyright (c) 2020-2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <soc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/hci_types.h>

#include "util/util.h"
#include "util/mem.h"
#include "util/memq.h"
#include "util/mayfly.h"
#include "util/dbuf.h"

#include "hal/cpu.h"
#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/ticker.h"

#include "ticker/ticker.h"

#include "pdu_df.h"
#include "lll/pdu_vendor.h"
#include "pdu.h"

#include "lll.h"
#include "lll/lll_adv_types.h"
#include "lll_adv.h"
#include "lll/lll_adv_pdu.h"
#include "lll_clock.h"
#include "lll/lll_vendor.h"
#include "lll_chan.h"
#include "lll_scan.h"
#include "lll/lll_df_types.h"
#include "lll_conn.h"
#include "lll_conn_iso.h"
#include "lll_sync.h"
#include "lll_sync_iso.h"

#include "isoal.h"

#include "ull_tx_queue.h"

#include "ull_filter.h"
#include "ull_iso_types.h"
#include "ull_scan_types.h"
#include "ull_sync_types.h"
#include "ull_conn_types.h"
#include "ull_adv_types.h"
#include "ull_conn_iso_types.h"

#include "ull_internal.h"
#include "ull_adv_internal.h"
#include "ull_scan_internal.h"
#include "ull_sync_internal.h"
#include "ull_conn_internal.h"
#include "ull_conn_iso_internal.h"
#include "ull_df_types.h"
#include "ull_df_internal.h"

#include "ull_llcp.h"
#include "ll.h"

#include <soc.h>
#include "hal/debug.h"

/* Check that timeout_reload member is at safe offset when ll_sync_set is
 * allocated using mem interface. timeout_reload being non-zero is used to
 * indicate that a sync is established. And is used to check for sync being
 * terminated under race conditions between HCI Tx and Rx thread when
 * Periodic Advertising Reports are generated.
 */
MEM_FREE_MEMBER_ACCESS_BUILD_ASSERT(struct ll_sync_set, timeout_reload);

static struct ll_sync_set *ull_sync_create(uint8_t sid, uint16_t timeout, uint16_t skip,
					   uint8_t cte_type, uint8_t rx_enable, uint8_t nodups);
static int init_reset(void);
static inline struct ll_sync_set *sync_acquire(void);
static void sync_ticker_cleanup(struct ll_sync_set *sync, ticker_op_func stop_op_cb);
static void ticker_cb(uint32_t ticks_at_expire, uint32_t ticks_drift,
		      uint32_t remainder, uint16_t lazy, uint8_t force,
		      void *param);
static void ticker_start_op_cb(uint32_t status, void *param);
static void ticker_update_op_cb(uint32_t status, void *param);
static void ticker_stop_sync_expire_op_cb(uint32_t status, void *param);
static void sync_expire(void *param);
static void ticker_stop_sync_lost_op_cb(uint32_t status, void *param);
static void sync_lost(void *param);
#if defined(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)
static bool peer_sid_sync_exists(uint8_t const peer_id_addr_type,
				 uint8_t const *const peer_id_addr,
				 uint8_t sid);
#endif /* CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC */
#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING) && \
	!defined(CONFIG_BT_CTLR_CTEINLINE_SUPPORT)
static struct pdu_cte_info *pdu_cte_info_get(struct pdu_adv *pdu);
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING && !CONFIG_BT_CTLR_CTEINLINE_SUPPORT */

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
static void ticker_update_op_status_give(uint32_t status, void *param);
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

static struct ll_sync_set ll_sync_pool[CONFIG_BT_PER_ADV_SYNC_MAX];
static void *sync_free;

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
/* Semaphore to wakeup thread on ticker API callback */
static struct k_sem sem_ticker_cb;
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

uint8_t ll_sync_create(uint8_t options, uint8_t sid, uint8_t adv_addr_type,
			    uint8_t *adv_addr, uint16_t skip,
			    uint16_t sync_timeout, uint8_t sync_cte_type)
{
	struct ll_scan_set *scan_coded;
	struct ll_scan_set *scan;
	struct ll_sync_set *sync;
	uint8_t rx_enable;
	uint8_t nodups;

	scan = ull_scan_set_get(SCAN_HANDLE_1M);
	if (!scan || scan->periodic.sync) {
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan_coded = ull_scan_set_get(SCAN_HANDLE_PHY_CODED);
		if (!scan_coded || scan_coded->periodic.sync) {
			return BT_HCI_ERR_CMD_DISALLOWED;
		}
	}

#if defined(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)
	/* Do not sync twice to the same peer and same SID */
	if (((options & BT_HCI_LE_PER_ADV_CREATE_SYNC_FP_USE_LIST) == 0U) &&
	    peer_sid_sync_exists(adv_addr_type, adv_addr, sid)) {
		return BT_HCI_ERR_CONN_ALREADY_EXISTS;
	}
#endif /* CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC */

	rx_enable = !(options & BT_HCI_LE_PER_ADV_CREATE_SYNC_FP_REPORTS_DISABLED);
	nodups = (options & BT_HCI_LE_PER_ADV_CREATE_SYNC_FP_FILTER_DUPLICATE) ? 1U : 0U;

	sync = ull_sync_create(sid, sync_timeout, skip, sync_cte_type, rx_enable, nodups);
	if (!sync) {
		return BT_HCI_ERR_MEM_CAPACITY_EXCEEDED;
	}

	scan->periodic.cancelled = 0U;
	scan->periodic.state = LL_SYNC_STATE_IDLE;
	scan->periodic.param = NULL;
	scan->periodic.filter_policy =
		options & BT_HCI_LE_PER_ADV_CREATE_SYNC_FP_USE_LIST;
	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan_coded->periodic.cancelled = 0U;
		scan_coded->periodic.state = LL_SYNC_STATE_IDLE;
		scan_coded->periodic.param = NULL;
		scan_coded->periodic.filter_policy =
			scan->periodic.filter_policy;
	}

	if (!scan->periodic.filter_policy) {
		sync->peer_id_addr_type = adv_addr_type;
		(void)memcpy(sync->peer_id_addr, adv_addr, BDADDR_SIZE);
	}

	/* Remember the peer address when periodic advertiser list is not
	 * used.
	 * NOTE: Peer address will be filled/overwritten with correct identity
	 * address on sync setup when privacy is enabled.
	 */
	if ((options & BT_HCI_LE_PER_ADV_CREATE_SYNC_FP_USE_LIST) == 0U) {
		sync->peer_id_addr_type = adv_addr_type;
		(void)memcpy(sync->peer_id_addr, adv_addr,
			     sizeof(sync->peer_id_addr));
	}

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
	/* Set filter policy in lll_sync */
	sync->lll.filter_policy = scan->periodic.filter_policy;
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

	/* Enable scanner to create sync */
	scan->periodic.sync = sync;

#if defined(CONFIG_BT_CTLR_FILTER_ACCEPT_LIST)
	scan->lll.is_sync = 1U;
#endif /* CONFIG_BT_CTLR_FILTER_ACCEPT_LIST */
	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan_coded->periodic.sync = sync;

#if defined(CONFIG_BT_CTLR_FILTER_ACCEPT_LIST)
		scan_coded->lll.is_sync = 1U;
#endif /* CONFIG_BT_CTLR_FILTER_ACCEPT_LIST */
	}

	return 0;
}

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER)
void ull_sync_setup_from_sync_transfer(struct ll_conn *conn, uint16_t service_data,
				       struct ll_sync_set *sync, struct pdu_adv_sync_info *si,
				       int16_t conn_evt_offset, uint16_t last_pa_event_counter,
				       uint16_t sync_conn_event_count, uint8_t sender_sca)
{
	struct node_rx_past_received *se_past;
	uint32_t ticks_slot_overhead;
	uint32_t ticks_slot_offset;
	uint32_t conn_interval_us;
	uint32_t sync_offset_us;
	uint32_t ready_delay_us;
	struct node_rx_pdu *rx;
	uint8_t *data_chan_map;
	struct lll_sync *lll;
	uint32_t interval_us;
	uint32_t slot_us;
	uint32_t ticks_anchor;
	uint8_t chm_last;
	uint32_t ret;
	uint16_t interval;
	uint16_t sync_handle;
	uint8_t sca;

	lll = &sync->lll;

	/* Copy channel map from sca_chm field in sync_info structure, and
	 * clear the SCA bits.
	 */
	chm_last = lll->chm_first;
	lll->chm_last = chm_last;
	data_chan_map = lll->chm[chm_last].data_chan_map;
	(void)memcpy(data_chan_map, si->sca_chm,
		     sizeof(lll->chm[chm_last].data_chan_map));
	data_chan_map[PDU_SYNC_INFO_SCA_CHM_SCA_BYTE_OFFSET] &=
		~PDU_SYNC_INFO_SCA_CHM_SCA_BIT_MASK;
	lll->chm[chm_last].data_chan_count =
		util_ones_count_get(data_chan_map,
				    sizeof(lll->chm[chm_last].data_chan_map));
	if (lll->chm[chm_last].data_chan_count < CHM_USED_COUNT_MIN) {
		/* Ignore sync setup, invalid available channel count */
		return;
	}

	memcpy(lll->access_addr, si->aa, sizeof(lll->access_addr));
	lll->data_chan_id = lll_chan_id(lll->access_addr);
	memcpy(lll->crc_init, si->crc_init, sizeof(lll->crc_init));
	lll->event_counter = sys_le16_to_cpu(si->evt_cntr);

	interval = sys_le16_to_cpu(si->interval);
	interval_us = interval * PERIODIC_INT_UNIT_US;

	/* Convert fromm 10ms units to interval units */
	if (sync->timeout != 0  && interval_us != 0) {
		sync->timeout_reload = RADIO_SYNC_EVENTS((sync->timeout * 10U *
						  USEC_PER_MSEC), interval_us);
	}

	/* Adjust Skip value so that there is minimum of 6 events that can be
	 * listened to before Sync_Timeout occurs.
	 * The adjustment of the skip value is controller implementation
	 * specific and not specified by the Bluetooth Core Specification v5.3.
	 * The Controller `may` use the Skip value, and the implementation here
	 * covers a case where Skip value could lead to less events being
	 * listened to until Sync_Timeout. Listening to more consecutive events
	 * before Sync_Timeout increases probability of retaining the Periodic
	 * Synchronization.
	 */
	if (sync->timeout_reload > CONN_ESTAB_COUNTDOWN) {
		uint16_t skip_max = sync->timeout_reload - CONN_ESTAB_COUNTDOWN;

		if (sync->skip > skip_max) {
			sync->skip = skip_max;
		}
	}

	sync->sync_expire = CONN_ESTAB_COUNTDOWN;

	/* Extract the SCA value from the sca_chm field of the sync_info
	 * structure.
	 */
	sca = (si->sca_chm[PDU_SYNC_INFO_SCA_CHM_SCA_BYTE_OFFSET] &
	       PDU_SYNC_INFO_SCA_CHM_SCA_BIT_MASK) >>
	      PDU_SYNC_INFO_SCA_CHM_SCA_BIT_POS;

	lll->sca = sca;

	lll->window_widening_periodic_us =
		DIV_ROUND_UP(((lll_clock_ppm_local_get() +
				   lll_clock_ppm_get(sca)) *
				  interval_us), USEC_PER_SEC);
	lll->window_widening_max_us = (interval_us >> 1) - EVENT_IFS_US;
	if (PDU_ADV_SYNC_INFO_OFFS_UNITS_GET(si)) {
		lll->window_size_event_us = OFFS_UNIT_300_US;
	} else {
		lll->window_size_event_us = OFFS_UNIT_30_US;
	}

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
	lll->node_cte_incomplete = NULL;
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

	/* Prepare Periodic Advertising Sync Transfer Received event (dispatched later) */
	sync_handle = ull_sync_handle_get(sync);
	rx = (void *)sync->node_rx_sync_estab;
	rx->hdr.type = NODE_RX_TYPE_SYNC_TRANSFER_RECEIVED;
	rx->hdr.handle = sync_handle;
	rx->rx_ftr.param = sync;

	/* Create node_rx and assign values */
	se_past = (void *)rx->pdu;
	se_past->rx_sync.status = BT_HCI_ERR_SUCCESS;
	se_past->rx_sync.interval = interval;
	se_past->rx_sync.phy = sync->lll.phy;
	se_past->rx_sync.sca = sca;
	se_past->conn_handle = ll_conn_handle_get(conn);
	se_past->service_data = service_data;

	conn_interval_us = conn->lll.interval * CONN_INT_UNIT_US;

	/* Calculate offset and schedule sync radio events */
	ready_delay_us = lll_radio_rx_ready_delay_get(lll->phy, PHY_FLAGS_S8);

	sync_offset_us = PDU_ADV_SYNC_INFO_OFFSET_GET(si) * lll->window_size_event_us;
	/* offs_adjust may be 1 only if sync setup by LL_PERIODIC_SYNC_IND */
	sync_offset_us += (PDU_ADV_SYNC_INFO_OFFS_ADJUST_GET(si) ? OFFS_ADJUST_US : 0U);
	sync_offset_us -= EVENT_TICKER_RES_MARGIN_US;
	sync_offset_us -= EVENT_JITTER_US;
	sync_offset_us -= ready_delay_us;

	if (conn_evt_offset) {
		int64_t conn_offset_us = (int64_t)conn_evt_offset * conn_interval_us;

		if ((int64_t)sync_offset_us + conn_offset_us < 0) {
			uint32_t total_offset_us = llabs((int64_t)sync_offset_us + conn_offset_us);
			uint32_t sync_intervals = DIV_ROUND_UP(total_offset_us, interval_us);

			lll->event_counter += sync_intervals;
			sync_offset_us = (sync_intervals * interval_us) - total_offset_us;
		} else {
			sync_offset_us += conn_offset_us;
		}
	}

	/* Calculate initial window widening - see Core Spec vol 6, part B, 5.1.13.1 */
	{
		uint16_t event_delta;
		uint32_t drift_us;
		uint64_t da;
		uint64_t db;
		uint64_t d;

		const uint32_t local_sca_ppm = lll_clock_ppm_local_get();

		event_delta = lll->event_counter - last_pa_event_counter;

		da = (uint64_t)(local_sca_ppm + lll_clock_ppm_get(sca)) * interval_us;
		da = DIV_ROUND_UP(da * (uint64_t)event_delta, USEC_PER_SEC);

		db = (uint64_t)(local_sca_ppm + lll_clock_ppm_get(sender_sca)) * conn_interval_us;
		db = DIV_ROUND_UP(db * (uint64_t)(ull_conn_event_counter(conn) -
						  sync_conn_event_count), USEC_PER_SEC);

		d = DIV_ROUND_UP((da + db) * (USEC_PER_SEC + local_sca_ppm +
					      lll_clock_ppm_get(sca) +
					      lll_clock_ppm_get(sender_sca)), USEC_PER_SEC);

		/* Limit drift compenstion to the maximum window widening */
		drift_us = MIN((uint32_t)d, lll->window_widening_max_us);

		/* Apply total drift to initial window size */
		lll->window_size_event_us += drift_us;

		/* Adjust offset if less than the drift compensation */
		while (sync_offset_us < drift_us) {
			sync_offset_us += interval_us;
			lll->event_counter++;
		}

		sync_offset_us -= drift_us;
	}

	interval_us -= lll->window_widening_periodic_us;

	/* Calculate event time reservation */
	slot_us = PDU_AC_MAX_US(PDU_AC_EXT_PAYLOAD_RX_SIZE, lll->phy);
	slot_us += ready_delay_us;

	/* Add implementation defined radio event overheads */
	if (IS_ENABLED(CONFIG_BT_CTLR_EVENT_OVERHEAD_RESERVE_MAX)) {
		slot_us += EVENT_OVERHEAD_START_US + EVENT_OVERHEAD_END_US;
	}

	sync->ull.ticks_slot = HAL_TICKER_US_TO_TICKS_CEIL(slot_us);

	ticks_slot_offset = HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_XTAL_US);
	if (IS_ENABLED(CONFIG_BT_CTLR_LOW_LAT)) {
		ticks_slot_overhead = ticks_slot_offset;
	} else {
		ticks_slot_overhead = 0U;
	}
	ticks_slot_offset += HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);

	sync->lll_sync_prepare = lll_sync_create_prepare;

	ticks_anchor = conn->llcp.prep.ticks_at_expire;

#if defined(CONFIG_BT_PERIPHERAL)
	if (conn->lll.role == BT_HCI_ROLE_PERIPHERAL) {
		/* Compensate for window widening */
		ticks_anchor += HAL_TICKER_US_TO_TICKS(conn->lll.periph.window_widening_event_us);
	}
#endif /* CONFIG_BT_PERIPHERAL */

	ret = ticker_start(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_ULL_HIGH,
			   (TICKER_ID_SCAN_SYNC_BASE + sync_handle),
			   ticks_anchor,
			   HAL_TICKER_US_TO_TICKS(sync_offset_us),
			   HAL_TICKER_US_TO_TICKS(interval_us),
			   HAL_TICKER_REMAINDER(interval_us),
			   TICKER_NULL_LAZY,
			   (sync->ull.ticks_slot + ticks_slot_overhead),
			   ticker_cb, sync,
			   ticker_start_op_cb, (void *)__LINE__);
	LL_ASSERT((ret == TICKER_STATUS_SUCCESS) ||
		  (ret == TICKER_STATUS_BUSY));
}
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER */


uint8_t ll_sync_create_cancel(void **rx)
{
	struct ll_scan_set *scan_coded;
	memq_link_t *link_sync_estab;
	memq_link_t *link_sync_lost;
	struct node_rx_pdu *node_rx;
	struct ll_scan_set *scan;
	struct ll_sync_set *sync;
	struct node_rx_sync *se;

	scan = ull_scan_set_get(SCAN_HANDLE_1M);
	if (!scan || !scan->periodic.sync) {
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan_coded = ull_scan_set_get(SCAN_HANDLE_PHY_CODED);
		if (!scan_coded || !scan_coded->periodic.sync) {
			return BT_HCI_ERR_CMD_DISALLOWED;
		}
	}

	/* Check for race condition where in sync is established when sync
	 * create cancel is invoked.
	 *
	 * Setting `scan->periodic.cancelled` to represent cancellation
	 * requested in the thread context. Checking `scan->periodic.sync` for
	 * NULL confirms if synchronization was established before
	 * `scan->periodic.cancelled` was set to 1U.
	 */
	scan->periodic.cancelled = 1U;
	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan_coded->periodic.cancelled = 1U;
	}
	cpu_dmb();
	sync = scan->periodic.sync;
	if (!sync) {
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	/* node_rx_sync_estab is assigned when Host calls create sync and cleared when sync is
	 * established. timeout_reload is set when sync is found and setup. It is non-zero until
	 * sync is terminated. Together they give information about current sync state:
	 * - node_rx_sync_estab == NULL && timeout_reload != 0 => sync is established
	 * - node_rx_sync_estab == NULL && timeout_reload == 0 => sync is terminated
	 * - node_rx_sync_estab != NULL && timeout_reload == 0 => sync is created
	 * - node_rx_sync_estab != NULL && timeout_reload != 0 => sync is waiting to be established
	 */
	if (!sync->node_rx_sync_estab) {
		/* There is no sync to be cancelled */
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	sync->is_stop = 1U;
	cpu_dmb();

	if (sync->timeout_reload != 0U) {
		uint16_t sync_handle = ull_sync_handle_get(sync);

		LL_ASSERT(sync_handle <= UINT8_MAX);

		/* Sync is not established yet, so stop sync ticker */
		const int err =
			ull_ticker_stop_with_mark((TICKER_ID_SCAN_SYNC_BASE +
						   (uint8_t)sync_handle),
						  sync, &sync->lll);
		if (err != 0 && err != -EALREADY) {
			return BT_HCI_ERR_CMD_DISALLOWED;
		}
	} /* else: sync was created but not yet setup, there is no sync ticker yet. */

	/* It is safe to remove association with scanner as cancelled flag is
	 * set, sync is_stop flag was set and sync has not been established.
	 */
	ull_sync_setup_reset(sync);

	/* Mark the sync context as sync create cancelled */
	if (IS_ENABLED(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)) {
		sync->timeout = 0U;
	}

	node_rx = sync->node_rx_sync_estab;
	link_sync_estab = node_rx->hdr.link;
	link_sync_lost = sync->node_rx_lost.rx.hdr.link;

	ll_rx_link_release(link_sync_lost);
	ll_rx_link_release(link_sync_estab);
	ll_rx_release(node_rx);

	/* Clear the node after release to mark the sync establish as being completed.
	 * In this case the completion reason is sync cancelled by Host.
	 */
	sync->node_rx_sync_estab = NULL;

	node_rx = (void *)&sync->node_rx_lost;
	node_rx->hdr.type = NODE_RX_TYPE_SYNC;
	node_rx->hdr.handle = LLL_HANDLE_INVALID;

	/* NOTE: struct node_rx_lost has uint8_t member following the
	 *       struct node_rx_hdr to store the reason.
	 */
	se = (void *)node_rx->pdu;
	se->status = BT_HCI_ERR_OP_CANCELLED_BY_HOST;

	/* NOTE: Since NODE_RX_TYPE_SYNC is only generated from ULL context,
	 *       pass ULL sync context as parameter.
	 */
	node_rx->rx_ftr.param = sync;

	*rx = node_rx;

	return 0;
}

uint8_t ll_sync_terminate(uint16_t handle)
{
	struct lll_scan_aux *lll_aux;
	memq_link_t *link_sync_lost;
	struct ll_sync_set *sync;
	int err;

	sync = ull_sync_is_enabled_get(handle);
	if (!sync) {
		return BT_HCI_ERR_UNKNOWN_ADV_IDENTIFIER;
	}

	/* Request terminate, no new ULL scheduling to be setup */
	sync->is_stop = 1U;
	cpu_dmb();

	/* Stop periodic sync ticker timeouts */
	err = ull_ticker_stop_with_mark(TICKER_ID_SCAN_SYNC_BASE + handle,
					sync, &sync->lll);
	LL_ASSERT_INFO2(err == 0 || err == -EALREADY, handle, err);
	if (err) {
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	/* Check and stop any auxiliary PDU receptions */
	lll_aux = sync->lll.lll_aux;
	if (lll_aux) {
#if defined(CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS)
		err = ull_scan_aux_stop(&sync->lll);
#else /* !CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS */
		struct ll_scan_aux_set *aux;

		aux = HDR_LLL2ULL(lll_aux);
		err = ull_scan_aux_stop(aux);
#endif /* !CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS */
		if (err && (err != -EALREADY)) {
			return BT_HCI_ERR_CMD_DISALLOWED;
		}

#if !defined(CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS)
		LL_ASSERT(!aux->parent);
#endif /* !CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS */
	}

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER)
	/* Clean up node_rx_sync_estab if still present */
	if (sync->node_rx_sync_estab) {
		memq_link_t *link_sync_estab;
		struct node_rx_pdu *node_rx;

		node_rx = (void *)sync->node_rx_sync_estab;
		link_sync_estab = node_rx->hdr.link;

		ll_rx_link_release(link_sync_estab);
		ll_rx_release(node_rx);

		sync->node_rx_sync_estab = NULL;
	}
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER */

	link_sync_lost = sync->node_rx_lost.rx.hdr.link;
	ll_rx_link_release(link_sync_lost);

	/* Mark sync context not sync established */
	sync->timeout_reload = 0U;

	ull_sync_release(sync);

	return 0;
}

/* @brief Link Layer interface function corresponding to HCI LE Set Periodic
 *        Advertising Receive Enable command.
 *
 * @param[in] handle Sync_Handle identifying the periodic advertising
 *                   train. Range: 0x0000 to 0x0EFF.
 * @param[in] enable Bit number 0 - Reporting Enabled.
 *                   Bit number 1 - Duplicate filtering enabled.
 *                   All other bits - Reserved for future use.
 *
 * @return HCI error codes as documented in Bluetooth Core Specification v5.3.
 */
uint8_t ll_sync_recv_enable(uint16_t handle, uint8_t enable)
{
	struct ll_sync_set *sync;

	sync = ull_sync_is_enabled_get(handle);
	if (!sync) {
		return BT_HCI_ERR_UNKNOWN_ADV_IDENTIFIER;
	}

	/* Reporting enabled/disabled */
	sync->rx_enable = (enable & BT_HCI_LE_SET_PER_ADV_RECV_ENABLE_ENABLE) ?
			  1U : 0U;

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_ADI_SUPPORT)
	sync->nodups = (enable & BT_HCI_LE_SET_PER_ADV_RECV_ENABLE_FILTER_DUPLICATE) ?
		       1U : 0U;
#endif

	return 0;
}

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_SENDER)
/* @brief Link Layer interface function corresponding to HCI LE Set Periodic
 *        Advertising Sync Transfer command.
 *
 * @param[in] conn_handle Connection_Handle identifying the connected device
 *                        Range: 0x0000 to 0x0EFF.
 * @param[in] service_data Service_Data value provided by the Host for use by the
 *                         Host of the peer device.
 * @param[in] sync_handle Sync_Handle identifying the periodic advertising
 *                        train. Range: 0x0000 to 0x0EFF.
 *
 * @return HCI error codes as documented in Bluetooth Core Specification v5.4.
 */
uint8_t ll_sync_transfer(uint16_t conn_handle, uint16_t service_data, uint16_t sync_handle)
{
	struct ll_sync_set *sync;
	struct ll_conn *conn;

	conn = ll_connected_get(conn_handle);
	if (!conn) {
		return BT_HCI_ERR_UNKNOWN_CONN_ID;
	}

	/* Verify that sync_handle is valid */
	sync = ull_sync_is_enabled_get(sync_handle);
	if (!sync) {
		return BT_HCI_ERR_UNKNOWN_ADV_IDENTIFIER;
	}

	/* Call llcp to start LLCP_PERIODIC_SYNC_IND */
	return ull_cp_periodic_sync(conn, sync, NULL, service_data);
}
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_SENDER */

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER)
/* @brief Link Layer interface function corresponding to HCI LE Set Periodic
 *        Advertising Sync Transfer Parameters command.
 *
 * @param[in] conn_handle Connection_Handle identifying the connected device
 *                        Range: 0x0000 to 0x0EFF.
 * @param[in] mode Mode specifies the action to be taken when a periodic advertising
 *                 synchronization is received.
 * @param[in] skip Skip specifying the number of consectutive periodic advertising
 *                 packets that the receiver may skip after successfully reciving a
 *                 periodic advertising packet. Range: 0x0000 to 0x01F3.
 * @param[in] timeout Sync_timeout specifying the maximum permitted time between
 *                    successful receives. Range: 0x000A to 0x4000.
 * @param[in] cte_type CTE_Type specifying whether to only synchronize to periodic
 *                     advertising with certain types of Constant Tone Extension.
 *
 * @return HCI error codes as documented in Bluetooth Core Specification v5.4.
 */
uint8_t ll_past_param(uint16_t conn_handle, uint8_t mode, uint16_t skip, uint16_t timeout,
		      uint8_t cte_type)
{
	struct ll_conn *conn;

	conn = ll_connected_get(conn_handle);
	if (!conn) {
		return BT_HCI_ERR_UNKNOWN_CONN_ID;
	}

	if (mode == BT_HCI_LE_PAST_MODE_SYNC_FILTER_DUPLICATES &&
	    !IS_ENABLED(CONFIG_BT_CTLR_SYNC_PERIODIC_ADI_SUPPORT)) {
		return BT_HCI_ERR_UNSUPP_FEATURE_PARAM_VAL;
	}

	/* Set PAST Param for connection instance */
	conn->past.mode     = mode;
	conn->past.skip     = skip;
	conn->past.timeout  = timeout;
	conn->past.cte_type = cte_type;

	return 0;
}

/* @brief Link Layer interface function corresponding to HCI LE Set Default Periodic
 *        Advertising Sync Transfer Parameters command.
 *
 * @param[in] mode Mode specifies the action to be taken when a periodic advertising
 *                   synchronization is received.
 * @param[in] skip Skip specifying the number of consectutive periodic advertising
 *                   packets that the receiver may skip after successfully reciving a
 *                   periodic advertising packet. Range: 0x0000 to 0x01F3.
 * @param[in] timeout Sync_timeout specifying the maximum permitted time between
 *                    successful receives. Range: 0x000A to 0x4000.
 * @param[in] cte_type CTE_Type specifying whether to only synchronize to periodic
 *                   advertising with certain types of Constant Tone Extension.
 *
 * @return HCI error codes as documented in Bluetooth Core Specification v5.4.
 */
uint8_t ll_default_past_param(uint8_t mode, uint16_t skip, uint16_t timeout, uint8_t cte_type)
{
	if (mode == BT_HCI_LE_PAST_MODE_SYNC_FILTER_DUPLICATES &&
	    !IS_ENABLED(CONFIG_BT_CTLR_SYNC_PERIODIC_ADI_SUPPORT)) {
		return BT_HCI_ERR_UNSUPP_FEATURE_PARAM_VAL;
	}

	/* Set default past param */
	ull_conn_default_past_param_set(mode, skip, timeout, cte_type);

	return 0;
}
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER */

int ull_sync_init(void)
{
	int err;

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

int ull_sync_reset(void)
{
	uint16_t handle;
	void *rx;
	int err;

	(void)ll_sync_create_cancel(&rx);

	for (handle = 0U; handle < CONFIG_BT_PER_ADV_SYNC_MAX; handle++) {
		(void)ll_sync_terminate(handle);
	}

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

struct ll_sync_set *ull_sync_set_get(uint16_t handle)
{
	if (handle >= CONFIG_BT_PER_ADV_SYNC_MAX) {
		return NULL;
	}

	return &ll_sync_pool[handle];
}

struct ll_sync_set *ull_sync_is_enabled_get(uint16_t handle)
{
	struct ll_sync_set *sync;

	sync = ull_sync_set_get(handle);
	if (!sync || !sync->timeout_reload) {
		return NULL;
	}

	return sync;
}

struct ll_sync_set *ull_sync_is_valid_get(struct ll_sync_set *sync)
{
	if (((uint8_t *)sync < (uint8_t *)ll_sync_pool) ||
	    ((uint8_t *)sync > ((uint8_t *)ll_sync_pool +
	     (sizeof(struct ll_sync_set) * (CONFIG_BT_PER_ADV_SYNC_MAX - 1))))) {
		return NULL;
	}

	return sync;
}

struct lll_sync *ull_sync_lll_is_valid_get(struct lll_sync *lll)
{
	struct ll_sync_set *sync;

	sync = HDR_LLL2ULL(lll);
	sync = ull_sync_is_valid_get(sync);
	if (sync) {
		return &sync->lll;
	}

	return NULL;
}

uint16_t ull_sync_handle_get(struct ll_sync_set *sync)
{
	return mem_index_get(sync, ll_sync_pool, sizeof(struct ll_sync_set));
}

uint16_t ull_sync_lll_handle_get(struct lll_sync *lll)
{
	return ull_sync_handle_get(HDR_LLL2ULL(lll));
}

void ull_sync_release(struct ll_sync_set *sync)
{
#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
	struct lll_sync *lll = &sync->lll;

	if (lll->node_cte_incomplete) {
		const uint8_t release_cnt = 1U;
		struct node_rx_pdu *node_rx;
		memq_link_t *link;

		node_rx = &lll->node_cte_incomplete->rx;
		link = node_rx->hdr.link;

		ll_rx_link_release(link);
		ull_iq_report_link_inc_quota(release_cnt);
		ull_df_iq_report_mem_release(node_rx);
		ull_df_rx_iq_report_alloc(release_cnt);

		lll->node_cte_incomplete = NULL;
	}
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

	/* Mark the sync context as sync create cancelled */
	if (IS_ENABLED(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)) {
		sync->timeout = 0U;
	}

#if !defined(CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS)
	/* reset accumulated data len */
	sync->data_len = 0U;
#endif /* !CONFIG_BT_CTLR_SCAN_AUX_USE_CHAINS */

	mem_release(sync, &sync_free);
}

bool ull_sync_setup_addr_check(struct ll_sync_set *sync, uint8_t filter_policy,
			       uint8_t addr_type, uint8_t *addr, uint8_t rl_idx)
{
	/* Check if Periodic Advertiser list to be used */
	if (IS_ENABLED(CONFIG_BT_CTLR_SYNC_PERIODIC_ADV_LIST) &&
	    filter_policy) {
		/* Check in Periodic Advertiser List */
		if (ull_filter_ull_pal_addr_match(addr_type, addr)) {
			/* Remember the address, to check with
			 * SID in Sync Info
			 */
			sync->peer_id_addr_type = addr_type;
			(void)memcpy(sync->peer_id_addr, addr,
				     BDADDR_SIZE);

			/* Address matched */
			return true;

		/* Check in Resolving List */
		} else if (IS_ENABLED(CONFIG_BT_CTLR_PRIVACY) &&
			   ull_filter_ull_pal_listed(rl_idx, &addr_type,
						     sync->peer_id_addr)) {
			/* Remember the address, to check with the
			 * SID in Sync Info
			 */
			sync->peer_id_addr_type = addr_type;

			/* Mark it as identity address from RPA */
			sync->peer_addr_resolved = 1U;

			/* Address matched */
			return true;
		}

	/* Check with explicitly supplied address */
	} else if ((addr_type == sync->peer_id_addr_type) &&
		   !memcmp(addr, sync->peer_id_addr, BDADDR_SIZE)) {
		/* Address matched */
		return true;

	/* Check identity address with explicitly supplied address */
	} else if (IS_ENABLED(CONFIG_BT_CTLR_PRIVACY) &&
		   (rl_idx < ll_rl_size_get())) {
		ll_rl_id_addr_get(rl_idx, &addr_type, addr);
		if ((addr_type == sync->peer_id_addr_type) &&
		    !memcmp(addr, sync->peer_id_addr, BDADDR_SIZE)) {
			/* Mark it as identity address from RPA */
			sync->peer_addr_resolved = 1U;

			/* Identity address matched */
			return true;
		}
	}

	return false;
}

bool ull_sync_setup_sid_match(struct ll_sync_set *sync, struct ll_scan_set *scan, uint8_t sid)
{
	return (scan->periodic.state == LL_SYNC_STATE_ADDR_MATCH) &&
		((IS_ENABLED(CONFIG_BT_CTLR_SYNC_PERIODIC_ADV_LIST) &&
		  scan->periodic.filter_policy &&
		  ull_filter_ull_pal_match(sync->peer_id_addr_type,
					   sync->peer_id_addr, sid)) ||
		 (!scan->periodic.filter_policy &&
		  (sid == sync->sid)));
}

void ull_sync_setup(struct ll_scan_set *scan, uint8_t phy,
		    struct node_rx_pdu *node_rx, struct pdu_adv_sync_info *si)
{
	uint32_t ticks_slot_overhead;
	uint32_t ticks_slot_offset;
	struct ll_sync_set *sync;
	struct node_rx_sync *se;
	struct node_rx_ftr *ftr;
	uint32_t sync_offset_us;
	uint32_t ready_delay_us;
	struct node_rx_pdu *rx;
	uint8_t *data_chan_map;
	struct lll_sync *lll;
	uint16_t sync_handle;
	uint32_t interval_us;
	uint32_t overhead_us;
	struct pdu_adv *pdu;
	uint16_t interval;
	uint32_t slot_us;
	uint8_t chm_last;
	uint32_t ret;
	uint8_t sca;

	/* Populate the LLL context */
	sync = scan->periodic.sync;
	lll = &sync->lll;

	/* Copy channel map from sca_chm field in sync_info structure, and
	 * clear the SCA bits.
	 */
	chm_last = lll->chm_first;
	lll->chm_last = chm_last;
	data_chan_map = lll->chm[chm_last].data_chan_map;
	(void)memcpy(data_chan_map, si->sca_chm,
		     sizeof(lll->chm[chm_last].data_chan_map));
	data_chan_map[PDU_SYNC_INFO_SCA_CHM_SCA_BYTE_OFFSET] &=
		~PDU_SYNC_INFO_SCA_CHM_SCA_BIT_MASK;
	lll->chm[chm_last].data_chan_count =
		util_ones_count_get(data_chan_map,
				    sizeof(lll->chm[chm_last].data_chan_map));
	if (lll->chm[chm_last].data_chan_count < CHM_USED_COUNT_MIN) {
		/* Ignore sync setup, invalid available channel count */
		return;
	}

	memcpy(lll->access_addr, si->aa, sizeof(lll->access_addr));
	lll->data_chan_id = lll_chan_id(lll->access_addr);
	memcpy(lll->crc_init, si->crc_init, sizeof(lll->crc_init));
	lll->event_counter = sys_le16_to_cpu(si->evt_cntr);
	lll->phy = phy;
	lll->forced = 0U;

	interval = sys_le16_to_cpu(si->interval);
	interval_us = interval * PERIODIC_INT_UNIT_US;

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_SENDER)
	/* Save Periodic Advertisement Interval */
	sync->interval = interval;
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_SENDER */

	/* Convert fromm 10ms units to interval units */
	sync->timeout_reload = RADIO_SYNC_EVENTS((sync->timeout * 10U *
						  USEC_PER_MSEC), interval_us);

	/* Adjust Skip value so that there is minimum of 6 events that can be
	 * listened to before Sync_Timeout occurs.
	 * The adjustment of the skip value is controller implementation
	 * specific and not specified by the Bluetooth Core Specification v5.3.
	 * The Controller `may` use the Skip value, and the implementation here
	 * covers a case where Skip value could lead to less events being
	 * listened to until Sync_Timeout. Listening to more consecutive events
	 * before Sync_Timeout increases probability of retaining the Periodic
	 * Synchronization.
	 */
	if (sync->timeout_reload > CONN_ESTAB_COUNTDOWN) {
		uint16_t skip_max = sync->timeout_reload - CONN_ESTAB_COUNTDOWN;

		if (sync->skip > skip_max) {
			sync->skip = skip_max;
		}
	} else {
		sync->skip = 0U;
	}

	sync->sync_expire = CONN_ESTAB_COUNTDOWN;

	/* Extract the SCA value from the sca_chm field of the sync_info
	 * structure.
	 */
	sca = (si->sca_chm[PDU_SYNC_INFO_SCA_CHM_SCA_BYTE_OFFSET] &
	       PDU_SYNC_INFO_SCA_CHM_SCA_BIT_MASK) >>
	      PDU_SYNC_INFO_SCA_CHM_SCA_BIT_POS;

#if defined(CONFIG_BT_CTLR_SYNC_ISO)
	lll->sca = sca;
#endif /* CONFIG_BT_CTLR_SYNC_ISO */

	lll->window_widening_periodic_us =
		DIV_ROUND_UP(((lll_clock_ppm_local_get() +
				   lll_clock_ppm_get(sca)) *
				  interval_us), USEC_PER_SEC);
	lll->window_widening_max_us = (interval_us >> 1) - EVENT_IFS_US;
	if (PDU_ADV_SYNC_INFO_OFFS_UNITS_GET(si)) {
		lll->window_size_event_us = OFFS_UNIT_300_US;
	} else {
		lll->window_size_event_us = OFFS_UNIT_30_US;
	}

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
	lll->node_cte_incomplete = NULL;
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

	/* Set the state to sync create */
	scan->periodic.state = LL_SYNC_STATE_CREATED;
	scan->periodic.param = NULL;
	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		struct ll_scan_set *scan_1m;

		scan_1m = ull_scan_set_get(SCAN_HANDLE_1M);
		if (scan == scan_1m) {
			struct ll_scan_set *scan_coded;

			scan_coded = ull_scan_set_get(SCAN_HANDLE_PHY_CODED);
			scan_coded->periodic.state = LL_SYNC_STATE_CREATED;
			scan_coded->periodic.param = NULL;
		} else {
			scan_1m->periodic.state = LL_SYNC_STATE_CREATED;
			scan_1m->periodic.param = NULL;
		}
	}

	sync_handle = ull_sync_handle_get(sync);

	/* Prepare sync notification, dispatch only on successful AUX_SYNC_IND
	 * reception.
	 */
	rx = (void *)sync->node_rx_sync_estab;
	rx->hdr.type = NODE_RX_TYPE_SYNC;
	rx->hdr.handle = sync_handle;
	rx->rx_ftr.param = sync;
	se = (void *)rx->pdu;
	se->interval = interval;
	se->phy = lll->phy;
	se->sca = sca;

	/* Calculate offset and schedule sync radio events */
	ftr = &node_rx->rx_ftr;
	pdu = (void *)((struct node_rx_pdu *)node_rx)->pdu;

	ready_delay_us = lll_radio_rx_ready_delay_get(lll->phy, PHY_FLAGS_S8);

	sync_offset_us = ftr->radio_end_us;
	sync_offset_us += PDU_ADV_SYNC_INFO_OFFSET_GET(si) *
			  lll->window_size_event_us;
	/* offs_adjust may be 1 only if sync setup by LL_PERIODIC_SYNC_IND */
	sync_offset_us += (PDU_ADV_SYNC_INFO_OFFS_ADJUST_GET(si) ? OFFS_ADJUST_US : 0U);
	sync_offset_us -= PDU_AC_US(pdu->len, lll->phy, ftr->phy_flags);
	sync_offset_us -= EVENT_TICKER_RES_MARGIN_US;
	sync_offset_us -= EVENT_JITTER_US;
	sync_offset_us -= ready_delay_us;

	/* Minimum prepare tick offset + minimum preempt tick offset are the
	 * overheads before ULL scheduling can setup radio for reception
	 */
	overhead_us = HAL_TICKER_TICKS_TO_US(HAL_TICKER_CNTR_CMP_OFFSET_MIN << 1);

	/* CPU execution overhead to setup the radio for reception */
	overhead_us += EVENT_OVERHEAD_END_US + EVENT_OVERHEAD_START_US;

	/* If not sufficient CPU processing time, skip to receiving next
	 * event.
	 */
	if ((sync_offset_us - ftr->radio_end_us) < overhead_us) {
		sync_offset_us += interval_us;
		lll->event_counter++;
	}

	interval_us -= lll->window_widening_periodic_us;

	/* Calculate event time reservation */
	slot_us = PDU_AC_MAX_US(PDU_AC_EXT_PAYLOAD_RX_SIZE, lll->phy);
	slot_us += ready_delay_us;

	/* Add implementation defined radio event overheads */
	if (IS_ENABLED(CONFIG_BT_CTLR_EVENT_OVERHEAD_RESERVE_MAX)) {
		slot_us += EVENT_OVERHEAD_START_US + EVENT_OVERHEAD_END_US;
	}

	sync->ull.ticks_slot = HAL_TICKER_US_TO_TICKS_CEIL(slot_us);

	ticks_slot_offset = HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_XTAL_US);
	if (IS_ENABLED(CONFIG_BT_CTLR_LOW_LAT)) {
		ticks_slot_overhead = ticks_slot_offset;
	} else {
		ticks_slot_overhead = 0U;
	}
	ticks_slot_offset += HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);

	sync->lll_sync_prepare = lll_sync_create_prepare;

	ret = ticker_start(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_ULL_HIGH,
			   (TICKER_ID_SCAN_SYNC_BASE + sync_handle),
			   ftr->ticks_anchor - ticks_slot_offset,
			   HAL_TICKER_US_TO_TICKS(sync_offset_us),
			   HAL_TICKER_US_TO_TICKS(interval_us),
			   HAL_TICKER_REMAINDER(interval_us),
			   TICKER_NULL_LAZY,
			   (sync->ull.ticks_slot + ticks_slot_overhead),
			   ticker_cb, sync,
			   ticker_start_op_cb, (void *)__LINE__);
	LL_ASSERT((ret == TICKER_STATUS_SUCCESS) ||
		  (ret == TICKER_STATUS_BUSY));
}

void ull_sync_setup_reset(struct ll_sync_set *sync)
{
	struct ll_scan_set *scan;

	/* Remove the sync context from being associated with scan contexts */
	scan = ull_scan_set_get(SCAN_HANDLE_1M);

	scan->periodic.sync = NULL;

#if defined(CONFIG_BT_CTLR_FILTER_ACCEPT_LIST)
	scan->lll.is_sync = 0U;
#endif /* CONFIG_BT_CTLR_FILTER_ACCEPT_LIST */

	if (IS_ENABLED(CONFIG_BT_CTLR_PHY_CODED)) {
		scan = ull_scan_set_get(SCAN_HANDLE_PHY_CODED);

		scan->periodic.sync = NULL;

#if defined(CONFIG_BT_CTLR_FILTER_ACCEPT_LIST)
		scan->lll.is_sync = 0U;
#endif /* CONFIG_BT_CTLR_FILTER_ACCEPT_LIST */
	}
}

void ull_sync_established_report(memq_link_t *link, struct node_rx_pdu *rx)
{
	struct node_rx_pdu *rx_establ;
	struct ll_sync_set *sync;
	struct node_rx_ftr *ftr;
	struct node_rx_sync *se;
	struct lll_sync *lll;

	ftr = &rx->rx_ftr;
	lll = ftr->param;
	sync = HDR_LLL2ULL(lll);

	/* Do nothing if sync is cancelled or lost. */
	if (unlikely(sync->is_stop || !sync->timeout_reload)) {
		return;
	}

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
	enum sync_status sync_status;

#if defined(CONFIG_BT_CTLR_CTEINLINE_SUPPORT)
	sync_status = ftr->sync_status;
#else
	struct pdu_cte_info *rx_cte_info;

	rx_cte_info = pdu_cte_info_get((struct pdu_adv *)rx->pdu);
	if (rx_cte_info != NULL) {
		sync_status = lll_sync_cte_is_allowed(lll->cte_type, lll->filter_policy,
						      rx_cte_info->time, rx_cte_info->type);
	} else {
		sync_status = lll_sync_cte_is_allowed(lll->cte_type, lll->filter_policy, 0,
						      BT_HCI_LE_NO_CTE);
	}

	/* If there is no CTEInline support, notify done event handler to terminate periodic
	 * advertising sync in case the CTE is not allowed.
	 * If the periodic filtering list is not used then terminate synchronization and notify
	 * host. If the periodic filtering list is used then stop synchronization with this
	 * particular periodic advertised but continue to search for other one.
	 */
	sync->is_term = ((sync_status == SYNC_STAT_TERM) || (sync_status == SYNC_STAT_CONT_SCAN));
#endif /* CONFIG_BT_CTLR_CTEINLINE_SUPPORT */

	/* Send periodic advertisement sync established report when sync has correct CTE type
	 * or the CTE type is incorrect and filter policy doesn't allow to continue scanning.
	 */
	if (sync_status == SYNC_STAT_ALLOWED || sync_status == SYNC_STAT_TERM) {
#else /* !CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

	if (1) {
#endif /* !CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

		/* Prepare and dispatch sync notification */
		rx_establ = (void *)sync->node_rx_sync_estab;
		rx_establ->hdr.handle = ull_sync_handle_get(sync);
		se = (void *)rx_establ->pdu;
		/* Clear the node to mark the sync establish as being completed.
		 * In this case the completion reason is sync being established.
		 */
		sync->node_rx_sync_estab = NULL;

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
		se->status = (ftr->sync_status == SYNC_STAT_TERM) ?
					   BT_HCI_ERR_UNSUPP_REMOTE_FEATURE :
					   BT_HCI_ERR_SUCCESS;
#else
		se->status = BT_HCI_ERR_SUCCESS;
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

		/* NOTE: footer param has already been populated during sync
		 * setup.
		 */

		ll_rx_put_sched(rx_establ->hdr.link, rx_establ);
	}

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
	/* Handle periodic advertising PDU and send periodic advertising scan report when
	 * the sync was found or was established in the past. The report is not send if
	 * scanning is terminated due to wrong CTE type.
	 */
	if (sync_status == SYNC_STAT_ALLOWED || sync_status == SYNC_STAT_READY) {
#else /* !CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

	if (1) {
#endif /* !CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

		/* Switch sync event prepare function to one responsible for regular PDUs receive */
		sync->lll_sync_prepare = lll_sync_prepare;

		/* Change node type to appropriately handle periodic
		 * advertising PDU report.
		 */
		rx->hdr.type = NODE_RX_TYPE_SYNC_REPORT;
		ull_scan_aux_setup(link, rx);
	} else {
		rx->hdr.type = NODE_RX_TYPE_RELEASE;
		ll_rx_put_sched(link, rx);
	}
}

void ull_sync_done(struct node_rx_event_done *done)
{
	struct ll_sync_set *sync;

	/* Get reference to ULL context */
	sync = CONTAINER_OF(done->param, struct ll_sync_set, ull);

	/* Do nothing if local terminate requested or sync lost */
	if (unlikely(sync->is_stop || !sync->timeout_reload)) {
		return;
	}

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
#if defined(CONFIG_BT_CTLR_CTEINLINE_SUPPORT)
	if (done->extra.sync_term) {
#else
	if (sync->is_term) {
#endif /* CONFIG_BT_CTLR_CTEINLINE_SUPPORT */
		/* In case the periodic advertising list filtering is not used the synchronization
		 * must be terminated and host notification must be send.
		 * In case the periodic advertising list filtering is used the synchronization with
		 * this particular periodic advertiser but search for other one from the list.
		 *
		 * Stop periodic advertising sync ticker and clear variables informing the
		 * sync is pending. That is a step to completely terminate the synchronization.
		 * In case search for another periodic advertiser it allows to setup new ticker for
		 * that.
		 */
		sync_ticker_cleanup(sync, NULL);
	} else
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */
	{
		uint32_t ticks_drift_minus;
		uint32_t ticks_drift_plus;
		uint16_t elapsed_event;
		struct lll_sync *lll;
		uint16_t skip_event;
		uint8_t force_lll;
		uint16_t lazy;
		uint8_t force;

		lll = &sync->lll;

		/* Events elapsed used in timeout checks below */
		skip_event = lll->skip_event;

		/* Sync drift compensation and new skip calculation */
		ticks_drift_plus = 0U;
		ticks_drift_minus = 0U;
		if (done->extra.trx_cnt) {
			/* Calculate drift in ticks unit */
			ull_drift_ticks_get(done, &ticks_drift_plus, &ticks_drift_minus);

			/* Enforce skip */
			lll->skip_event = sync->skip;

			/* Reset failed to establish sync countdown */
			sync->sync_expire = 0U;
		}

		elapsed_event = lll->lazy_prepare + 1U;

		/* Reset supervision countdown */
		if (done->extra.crc_valid) {
			sync->timeout_expire = 0U;
		}

		/* check sync failed to establish */
		else if (sync->sync_expire) {
			if (sync->sync_expire > elapsed_event) {
				sync->sync_expire -= elapsed_event;
			} else {
				sync_ticker_cleanup(sync, ticker_stop_sync_expire_op_cb);

				return;
			}
		}

		/* If anchor point not sync-ed, start timeout countdown, and break skip if any */
		else if (!sync->timeout_expire) {
			sync->timeout_expire = sync->timeout_reload;
		}

		/* check timeout */
		force = 0U;
		force_lll = 0U;
		if (sync->timeout_expire) {
			if (sync->timeout_expire > elapsed_event) {
				sync->timeout_expire -= elapsed_event;

				/* break skip */
				lll->skip_event = 0U;

				if (sync->timeout_expire <= 6U) {
					force_lll = 1U;

					force = 1U;
				} else if (skip_event) {
					force = 1U;
				}
			} else {
				sync_ticker_cleanup(sync, ticker_stop_sync_lost_op_cb);

				return;
			}
		}

		lll->forced = force_lll;

		/* Check if skip needs update */
		lazy = 0U;
		if ((force) || (skip_event != lll->skip_event)) {
			lazy = lll->skip_event + 1U;
		}

		/* Update Sync ticker instance */
		if (ticks_drift_plus || ticks_drift_minus || lazy || force) {
			uint16_t sync_handle = ull_sync_handle_get(sync);
			uint32_t ticker_status;

			/* Call to ticker_update can fail under the race
			 * condition where in the periodic sync role is being
			 * stopped but at the same time it is preempted by
			 * periodic sync event that gets into close state.
			 * Accept failure when periodic sync role is being
			 * stopped.
			 */
			ticker_status =
				ticker_update(TICKER_INSTANCE_ID_CTLR,
					      TICKER_USER_ID_ULL_HIGH,
					      (TICKER_ID_SCAN_SYNC_BASE +
					       sync_handle),
					      ticks_drift_plus,
					      ticks_drift_minus, 0, 0,
					      lazy, force,
					      ticker_update_op_cb, sync);
			LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
				  (ticker_status == TICKER_STATUS_BUSY) ||
				  ((void *)sync == ull_disable_mark_get()));
		}
	}
}

void ull_sync_chm_update(uint8_t sync_handle, uint8_t *acad, uint8_t acad_len)
{
	struct pdu_adv_sync_chm_upd_ind *chm_upd_ind;
	struct ll_sync_set *sync;
	struct lll_sync *lll;
	uint8_t chm_last;
	uint16_t ad_len;

	/* Get reference to LLL context */
	sync = ull_sync_set_get(sync_handle);
	LL_ASSERT(sync);
	lll = &sync->lll;

	/* Ignore if already in progress */
	if (lll->chm_last != lll->chm_first) {
		return;
	}

	/* Find the Channel Map Update Indication */
	do {
		/* Pick the length and find the Channel Map Update Indication */
		ad_len = acad[PDU_ADV_DATA_HEADER_LEN_OFFSET];
		if (ad_len &&
		    (acad[PDU_ADV_DATA_HEADER_TYPE_OFFSET] ==
		     PDU_ADV_DATA_TYPE_CHANNEL_MAP_UPDATE_IND)) {
			break;
		}

		/* Add length field size */
		ad_len += 1U;
		if (ad_len < acad_len) {
			acad_len -= ad_len;
		} else {
			return;
		}

		/* Move to next AD data */
		acad += ad_len;
	} while (acad_len);

	/* Validate the size of the Channel Map Update Indication */
	if (ad_len != (sizeof(*chm_upd_ind) + 1U)) {
		return;
	}

	/* Pick the parameters into the procedure context */
	chm_last = lll->chm_last + 1U;
	if (chm_last == DOUBLE_BUFFER_SIZE) {
		chm_last = 0U;
	}

	chm_upd_ind = (void *)&acad[PDU_ADV_DATA_HEADER_DATA_OFFSET];
	(void)memcpy(lll->chm[chm_last].data_chan_map, chm_upd_ind->chm,
		     sizeof(lll->chm[chm_last].data_chan_map));
	lll->chm[chm_last].data_chan_count =
		util_ones_count_get(lll->chm[chm_last].data_chan_map,
				    sizeof(lll->chm[chm_last].data_chan_map));
	if (lll->chm[chm_last].data_chan_count < CHM_USED_COUNT_MIN) {
		/* Ignore channel map, invalid available channel count */
		return;
	}

	lll->chm_instant = sys_le16_to_cpu(chm_upd_ind->instant);

	/* Set Channel Map Update Procedure in progress */
	lll->chm_last = chm_last;
}

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
/* @brief Function updates periodic sync slot duration.
 *
 * @param[in] sync              Pointer to sync instance
 * @param[in] slot_plus_us      Number of microsecond to add to ticker slot
 * @param[in] slot_minus_us     Number of microsecond to subtracks from ticker slot
 *
 * @retval 0            Successful ticker slot update.
 * @retval -ENOENT      Ticker node related with provided sync is already stopped.
 * @retval -ENOMEM      Couldn't enqueue update ticker job.
 * @retval -EFAULT      Somethin else went wrong.
 */
int ull_sync_slot_update(struct ll_sync_set *sync, uint32_t slot_plus_us,
			 uint32_t slot_minus_us)
{
	uint32_t volatile ret_cb;
	uint32_t ret;

	ret_cb = TICKER_STATUS_BUSY;
	ret = ticker_update(TICKER_INSTANCE_ID_CTLR,
			    TICKER_USER_ID_THREAD,
			    (TICKER_ID_SCAN_SYNC_BASE +
			    ull_sync_handle_get(sync)),
			    0, 0,
			    HAL_TICKER_US_TO_TICKS(slot_plus_us),
			    HAL_TICKER_US_TO_TICKS(slot_minus_us),
			    0, 0,
			    ticker_update_op_status_give,
			    (void *)&ret_cb);
	if (ret == TICKER_STATUS_BUSY || ret == TICKER_STATUS_SUCCESS) {
		/* Wait for callback or clear semaphore is callback was already
		 * executed.
		 */
		k_sem_take(&sem_ticker_cb, K_FOREVER);

		if (ret_cb == TICKER_STATUS_FAILURE) {
			return -EFAULT; /* Something went wrong */
		} else {
			return 0;
		}
	} else {
		if (ret_cb != TICKER_STATUS_BUSY) {
			/* Ticker callback was executed and job enqueue was successful.
			 * Call k_sem_take to clear ticker callback semaphore.
			 */
			k_sem_take(&sem_ticker_cb, K_FOREVER);
		}
		/* Ticker was already stopped or job was not enqueued. */
		return (ret_cb == TICKER_STATUS_FAILURE) ? -ENOENT : -ENOMEM;
	}
}
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

static int init_reset(void)
{
	/* Initialize sync pool. */
	mem_init(ll_sync_pool, sizeof(struct ll_sync_set),
		 sizeof(ll_sync_pool) / sizeof(struct ll_sync_set),
		 &sync_free);

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
	k_sem_init(&sem_ticker_cb, 0, 1);
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

	return 0;
}

static inline struct ll_sync_set *sync_acquire(void)
{
	return mem_acquire(&sync_free);
}

static struct ll_sync_set *ull_sync_create(uint8_t sid, uint16_t timeout, uint16_t skip,
					   uint8_t cte_type, uint8_t rx_enable, uint8_t nodups)
{
	memq_link_t *link_sync_estab;
	memq_link_t *link_sync_lost;
	struct node_rx_pdu *node_rx;
	struct lll_sync *lll;
	struct ll_sync_set *sync;

	link_sync_estab = ll_rx_link_alloc();
	if (!link_sync_estab) {
		return NULL;
	}

	link_sync_lost = ll_rx_link_alloc();
	if (!link_sync_lost) {
		ll_rx_link_release(link_sync_estab);

		return NULL;
	}

	node_rx = ll_rx_alloc();
	if (!node_rx) {
		ll_rx_link_release(link_sync_lost);
		ll_rx_link_release(link_sync_estab);

		return NULL;
	}

	sync = sync_acquire();
	if (!sync) {
		ll_rx_release(node_rx);
		ll_rx_link_release(link_sync_lost);
		ll_rx_link_release(link_sync_estab);

		return NULL;
	}

	sync->peer_addr_resolved = 0U;

	/* Initialize sync context */
	node_rx->hdr.link = link_sync_estab;
	sync->node_rx_lost.rx.hdr.link = link_sync_lost;

	/* Make sure that the node_rx_sync_establ hasn't got anything assigned. It is used to
	 * mark when sync establishment is in progress.
	 */
	LL_ASSERT(!sync->node_rx_sync_estab);
	sync->node_rx_sync_estab = node_rx;

	/* Reporting initially enabled/disabled */
	sync->rx_enable = rx_enable;

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_ADI_SUPPORT)
	sync->nodups = nodups;
#endif
	sync->skip = skip;
	sync->is_stop = 0U;

#if defined(CONFIG_BT_CTLR_SYNC_ISO)
	sync->enc = 0U;
#endif /* CONFIG_BT_CTLR_SYNC_ISO */

	/* NOTE: Use timeout not zero to represent sync context used for sync
	 * create.
	 */
	sync->timeout = timeout;

	/* NOTE: Use timeout_reload not zero to represent sync established. */
	sync->timeout_reload = 0U;
	sync->timeout_expire = 0U;

	/* Remember the SID */
	sync->sid = sid;

#if defined(CONFIG_BT_CTLR_SYNC_ISO)
	/* Reset Broadcast Isochronous Group Sync Establishment */
	sync->iso.sync_iso = NULL;
#endif /* CONFIG_BT_CTLR_SYNC_ISO */

	/* Initialize sync LLL context */
	lll = &sync->lll;
	lll->lll_aux = NULL;
	lll->is_rx_enabled = sync->rx_enable;
	lll->skip_prepare = 0U;
	lll->skip_event = 0U;
	lll->window_widening_prepare_us = 0U;
	lll->window_widening_event_us = 0U;
#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
	lll->cte_type = cte_type;
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
	ull_df_sync_cfg_init(&lll->df_cfg);
	LL_ASSERT(!lll->node_cte_incomplete);
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

	/* Initialise ULL and LLL headers */
	ull_hdr_init(&sync->ull);
	lll_hdr_init(lll, sync);

	return sync;
}

static void sync_ticker_cleanup(struct ll_sync_set *sync, ticker_op_func stop_op_cb)
{
	uint16_t sync_handle = ull_sync_handle_get(sync);
	uint32_t ret;

	/* Stop Periodic Sync Ticker */
	ret = ticker_stop(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_ULL_HIGH,
			  TICKER_ID_SCAN_SYNC_BASE + sync_handle, stop_op_cb, (void *)sync);
	LL_ASSERT((ret == TICKER_STATUS_SUCCESS) ||
		  (ret == TICKER_STATUS_BUSY));

	/* Mark sync context not sync established */
	sync->timeout_reload = 0U;
}

static void ticker_cb(uint32_t ticks_at_expire, uint32_t ticks_drift,
		      uint32_t remainder, uint16_t lazy, uint8_t force,
		      void *param)
{
	static memq_link_t link_lll_prepare;
	static struct mayfly mfy_lll_prepare = {
		0, 0, &link_lll_prepare, NULL, NULL};
	static struct lll_prepare_param p;
	struct ll_sync_set *sync = param;
	struct lll_sync *lll;
	uint32_t ret;
	uint8_t ref;

	DEBUG_RADIO_PREPARE_O(1);

	lll = &sync->lll;

	/* Commit receive enable changed value */
	lll->is_rx_enabled = sync->rx_enable;

	/* Increment prepare reference count */
	ref = ull_ref_inc(&sync->ull);
	LL_ASSERT(ref);

	/* Append timing parameters */
	p.ticks_at_expire = ticks_at_expire;
	p.remainder = remainder;
	p.lazy = lazy;
	p.force = force;
	p.param = lll;
	mfy_lll_prepare.param = &p;
	mfy_lll_prepare.fp = sync->lll_sync_prepare;

	/* Kick LLL prepare */
	ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH, TICKER_USER_ID_LLL, 0,
			     &mfy_lll_prepare);
	LL_ASSERT(!ret);

	DEBUG_RADIO_PREPARE_O(1);
}

static void ticker_start_op_cb(uint32_t status, void *param)
{
	ARG_UNUSED(param);
	LL_ASSERT(status == TICKER_STATUS_SUCCESS);
}

static void ticker_update_op_cb(uint32_t status, void *param)
{
	LL_ASSERT(status == TICKER_STATUS_SUCCESS ||
		  param == ull_disable_mark_get());
}

static void ticker_stop_sync_expire_op_cb(uint32_t status, void *param)
{
	uint32_t retval;
	static memq_link_t link;
	static struct mayfly mfy = {0, 0, &link, NULL, sync_expire};

	LL_ASSERT(status == TICKER_STATUS_SUCCESS);

	mfy.param = param;

	retval = mayfly_enqueue(TICKER_USER_ID_ULL_LOW, TICKER_USER_ID_ULL_HIGH,
				0, &mfy);
	LL_ASSERT(!retval);
}

static void sync_expire(void *param)
{
	struct ll_sync_set *sync = param;
	struct node_rx_sync *se;
	struct node_rx_pdu *rx;

	/* Generate Periodic advertising sync failed to establish */
	rx = (void *)sync->node_rx_sync_estab;
	rx->hdr.handle = LLL_HANDLE_INVALID;

	/* Clear the node to mark the sync establish as being completed.
	 * In this case the completion reason is sync expire.
	 */
	sync->node_rx_sync_estab = NULL;

	/* NOTE: struct node_rx_sync_estab has uint8_t member following the
	 *       struct node_rx_hdr to store the reason.
	 */
	se = (void *)rx->pdu;
	se->status = BT_HCI_ERR_CONN_FAIL_TO_ESTAB;

	/* NOTE: footer param has already been populated during sync setup */

	/* Enqueue the sync failed to established towards ULL context */
	ll_rx_put_sched(rx->hdr.link, rx);
}

static void ticker_stop_sync_lost_op_cb(uint32_t status, void *param)
{
	uint32_t retval;
	static memq_link_t link;
	static struct mayfly mfy = {0, 0, &link, NULL, sync_lost};

	/* When in race between terminate requested in thread context and
	 * sync lost scenario, do not generate the sync lost node rx from here
	 */
	if (status != TICKER_STATUS_SUCCESS) {
		LL_ASSERT(param == ull_disable_mark_get());

		return;
	}

	mfy.param = param;

	retval = mayfly_enqueue(TICKER_USER_ID_ULL_LOW, TICKER_USER_ID_ULL_HIGH,
				0, &mfy);
	LL_ASSERT(!retval);
}

static void sync_lost(void *param)
{
	struct ll_sync_set *sync;
	struct node_rx_pdu *rx;

	/* sync established was not generated yet, no free node rx */
	sync = param;
	if (sync->lll_sync_prepare != lll_sync_prepare) {
		sync_expire(param);

		return;
	}

	/* Generate Periodic advertising sync lost */
	rx = (void *)&sync->node_rx_lost;
	rx->hdr.handle = ull_sync_handle_get(sync);
	rx->hdr.type = NODE_RX_TYPE_SYNC_LOST;
	rx->rx_ftr.param = sync;

	/* Enqueue the sync lost towards ULL context */
	ll_rx_put_sched(rx->hdr.link, rx);

#if defined(CONFIG_BT_CTLR_SYNC_ISO)
	if (sync->iso.sync_iso) {
		/* ISO create BIG flag in the periodic advertising context is still set */
		struct ll_sync_iso_set *sync_iso;

		sync_iso = sync->iso.sync_iso;

		rx = (void *)&sync_iso->node_rx_lost;
		rx->hdr.handle = sync_iso->big_handle;
		rx->hdr.type = NODE_RX_TYPE_SYNC_ISO;
		rx->rx_ftr.param = sync_iso;
		*((uint8_t *)rx->pdu) = BT_HCI_ERR_CONN_FAIL_TO_ESTAB;

		/* Enqueue the sync iso lost towards ULL context */
		ll_rx_put_sched(rx->hdr.link, rx);
	}
#endif /* CONFIG_BT_CTLR_SYNC_ISO */
}

#if defined(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)
static struct ll_sync_set *sync_is_create_get(uint16_t handle)
{
	struct ll_sync_set *sync;

	sync = ull_sync_set_get(handle);
	if (!sync || !sync->timeout) {
		return NULL;
	}

	return sync;
}

static bool peer_sid_sync_exists(uint8_t const peer_id_addr_type,
				 uint8_t const *const peer_id_addr,
				 uint8_t sid)
{
	uint16_t handle;

	for (handle = 0U; handle < CONFIG_BT_PER_ADV_SYNC_MAX; handle++) {
		struct ll_sync_set *sync = sync_is_create_get(handle);

		if (sync &&
		    (sync->peer_id_addr_type == peer_id_addr_type) &&
		    !memcmp(sync->peer_id_addr, peer_id_addr, BDADDR_SIZE) &&
		    (sync->sid == sid)) {
			return true;
		}
	}

	return false;
}
#endif /* CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC */

#if defined(CONFIG_BT_CTLR_DF_SCAN_CTE_RX)
static void ticker_update_op_status_give(uint32_t status, void *param)
{
	*((uint32_t volatile *)param) = status;

	k_sem_give(&sem_ticker_cb);
}
#endif /* CONFIG_BT_CTLR_DF_SCAN_CTE_RX */

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING) && \
	!defined(CONFIG_BT_CTLR_CTEINLINE_SUPPORT)
static struct pdu_cte_info *pdu_cte_info_get(struct pdu_adv *pdu)
{
	struct pdu_adv_com_ext_adv *com_hdr;
	struct pdu_adv_ext_hdr *hdr;

	com_hdr = &pdu->adv_ext_ind;
	hdr = &com_hdr->ext_hdr;

	if (!com_hdr->ext_hdr_len || (com_hdr->ext_hdr_len != 0 && !hdr->cte_info)) {
		return NULL;
	}

	/* Make sure there are no fields that are not allowed for AUX_SYNC_IND and AUX_CHAIN_IND */
	LL_ASSERT(!hdr->adv_addr);
	LL_ASSERT(!hdr->tgt_addr);

	return (struct pdu_cte_info *)hdr->data;
}
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING && !CONFIG_BT_CTLR_CTEINLINE_SUPPORT */

#if defined(CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER)
void ull_sync_transfer_received(struct ll_conn *conn, uint16_t service_data,
				struct pdu_adv_sync_info *si, uint16_t conn_event_count,
				uint16_t last_pa_event_counter, uint8_t sid,
				uint8_t addr_type, uint8_t sca, uint8_t phy,
				uint8_t *adv_addr, uint16_t sync_conn_event_count,
				uint8_t addr_resolved)
{
	struct ll_sync_set *sync;
	uint16_t conn_evt_current;
	uint8_t rx_enable;
	uint8_t nodups;

	if (conn->past.mode == BT_HCI_LE_PAST_MODE_NO_SYNC) {
		/* Ignore LL_PERIODIC_SYNC_IND - see Bluetooth Core Specification v5.4
		 * Vol 6, Part E, Section 7.8.91
		 */
		return;
	}

#if defined(CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC)
	/* Do not sync twice to the same peer and same SID */
	if (peer_sid_sync_exists(addr_type, adv_addr, sid)) {
		return;
	}
#endif /* CONFIG_BT_CTLR_CHECK_SAME_PEER_SYNC */

	nodups = (conn->past.mode == BT_HCI_LE_PAST_MODE_SYNC_FILTER_DUPLICATES) ? 1U : 0U;
	rx_enable = (conn->past.mode == BT_HCI_LE_PAST_MODE_NO_REPORTS) ? 0U : 1U;

	sync = ull_sync_create(sid, conn->past.timeout, conn->past.skip, conn->past.cte_type,
			       rx_enable, nodups);
	if (!sync) {
		return;
	}

#if defined(CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING)
	/* Reset filter policy in lll_sync */
	sync->lll.filter_policy = 0U;
#endif /* CONFIG_BT_CTLR_SYNC_PERIODIC_CTE_TYPE_FILTERING */

	sync->peer_id_addr_type = addr_type;
	sync->peer_addr_resolved = addr_resolved;
	memcpy(sync->peer_id_addr, adv_addr, BDADDR_SIZE);
	sync->lll.phy = phy;

	conn_evt_current = ull_conn_event_counter(conn);

	/* LLCP should have ensured this holds */
	LL_ASSERT(sync_conn_event_count != conn_evt_current);

	ull_sync_setup_from_sync_transfer(conn, service_data, sync, si,
					  conn_event_count - conn_evt_current,
					  last_pa_event_counter, sync_conn_event_count,
					  sca);
}
#endif /* CONFIG_BT_CTLR_SYNC_TRANSFER_RECEIVER */
