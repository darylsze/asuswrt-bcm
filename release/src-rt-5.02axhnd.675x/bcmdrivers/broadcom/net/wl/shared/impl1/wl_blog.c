/*
    Copyright (c) 2017 Broadcom
    All Rights Reserved

    <:label-BRCM:2017:DUAL/GPL:standard
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2, as published by
    the Free Software Foundation (the "GPL").
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    
    A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
    writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
    Boston, MA 02111-1307, USA.
    
    :>
*/

#if defined(BCM_BLOG)

#include <linux/bcm_skb_defines.h>

#include <wlc_cfg.h>
#include <wlc_channel.h>

#include <wlioctl.h>
#include <wl_dbg.h>

#include <wlc_pub.h>
#include <wl_linux.h>
#include <wl_pktc.h>

#include <wl_blog.h>
#if defined(BCM_PKTFWD)
#include <wlc_scb.h>
#endif

struct sk_buff *wl_xlate_to_skb(struct wl_info *wl, struct sk_buff *s)
{
	struct sk_buff *orig_s, *xlated_s;

	if (IS_SKBUFF_PTR(s)) {
		/* reset skb->cb field as initializing WLPKTTAG for all incoming skbs */
		memset(&s->cb[0], 0, sizeof(s->cb));
#ifdef DSLCPE_CACHE_SMARTFLUSH
		PKTSETDIRTYP(wl->osh, s, NULL);
#endif
#if defined(BCM_PKTFWD)
		wl_pktfwd_stats_gp->txf_fkb_pkts++;
#elif defined(PKTC_TBL)
		if (wl->pub->pktc_tbl && WLPKTCTBL(wl->pub->pktc_tbl)->g_stats)
			WLCNTINCR(WLPKTCTBL(wl->pub->pktc_tbl)->g_stats->tx_slowpath_skb);
#endif
		return s;
	}
#if defined(BCM_PKTFWD)
	wl_pktfwd_stats_gp->txf_fkb_pkts++;
#elif defined(PKTC_TBL)
	if (wl->pub->pktc_tbl && WLPKTCTBL(wl->pub->pktc_tbl)->g_stats)
		WLCNTINCR(WLPKTCTBL(wl->pub->pktc_tbl)->g_stats->tx_slowpath_fkb);
#endif
	orig_s = s;
	xlated_s = nbuff_xlate((pNBuff_t)s);
	if (xlated_s == NULL) {
		nbuff_free((pNBuff_t) orig_s);
		return NULL;
	}
	return xlated_s;
}

#if defined(BCM_WFD) && defined(CONFIG_BCM_FC_BASED_WFD)
typedef int (*FC_WFD_ENQUEUE_HOOK)(void * nbuff_p,const Blog_t * const blog_p); /* Xmit with blog */
extern FC_WFD_ENQUEUE_HOOK fc_wfd_enqueue_cb;
static int wl_fc_wfd_enqueue(void * nbuff_p,const Blog_t * const blog_p)
{
    if(fc_wfd_enqueue_cb)
        return (fc_wfd_enqueue_cb)(nbuff_p,blog_p);
         
    return 0;
}
#endif

int wl_handle_blog_emit(struct wl_info *wl, struct wl_if *wlif, struct sk_buff *skb,
	struct net_device *dev)
{
	/* Fix the priority if WME is enabled */
	if (WME_ENAB(wl->pub) && (PKTPRIO(skb) == 0))
		pktsetprio(skb, FALSE);

	if (wl->pub->fcache && (skb->blog_p != NULL)) {
		uint8_t prio4bit = 0;
		uint32_t hw_port;

#if defined(BCM_WFD)
		struct ether_header *eh = (struct ether_header*) PKTDATA(wl->osh, skb);
#endif

		ENCODE_WLAN_PRIORITY_MARK(prio4bit, skb->mark);
		skb->blog_p->wfd.nic_ucast.priority = (prio4bit & 0x0f);

#if defined(DSLCPE_PLATFORM_WITH_RUNNER) && defined(BCM_WFD)
		if (ETHER_ISMULTI(eh->ether_dhost)) {
			skb->blog_p->wfd.mcast.is_tx_hw_acc_en = 1;
			skb->blog_p->wfd.mcast.is_wfd = 1;
			skb->blog_p->wfd.mcast.is_chain = 0;
			skb->blog_p->wfd.mcast.wfd_idx = wl->wfd_idx;
			skb->blog_p->wfd.mcast.wfd_prio = 0 ; /* put mcast in high prio queue */
			skb->blog_p->wfd.mcast.ssid = wlif->subunit;
		}
#endif

#if defined(BCM_WFD)
		if (!ETHER_ISMULTI(eh->ether_dhost))
		{
			skb->blog_p->wfd.nic_ucast.is_wfd = 1;
#if defined(BCM_WFD) && defined(CONFIG_BCM_FC_BASED_WFD)
			if(skb->blog_p->wfd.nic_ucast.is_chain)
			{
				skb->blog_p->dev_xmit_blog = wl_fc_wfd_enqueue;
			}
#else
			skb->blog_p->dev_xmit_blog = NULL;
#if defined(CONFIG_BCM_OVS)
#if defined(PKTC_TBL)
			{
			uint32_t chainIdx = PKTC_INVALID_CHAIN_IDX;
			chainIdx = wl_pktc_req(PKTC_TBL_UPDATE, (unsigned long)eh->ether_dhost, (unsigned long)skb->dev, 0);
			if (chainIdx != PKTC_INVALID_CHAIN_IDX) {
				skb->blog_p->wfd.nic_ucast.is_tx_hw_acc_en = 1;
				skb->blog_p->wfd.nic_ucast.is_chain = 1;
				skb->blog_p->wfd.nic_ucast.wfd_idx = ((chainIdx & PKTC_WFD_IDX_BITMASK) >> PKTC_WFD_IDX_BITPOS);
				skb->blog_p->wfd.nic_ucast.chain_idx = chainIdx;
			}
			}
#endif /* PKTC_TBL */
#endif /* CONFIG_BCM_OVS */
#endif /* BCM_WFD && CONFIG_BCM_FC_BASED_WFD */
		}
#endif
		hw_port = netdev_path_get_hw_port((struct net_device *)(skb->dev));
		blog_emit(skb, dev, TYPE_ETH, hw_port, BLOG_WLANPHY);
	}
	skb->dev = dev;

	return 0;
}

int wl_handle_blog_sinit(struct wl_info *wl, struct sk_buff *skb)
{
	/* If Linux TCP/IP stack is bypassed by injecting this packet directly to fastlath,
	 * then avoid software FC - it will probably be slower than fastpath.
	 */
	if (wl->pub->fcache) {
		BlogAction_t blog_ret;
		uint32_t hw_port;

		/* Clear skb->mark (WLAN internal for priority) field as it will be used as skb->fc_ctxt in blog for ingress */
		skb->mark = 0;

		hw_port = netdev_path_get_hw_port((struct net_device *)(skb->dev));
		blog_ret = blog_sinit(skb, skb->dev, TYPE_ETH, hw_port, BLOG_WLANPHY);
		if (PKT_DONE == blog_ret) {
			/* Doesnot need go to IP stack */
			return 0;
		} else if (PKT_BLOG == blog_ret) {
#if defined(CONFIG_BCM_PON)
			/* PON Platforms support WLAN_RX_ACCELERATION through loopback model */
			skb->blog_p->rnr.is_rx_hw_acc_en = 1;
#elif defined(BCM_AWL)
			/* Archer platforms support acceleration through Archer driver */
			skb->blog_p->wl_hw_support.is_rx_hw_acc_en = 1;
#else
			/* For NIC mode -- RX is always on host, so HW can't accelerate */
			skb->blog_p->rnr.is_rx_hw_acc_en = 0;
#endif
		}
	}

	return -1;
}

void wl_handle_blog_event(wl_info_t *wl, wlc_event_t *e)
{
	struct net_device *dev;
	BlogFlushParams_t params = {};

	if (!(e->addr) || (e->event.status != WLC_E_STATUS_SUCCESS))
		return;

    dev = dev_get_by_name(&init_net, e->event.ifname);
    if (dev == NULL) {
        WL_ERROR(("wl%d: wl_handle_blog_event - Invalid interface\n",
                 wl->pub->unit));
        return;
    }

	switch (e->event.event_type) {
		case WLC_E_DEAUTH:
		case WLC_E_DEAUTH_IND:
		case WLC_E_DISASSOC:
		case WLC_E_DISASSOC_IND:
			WL_ERROR(("wl%d: notify system/blog disconnection event.\n",
				wl->pub->unit));
			/* also destroy the fcache flow */
			params.flush_dstmac = 1;
			params.flush_srcmac = 1;
			memcpy(&params.mac[0], &e->event.addr.octet[0], sizeof(e->event.addr.octet));
			blog_notify_async_wait(FLUSH, dev, (unsigned long)&params, 0);

#if defined(PKTC_TBL)
			/* mark as STA disassoc */
			WL_ERROR(("%s: mark as DIS-associated. addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
				__FUNCTION__,
				e->event.addr.octet[0], e->event.addr.octet[1],
				e->event.addr.octet[2], e->event.addr.octet[3],
				e->event.addr.octet[4], e->event.addr.octet[5]));

            wl_pktc_req(PKTC_TBL_SET_STA_ASSOC, (unsigned long)e->event.addr.octet,
                        0, e->event.event_type);

            if (wl_pktc_del_hook != NULL) {
                wl_pktc_del_hook((unsigned long)e->event.addr.octet, dev);
            }
#endif /* PKTC_TBL */
			break;

		case WLC_E_ASSOC:
		case WLC_E_ASSOC_IND:
		case WLC_E_REASSOC_IND:
			WL_ERROR(("wl%d: notify system/blog association event.\n",
				wl->pub->unit));
                        /* BCM_PKTFWD: flowid and incarn will be updated for this STA
                         * Destroy stale entries in fcache
                         */
			params.flush_dstmac = 1;
			params.flush_srcmac = 1;
			memcpy(&params.mac[0], &e->event.addr.octet[0], sizeof(e->event.addr.octet));
			blog_notify_async_wait(FLUSH, dev, (unsigned long)&params, 0);
#if defined(PKTC_TBL)
			/* mark as STA assoc */
			WL_ERROR(("%s: mark as associated. addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
				__FUNCTION__,
				e->event.addr.octet[0], e->event.addr.octet[1],
				e->event.addr.octet[2], e->event.addr.octet[3],
				e->event.addr.octet[4], e->event.addr.octet[5]));
#if defined(BCM_PKTFWD)
			/* setting dwds client properly before d3lut_elem insertion */
			{
				wl_if_t *wlif = WL_DEV_IF(dev);
				struct scb *scb = wlc_scbfind_from_wlcif(wl->wlc, wlif->wlcif, e->event.addr.octet);
				wlc_bsscfg_t *bsscfg = wl_bsscfg_find(wlif);
				netdev_wlan_unset_dwds_client(wlif->d3fwd_wlif); /* reset first */
				if (BSSCFG_STA(bsscfg) && scb && SCB_DWDS(scb)) {
					netdev_wlan_set_dwds_client(wlif->d3fwd_wlif);
				}
			}
#endif /* BCM_PKTFWD */
			wl_pktc_req(PKTC_TBL_SET_STA_ASSOC, (unsigned long)e->event.addr.octet,
				1, e->event.event_type);
			wl_pktc_req(PKTC_TBL_UPDATE, (unsigned long)e->event.addr.octet,
				(unsigned long)dev, 0);

#endif /* PKTC_TBL */
			break;

		default:
			break;
	}
    /* Release reference to device */
    dev_put(dev);
}

#endif /* BCM_BLOG */
