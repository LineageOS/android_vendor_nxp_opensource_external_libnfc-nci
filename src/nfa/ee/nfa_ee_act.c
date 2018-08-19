/******************************************************************************
 *  Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *  Not a Contribution.
 *
 *  Copyright (C) 2015-2018 NXP Semiconductors
 *  The original Work has been changed by NXP Semiconductors.
 *
 *  Copyright (C) 2010-2014 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the action functions for NFA-EE
 *
 ******************************************************************************/
#include <string.h>
#include "nfa_sys.h"
#include "nfa_api.h"
#include "nfa_dm_int.h"
#include "nfa_sys_int.h"
#include "nfc_api.h"
#include "nfa_ee_int.h"
#include "nci_hmsgs.h"
#if (NXP_EXTNS == TRUE)
#include "nfa_hci_int.h"
#endif
#include <config.h>

/* the de-bounce timer:
 * The NFA-EE API functions are called to set the routing and VS configuration.
 * When this timer expires, the configuration is sent to NFCC all at once.
 * This is the timeout value for the de-bounce timer. */
#ifndef NFA_EE_ROUT_TIMEOUT_VAL
#define NFA_EE_ROUT_TIMEOUT_VAL 1000
#endif

/* the following 2 tables convert the technology mask in API and control block
 * to the command for NFCC */
const uint8_t nfa_ee_tech_mask_list[NFA_EE_NUM_TECH] = {
    NFA_TECHNOLOGY_MASK_A, NFA_TECHNOLOGY_MASK_B, NFA_TECHNOLOGY_MASK_F};

const uint8_t nfa_ee_tech_list[NFA_EE_NUM_TECH] = {
    NFC_RF_TECHNOLOGY_A, NFC_RF_TECHNOLOGY_B, NFC_RF_TECHNOLOGY_F};

/* the following 2 tables convert the protocol mask in API and control block to
 * the command for NFCC */
static void add_route_tech_proto_tlv(uint8_t** pp, uint8_t tlv_type,
                                     uint8_t nfcee_id, uint8_t pwr_cfg,
                                     uint8_t tech_proto) {
  *(*pp)++ = tlv_type;
  *(*pp)++ = 3;
  *(*pp)++ = nfcee_id;
  *(*pp)++ = pwr_cfg;
  *(*pp)++ = tech_proto;
}

static void add_route_aid_tlv(uint8_t** pp, uint8_t* pa, uint8_t nfcee_id,
                              uint8_t pwr_cfg, uint8_t tag) {
  pa++;                /* EMV tag */
  uint8_t len = *pa++; /* aid_len */
  *(*pp)++ = tag;
  *(*pp)++ = len + 2;
  *(*pp)++ = nfcee_id;
  *(*pp)++ = pwr_cfg;
  /* copy the AID */
  memcpy(*pp, pa, len);
  *pp += len;
}

const uint8_t nfa_ee_proto_mask_list[NFA_EE_NUM_PROTO] = {
    NFA_PROTOCOL_MASK_T1T, NFA_PROTOCOL_MASK_T2T, NFA_PROTOCOL_MASK_T3T,
    NFA_PROTOCOL_MASK_ISO_DEP, NFA_PROTOCOL_MASK_NFC_DEP
#if (NXP_EXTNS == TRUE)
    ,
    NFC_PROTOCOL_MASK_ISO7816
#endif
};

const uint8_t nfa_ee_proto_list[NFA_EE_NUM_PROTO] = {
    NFC_PROTOCOL_T1T, NFC_PROTOCOL_T2T, NFC_PROTOCOL_T3T, NFC_PROTOCOL_ISO_DEP,
    NFC_PROTOCOL_NFC_DEP
#if (NXP_EXTNS == TRUE)
    ,
    NFC_PROTOCOL_ISO7816
#endif
};

#if (NXP_EXTNS == TRUE)
static uint8_t* proto_route_buff;
static uint8_t* tech_route_buff;
static uint8_t* proto_pp;
static uint8_t* tech_pp;
static uint8_t proto_tlv_ctr;
static uint8_t tech_tlv_ctr;
/* Ex: num of proto= 6, proto_mask =5 and num of EEs 4, total  size = (6x5x4) */
#define NFA_EE_PROTO_BUFF_SIZE \
  (NFA_EE_NUM_PROTO * NFA_EE_PROTO_ROUTE_ENTRY_SIZE * NFA_EE_MAX_EE_SUPPORTED)
/* Ex: num of tech =3 , tech_mask =5 and num of EEs 4, total  size = (3x5x4) */
#define NFA_EE_TECH_BUFF_SIZE \
  (NFA_EE_NUM_TECH * NFA_EE_TECH_ROUTE_ENTRY_SIZE * NFA_EE_MAX_EE_SUPPORTED)
#endif /* - Routing entries optimization */

#if (NXP_EXTNS == TRUE)
uint8_t NFA_REMOVE_ALL_AID[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t NFA_REMOVE_ALL_APDU[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF};
uint8_t nfa_ee_ce_route_strict_disable = 0x01;
bool gNfaProvisionMode =
    false; /* to know if device is set to provision mode or not */
uint8_t nfa_ee_ce_p61_active = 0x00;
static uint16_t max_routing_table_size;
static uint16_t max_aid_config_length;
static uint16_t max_aid_entries;
#endif

static void nfa_ee_report_discover_req_evt(void);
static void nfa_ee_build_discover_req_evt(tNFA_EE_DISCOVER_REQ* p_evt_data);
void nfa_ee_check_set_routing(uint16_t new_size, int* p_max_len, uint8_t* p,
                              int* p_cur_offset);
/*******************************************************************************
**
** Function         nfa_ee_trace_aid
**
** Description      trace AID
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_trace_aid(char* p_str, uint8_t id, uint8_t aid_len,
                             uint8_t* p) {
  int len = aid_len;
  int xx, yy = 0;
  char buff[100];

  buff[0] = 0;
  if (aid_len > NFA_MAX_AID_LEN) {
    NFA_TRACE_ERROR2("aid_len: %d exceeds max(%d)", aid_len, NFA_MAX_AID_LEN);
    len = NFA_MAX_AID_LEN;
  }
  for (xx = 0; xx < len; xx++) {
    yy += snprintf(&buff[yy], 100, "%02x ", *p);
    p++;
  }
  NFA_TRACE_DEBUG4("%s id:0x%x len=%d aid:%s", p_str, id, aid_len, buff);
}

/*******************************************************************************
**
** Function         nfa_ee_trace_apdu
**
** Description      trace AID
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_trace_apdu(char* p_str, tNFA_EE_API_ADD_APDU* p_apdu) {
  int apdu_len = p_apdu->apdu_len;
  int mask_len = p_apdu->mask_len;
  int xx, yy = 0;
  char apdu[125];
  char mask[125];
  apdu[0] = 0;
  mask[0] = 0;
  uint8_t* p;
  if (apdu_len > NFA_MAX_APDU_DATA_LEN) {
    NFA_TRACE_ERROR2("apdu_len: %d exceeds max(%d)", apdu_len, NFA_MAX_APDU_DATA_LEN);
    apdu_len = NFA_MAX_APDU_DATA_LEN;
    p_apdu->apdu_len = apdu_len;
  }

  if (mask_len > NFA_MAX_APDU_MASK_LEN) {
    NFA_TRACE_ERROR2("mask_len: %d exceeds max(%d)", mask_len, NFA_MAX_APDU_MASK_LEN);
    mask_len = NFA_MAX_APDU_MASK_LEN;
    p_apdu->mask_len = mask_len;
  }

  p =  p_apdu->p_apdu;
  for (xx = 0; xx < apdu_len; xx++) {
    yy += snprintf(&apdu[yy], 125, "%02x ", *p);
    p++;
  }

  p =  p_apdu->p_mask;
  yy = 0;
  for (xx = 0; xx < mask_len; xx++) {
    yy += snprintf(&mask[yy], 125, "%02x ", *p);
    p++;
  }
  NFA_TRACE_DEBUG6("%s id:0x%x apdu_len=%d apdu:%s mask_len=%d mask:%s", p_str, p_apdu->nfcee_id, apdu_len, apdu,mask_len,mask);
}
/*******************************************************************************
**
** Function         nfa_ee_update_route_size
**
** Description      Update the size required for technology and protocol routing
**                  of the given NFCEE ID.
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_update_route_size(tNFA_EE_ECB* p_cb) {
  int xx;
  uint8_t power_cfg = 0;

  p_cb->size_mask = 0;
  /* add the Technology based routing */
  for (xx = 0; xx < NFA_EE_NUM_TECH; xx++) {
    power_cfg = 0;
    if (p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
    if (power_cfg) {
      /* 5 = 1 (tag) + 1 (len) + 1(nfcee_id) + 1(power cfg) + 1 (techonogy) */
      p_cb->size_mask += 5;
    }
  }

  /* add the Protocol based routing */
  for (xx = 0; xx < NFA_EE_NUM_PROTO; xx++) {
    power_cfg = 0;
    if (p_cb->proto_switch_on & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->proto_switch_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->proto_battery_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
    if (power_cfg) {
      /* 5 = 1 (tag) + 1 (len) + 1(nfcee_id) + 1(power cfg) + 1 (protocol) */
      p_cb->size_mask += 5;
    }
  }
  NFA_TRACE_DEBUG2("nfa_ee_update_route_size nfcee_id:0x%x size_mask:%d",
                   p_cb->nfcee_id, p_cb->size_mask);
}

/*******************************************************************************
**
** Function         nfa_ee_update_route_apdu_size
**
** Description      Update the size required for APDU routing
**                  of the given NFCEE ID.
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_update_route_apdu_size(tNFA_EE_ECB* p_cb) {
  uint8_t* pa, len;
  int start_offset;
  int xx;

  p_cb->size_apdu = 0;
  if (p_cb->apdu_pattern_entries) {
    start_offset = 0;
    for (xx = 0; xx < p_cb->apdu_pattern_entries; xx++) {
      /* add one APDU entry */
      if (p_cb->apdu_rt_info[xx] & NFA_EE_AE_ROUTE) {
        pa = &p_cb->apdu_cfg[start_offset];
        pa++;        /* EMV tag */
        len = *pa++; /* apdu_len */
        /* 4 = 1 (tag) + 1 (len) + 1(nfcee_id) + 1(power cfg) */
        p_cb->size_apdu += 4;
        p_cb->size_apdu += len;
      }
      start_offset += p_cb->apdu_len[xx];
    }
  }
  NFA_TRACE_DEBUG2("nfa_ee_update_route_apdu_size nfcee_id:0x%x size_apdu:%d",
                   p_cb->nfcee_id, p_cb->size_apdu);
}

/*******************************************************************************
**
** Function         nfa_ee_update_route_aid_size
**
** Description      Update the size required for AID routing
**                  of the given NFCEE ID.
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_update_route_aid_size(tNFA_EE_ECB* p_cb) {
  uint8_t* pa, len;
  int start_offset;
  int xx;

  p_cb->size_aid = 0;
  if (p_cb->aid_entries) {
    start_offset = 0;
    for (xx = 0; xx < p_cb->aid_entries; xx++) {
      /* add one AID entry */
      if (p_cb->aid_rt_info[xx] & NFA_EE_AE_ROUTE) {
        pa = &p_cb->aid_cfg[start_offset];
        pa++;        /* EMV tag */
        len = *pa++; /* aid_len */
        /* 4 = 1 (tag) + 1 (len) + 1(nfcee_id) + 1(power cfg) */
        p_cb->size_aid += 4;
        p_cb->size_aid += len;
      }
      start_offset += p_cb->aid_len[xx];
    }
  }
  NFA_TRACE_DEBUG2("nfa_ee_update_route_aid_size nfcee_id:0x%x size_aid:%d",
                   p_cb->nfcee_id, p_cb->size_aid);
}

/*******************************************************************************
**
** Function         nfa_ee_total_lmrt_size
**
** Description      the total listen mode routing table size
**
** Returns          uint16_t
**
*******************************************************************************/
static uint16_t nfa_ee_total_lmrt_size(void) {
  int xx;
  uint16_t lmrt_size = 0;
  tNFA_EE_ECB* p_cb;

  p_cb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
  lmrt_size += p_cb->size_mask;
  lmrt_size += p_cb->size_aid;
  lmrt_size += p_cb->size_apdu;
  p_cb = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee - 1];
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb--) {
    if (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE) {
      lmrt_size += p_cb->size_mask;
      lmrt_size += p_cb->size_aid;
      lmrt_size += p_cb->size_apdu;
    }
  }
  NFA_TRACE_DEBUG1("nfa_ee_total_lmrt_size size:%d", lmrt_size);
  return lmrt_size;
}

static void nfa_ee_add_tech_route_to_ecb(tNFA_EE_ECB* p_cb, uint8_t* pp,
                                         uint8_t* p, uint8_t* ps,
                                         int* p_cur_offset) {
  uint8_t num_tlv = *ps;

  /* add the Technology based routing */
  for (int xx = 0; xx < NFA_EE_NUM_TECH; xx++) {
    uint8_t power_cfg = 0;
    if (p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
    if (power_cfg & NCI_ROUTE_PWR_STATE_ON) {
       if (p_cb->tech_screen_lock & nfa_ee_tech_mask_list[xx])
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
       if (p_cb->tech_screen_off & nfa_ee_tech_mask_list[xx])
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
       if (p_cb->tech_screen_off_lock & nfa_ee_tech_mask_list[xx])
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
       }
    if (power_cfg) {
      add_route_tech_proto_tlv(&pp, NFC_ROUTE_TAG_TECH, p_cb->nfcee_id,
                               power_cfg, nfa_ee_tech_list[xx]);
      num_tlv++;
      if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
        nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }

  /* update the num_tlv and current offset */
  uint8_t entry_size = (uint8_t)(pp - p);
  *p_cur_offset += entry_size;
  *ps = num_tlv;
}

static void nfa_ee_add_proto_route_to_ecb(tNFA_EE_ECB* p_cb, uint8_t* pp,
                                          uint8_t* p, uint8_t* ps,
                                          int* p_cur_offset) {
  uint8_t num_tlv = *ps;

  /* add the Protocol based routing */
  for (int xx = 0; xx < NFA_EE_NUM_PROTO; xx++) {
    uint8_t power_cfg = 0, proto_tag = 0;
    if (p_cb->proto_switch_on & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->proto_switch_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->proto_battery_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
    if (power_cfg) {
      /* Applying Route Block for ISO DEP Protocol, so that AIDs
       * which are not in the routing table can also be blocked */
      if (nfa_ee_proto_mask_list[xx] == NFA_PROTOCOL_MASK_ISO_DEP
#if(NXP_EXTNS == TRUE)
          ||nfa_ee_proto_mask_list[xx] == NFC_PROTOCOL_MASK_ISO7816
#endif
         ) {
        proto_tag = NFC_ROUTE_TAG_PROTO | nfa_ee_cb.route_block_control;

        /* Enable screen on lock power state for ISO-DEP protocol to
           enable HCE screen lock */
#if(NXP_EXTNS == TRUE)
       if (p_cb->nfcee_id == NFC_DH_ID)
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
      else
      {
        if (p_cb->proto_screen_lock & nfa_ee_proto_mask_list[xx])
           power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
        if (p_cb->proto_screen_off & nfa_ee_proto_mask_list[xx])
           power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
        if (p_cb->proto_screen_off_lock & nfa_ee_proto_mask_list[xx])
           power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
       }
#else
        if (NFC_GetNCIVersion() == NCI_VERSION_2_0)
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK;
#endif
      } else {
        proto_tag = NFC_ROUTE_TAG_PROTO;
      }

      add_route_tech_proto_tlv(&pp, proto_tag, p_cb->nfcee_id, power_cfg,
                               nfa_ee_proto_list[xx]);
      num_tlv++;
      if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
        nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }

  /* add NFC-DEP routing to HOST */
  if (p_cb->nfcee_id == NFC_DH_ID) {
    add_route_tech_proto_tlv(&pp, NFC_ROUTE_TAG_PROTO, NFC_DH_ID,
                             NCI_ROUTE_PWR_STATE_ON, NFC_PROTOCOL_NFC_DEP);

    num_tlv++;
    NFA_TRACE_DEBUG1("%s - NFC DEP added for DH!!!", __func__);
  }
  /* update the num_tlv and current offset */
  uint8_t entry_size = (uint8_t)(pp - p);
  *p_cur_offset += entry_size;
  *ps = num_tlv;
}

static void nfa_ee_add_aid_route_to_ecb(tNFA_EE_ECB* p_cb, uint8_t* pp,
                                        uint8_t* p, uint8_t* ps,
                                        int* p_cur_offset, int* p_max_len) {
  uint8_t num_tlv = *ps;

  /* add the AID routing */
  if (p_cb->aid_entries) {
    int start_offset = 0;
    for (int xx = 0; xx < p_cb->aid_entries; xx++) {
      /* remember the beginning of this AID routing entry, just in case we
       * need to put it in next command */
      uint8_t route_qual = 0;
      uint8_t* p_start = pp;
      /* add one AID entry */
      if (p_cb->aid_rt_info[xx] & NFA_EE_AE_ROUTE) {
        uint8_t* pa = &p_cb->aid_cfg[start_offset];

        NFA_TRACE_DEBUG2("%s -  p_cb->aid_info%x", __func__,
                         p_cb->aid_info[xx]);
        if (p_cb->aid_info[xx] & NCI_ROUTE_QUAL_LONG_SELECT) {
          NFA_TRACE_DEBUG2("%s - %x", __func__,
                           p_cb->aid_info[xx] & NCI_ROUTE_QUAL_LONG_SELECT);
          route_qual |= NCI_ROUTE_QUAL_LONG_SELECT;
        }
        if (p_cb->aid_info[xx] & NCI_ROUTE_QUAL_SHORT_SELECT) {
          NFA_TRACE_DEBUG2("%s - %x", __func__,
                           p_cb->aid_info[xx] & NCI_ROUTE_QUAL_SHORT_SELECT);
          route_qual |= NCI_ROUTE_QUAL_SHORT_SELECT;
        }

        uint8_t tag =
            NFC_ROUTE_TAG_AID | nfa_ee_cb.route_block_control | route_qual;
#if(NXP_EXTNS == TRUE)
            if(nfa_ee_is_active(p_cb->aid_rt_loc[xx]|NFA_HANDLE_GROUP_EE)) {
                add_route_aid_tlv(&pp, pa, p_cb->aid_rt_loc[xx], p_cb->aid_pwr_cfg[xx], tag);
                num_tlv++;
            } else {
                NFA_TRACE_DEBUG2("%s -  ignoring route loc%x", __func__,p_cb->aid_rt_loc[xx]);
            }
#else
        add_route_aid_tlv(&pp, pa, p_cb->nfcee_id, p_cb->aid_pwr_cfg[xx], tag);
#endif
      }
      start_offset += p_cb->aid_len[xx];
      uint8_t new_size = (uint8_t)(pp - p_start);
      nfa_ee_check_set_routing(new_size, p_max_len, ps, p_cur_offset);
      if (*ps == 0
#if(NXP_EXTNS == TRUE)
          && (num_tlv > 0x00)
#endif
         ) {
        /* just sent routing command, update local */
        *ps = 1;
        num_tlv = *ps;
        *p_cur_offset = new_size;
        pp = ps + 1;
        p = pp;
        memcpy(p, p_start, new_size);
        pp += new_size;
      } else {
        /* add the new entry */
        *ps = num_tlv;
        *p_cur_offset += new_size;
      }
    }
  } else {
    NFA_TRACE_DEBUG1("%s - No AID entries available", __func__);
  }
}
static void nfa_ee_add_apdu_route_to_ecb(tNFA_EE_ECB* p_cb, uint8_t* pp,
                                        uint8_t* p, uint8_t* ps,
                                        int* p_cur_offset, int* p_max_len) {
    uint8_t num_tlv = *ps, len;
    uint16_t tlv_size;
    tlv_size = (uint8_t)*p_cur_offset;
    int start_offset, xx;
    uint8_t  *pa;
    uint8_t* p_start;
    uint8_t new_size;
    uint8_t max_tlv;

    /* add the AID routing */
          if (p_cb->apdu_pattern_entries) {
            start_offset = 0;
            for (xx = 0; xx < p_cb->apdu_pattern_entries; xx++) {
                p_start = pp;
                /* add one APDU entry */
                if (p_cb->apdu_rt_info[xx] & NFA_EE_AE_ROUTE) {
                    num_tlv++;
                    pa = &p_cb->apdu_cfg[start_offset];
                    pa++;        /* EMV tag */
                    len = *pa++; /* apdu_len */
                    *pp++ = NFC_ROUTE_TAG_APDU;
                    *pp++ = len + 2;
                    *pp++ = (p_cb->apdu_rt_info[xx] >> NFA_EE_APDU_ROUTE_MASK);
                    NFA_TRACE_DEBUG1("nfa_ee_route_add_one_ecb_by_route_order p_cb->apdu_rt_info[xx] %x", p_cb->apdu_rt_info[xx]);
                    *pp++ = p_cb->apdu_pwr_cfg[xx];
                    /* copy the APDU */
                    memcpy(pp, pa, len);
                    pp += len;
                }
                start_offset += p_cb->apdu_len[xx];
                new_size = (uint8_t)(pp - p_start);
                NFA_TRACE_DEBUG1("nfa_ee_route_add_one_ecb_by_route_order --before num_tlv:- %d", num_tlv);
                NFA_TRACE_DEBUG1("nfa_ee_route_add_one_ecb_by_route_order --before new_size:- %d",new_size);
                nfa_ee_check_set_routing(new_size, p_max_len, ps, p_cur_offset);
                if (*ps == 0) {
                  /* just sent routing command, update local */
                  *ps = 1;
                   num_tlv = *ps;
                   *p_cur_offset = new_size;
                   pp = ps + 1;
                   p = pp;
                   tlv_size = (uint8_t)*p_cur_offset;
                   max_tlv = (uint8_t)((*p_max_len > NFA_EE_ROUT_MAX_TLV_SIZE)
                                        ? NFA_EE_ROUT_MAX_TLV_SIZE
                                        : *p_max_len);
                   memcpy(p, p_start, new_size);
                   pp += new_size;
                   } else {
                    /* add the new entry */
                     *ps = num_tlv;
                     *p_cur_offset += new_size;
                }
            }
            NFA_TRACE_DEBUG1("nfa_ee_route_add_one_ecb_by_route_order --num_tlv:- %d", num_tlv);
            NFA_TRACE_DEBUG1("nfa_ee_route_add_one_ecb_by_route_order --new_size:- %d",new_size);
          }
}
/*******************************************************************************
**
** Function         nfa_ee_conn_cback
**
** Description      process connection callback event from stack
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_conn_cback(uint8_t conn_id, tNFC_CONN_EVT event,
                              tNFC_CONN* p_data) {
  tNFA_EE_NCI_CONN cbk;

  NFA_TRACE_DEBUG2("nfa_ee_conn_cback: conn_id: %d, event=0x%02x", conn_id,
                   event);

  cbk.hdr.event = NFA_EE_NCI_CONN_EVT;
  if (event == NFC_DATA_CEVT) {
    /* Treat data event specially to avoid potential memory leak */
    cbk.hdr.event = NFA_EE_NCI_DATA_EVT;
  }
  cbk.conn_id = conn_id;
  cbk.event = event;
  cbk.p_data = p_data;

  nfa_ee_evt_hdlr((void*)&cbk);
}

/*******************************************************************************
**
** Function         nfa_ee_find_total_aid_len
**
** Description      Find the total len in aid_cfg from start_entry to the last
**
** Returns          void
**
*******************************************************************************/
int nfa_ee_find_total_aid_len(tNFA_EE_ECB* p_cb, int start_entry) {
  int len = 0, xx;

  if (p_cb->aid_entries > start_entry) {
    for (xx = start_entry; xx < p_cb->aid_entries; xx++) {
      len += p_cb->aid_len[xx];
    }
  }
  return len;
}

/*******************************************************************************
**
** Function         nfa_ee_find_total_apdu_len
**
** Description      Find the total len in apdu_cfg from start_entry to the last
**
** Returns          void
**
*******************************************************************************/
int nfa_ee_find_total_apdu_len(tNFA_EE_ECB* p_cb, int start_entry) {
  int len = 0, xx;

  if (p_cb->apdu_pattern_entries > start_entry) {
    for (xx = start_entry; xx < p_cb->apdu_pattern_entries; xx++) {
      len += p_cb->apdu_len[xx];
    }
  }
  return len;
}
#if (NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function         nfa_all_ee_find_total_aid_len
**
** Description      Find the total len in aid_cfg from start_entry to the last
**                  for all EE and DH.
**
** Returns          total length
**
*******************************************************************************/
int nfa_all_ee_find_total_aid_len() {
  uint32_t xx;
  int total_len = 0;
  tNFA_EE_ECB* p_cb = nfa_ee_cb.ecb;
  for (xx = 0; xx < nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED; xx++, p_cb++) {
    total_len += nfa_ee_find_total_aid_len(p_cb, 0);
    total_len += (p_cb->aid_entries * 2); /*Adding tag/len */
  }

  tNFA_EE_ECB* p_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
  total_len += nfa_ee_find_total_aid_len(p_ecb, 0);
  total_len += (p_ecb->aid_entries * 2); /*Adding tag/len */

  return total_len;
}
/*******************************************************************************
**
** Function         nfa_all_ee_find_total_apdu_pattern_len
**
** Description      Find the total len in apdu_cfg from start_entry to the last
**                  for all EE and DH.
**
** Returns          total length
**
*******************************************************************************/
int nfa_all_ee_find_total_apdu_pattern_len() {
  uint32_t xx;
  int total_len = 0;
  tNFA_EE_ECB* p_cb = nfa_ee_cb.ecb;
  for (xx = 0; xx < NFA_EE_MAX_EE_SUPPORTED; xx++, p_cb++) {
    total_len += nfa_ee_find_total_apdu_len(p_cb, 0);
    total_len += (p_cb->apdu_pattern_entries * 2); /*Adding nfceeid/power_state */
  }
  return total_len;
}
#endif

/*******************************************************************************
**
** Function         nfa_ee_find_aid_offset
**
** Description      Given the AID, find the associated tNFA_EE_ECB and the
**                  offset in aid_cfg[]. *p_entry is the index.
**
** Returns          void
**
*******************************************************************************/
tNFA_EE_ECB* nfa_ee_find_aid_offset(uint8_t aid_len, uint8_t* p_aid,
                                    int* p_offset, int* p_entry) {
  int xx, yy, aid_len_offset, offset;
  tNFA_EE_ECB* p_ret = NULL, *p_ecb;

  p_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
  aid_len_offset = 1; /* skip the tag */
  for (yy = 0; yy < nfa_ee_cb.cur_ee; yy++, p_ecb++) {
    if (p_ecb->aid_entries) {
      offset = 0;
      for (xx = 0; xx < p_ecb->aid_entries; xx++) {
        if ((p_ecb->aid_cfg[offset + aid_len_offset] == aid_len) &&
            (memcmp(&p_ecb->aid_cfg[offset + aid_len_offset + 1], p_aid,
                    aid_len) == 0)) {
          p_ret = p_ecb;
          if (p_offset) *p_offset = offset;
          if (p_entry) *p_entry = xx;
          break;
        }
        offset += p_ecb->aid_len[xx];
      }

      if (p_ret) {
        /* found the entry already */
        break;
      }
    }
    p_ecb = &nfa_ee_cb.ecb[yy];
  }

  return p_ret;
}

/*******************************************************************************
**
** Function         nfa_ee_find_apdu_offset
**
** Description      Given the APDU, find the associated tNFA_EE_ECB and the
**                  offset in apdu_cfg[]. *p_entry is the index.
**
** Returns          void
**
*******************************************************************************/
tNFA_EE_ECB* nfa_ee_find_apdu_offset(uint8_t apdu_len, uint8_t* p_apdu,
                                    int* p_offset, int* p_entry) {
  int xx, yy, apdu_len_offset = 1, offset;
  tNFA_EE_ECB* p_ret = NULL, *p_ecb;
  p_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
  for (yy = 0; yy < nfa_ee_cb.cur_ee; yy++, p_ecb++) {
    if (p_ecb->apdu_pattern_entries) {
      offset = 0;
      for (xx = 0; xx < p_ecb->apdu_pattern_entries; xx++) {
        if (((p_ecb->apdu_cfg[offset + apdu_len_offset]/2) == apdu_len) &&
            (memcmp(&p_ecb->apdu_cfg[offset + apdu_len_offset + 1], p_apdu,
                    apdu_len) == 0)) {
          p_ret = p_ecb;
          if (p_offset) *p_offset = offset;
            if (p_entry) *p_entry = xx;
          break;
        }
        offset += p_ecb->aid_len[xx];
      }

      if (p_ret) {
        /* found the entry already */
        break;
      }
    }
    p_ecb = &nfa_ee_cb.ecb[yy];
  }
  return p_ret;
}

/*******************************************************************************
**
** Function         nfa_ee_report_event
**
** Description      report the given event to the callback
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_report_event(tNFA_EE_CBACK* p_cback, tNFA_EE_EVT event,
                         tNFA_EE_CBACK_DATA* p_data) {
  int xx;

  /* use the given callback, if not NULL */
  if (p_cback) {
    (*p_cback)(event, p_data);
    return;
  }
  /* if the given is NULL, report to all registered ones */
  for (xx = 0; xx < NFA_EE_MAX_CBACKS; xx++) {
    if (nfa_ee_cb.p_ee_cback[xx] != NULL) {
      (*nfa_ee_cb.p_ee_cback[xx])(event, p_data);
    }
  }
}
/*******************************************************************************
**
** Function         nfa_ee_start_timer
**
** Description      start the de-bounce timer
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_start_timer(void) {
  if (nfa_dm_is_active())
    nfa_sys_start_timer(&nfa_ee_cb.timer, NFA_EE_ROUT_TIMEOUT_EVT,
                        NFA_EE_ROUT_TIMEOUT_VAL);
}

/*******************************************************************************
**
** Function         nfa_ee_get_num_nfcee_configured
**
** Description      Get the number of NFCEE configured
**
** Returns          void
**
*******************************************************************************/
tNFA_STATUS nfa_ee_get_num_nfcee_configured(tNFA_VSC_CBACK* p_cback)
{
  tNFA_STATUS status = NFA_STATUS_OK;
  uint8_t p_data[255];
  uint8_t* p = p_data, *parm_len, *num_param;
  memset(p_data, 0, sizeof(p_data));
  NCI_MSG_BLD_HDR0(p, NCI_MT_CMD, NCI_GID_CORE);
  NCI_MSG_BLD_HDR1(p, NCI_MSG_CORE_GET_CONFIG);
  parm_len = p++;
  num_param = p++;
  UINT8_TO_STREAM(p, NXP_NFC_SET_CONFIG_PARAM_EXT);
  UINT8_TO_STREAM(p, NXP_NFC_PARAM_ID_SWP1);
  (*num_param)++;
  UINT8_TO_STREAM(p, NXP_NFC_SET_CONFIG_PARAM_EXT);
  UINT8_TO_STREAM(p, NXP_NFC_PARAM_ID_SWP2);
  (*num_param)++;

  if(nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC) {
    UINT8_TO_STREAM(p, NXP_NFC_SET_CONFIG_PARAM_EXT);
    UINT8_TO_STREAM(p, NXP_NFC_PARAM_ID_SWP1A);
    (*num_param)++;
  }

  *parm_len = (p - num_param);
  if (*num_param != 0x00) {
    status = NFA_SendRawVsCommand(p - p_data, p_data, p_cback);
  } else {
      status = NFA_STATUS_FAILED;
  }
  NFA_TRACE_DEBUG1("nfa_ee_get_num_nfcee_configured %x", *num_param);

  return status;
}

/*******************************************************************************
**
** Function         nfa_hci_read_num_nfcee_config_cb
**
** Description      Callback function to handle the response of num of NFCEE
*                   configured.
**
** Returns          None
**
*********************************************************************************/
 void nfa_ee_read_num_nfcee_config_cb(uint8_t event, uint16_t param_len,
                                             uint8_t* p_param) {
    uint8_t num_param_id = 0x00, xx;
    uint8_t configured_num_nfcee = 0x00;
    uint8_t NFA_PARAM_ID_INDEX = 0x04;
    uint8_t param_id1 = 0x00;
    uint8_t param_id2 = 0x00;
    uint8_t status = 0x00;

    if (param_len == 0x00 || p_param == NULL ||
        p_param[NFA_PARAM_ID_INDEX - 1] != NFA_STATUS_OK) {
      return;
    }
    if(NFA_GetNCIVersion() != NCI_VERSION_2_0) { //HCI_NETWORK_ACCESS
        configured_num_nfcee = 1;
    }
    p_param += NFA_PARAM_ID_INDEX;
    STREAM_TO_UINT8(num_param_id, p_param);

    while (num_param_id > 0x00) {
      STREAM_TO_UINT8(param_id1, p_param);
      STREAM_TO_UINT8(param_id2, p_param);
      p_param++;
      STREAM_TO_UINT8(status, p_param);
      if (param_id1 == NXP_NFC_SET_CONFIG_PARAM_EXT &&
          param_id2 == NXP_NFC_PARAM_ID_SWP1 && status != 0x00)
        configured_num_nfcee++;
      else if (param_id1 == NXP_NFC_SET_CONFIG_PARAM_EXT &&
               param_id2 == NXP_NFC_PARAM_ID_SWP2 && status != 0x00)
        configured_num_nfcee++;
      else if (param_id1 == NXP_NFC_SET_CONFIG_PARAM_EXT &&
               param_id2 == NXP_NFC_PARAM_ID_SWP1A && status != 0x00)
        configured_num_nfcee++;
      else if (param_id1 == NXP_NFC_SET_CONFIG_PARAM_EXT &&
               param_id2 == NXP_NFC_PARAM_ID_NDEF_NFCEE && status != 0x00)
        configured_num_nfcee++;
      num_param_id--;
    }
    if(configured_num_nfcee)
        nfa_ee_max_ee_cfg = configured_num_nfcee;
    NFA_TRACE_DEBUG1("nfa_ee_read_num_nfcee_config_cb %x", configured_num_nfcee);
}

/******************************************************************************
**
** Function         nfa_ee_api_discover
**
** Description      process discover command from user
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_discover(tNFA_EE_MSG* p_data) {
  tNFA_EE_CBACK* p_cback = p_data->ee_discover.p_cback;
  tNFA_EE_CBACK_DATA evt_data = {0};

  NFA_TRACE_DEBUG1("nfa_ee_api_discover() in_use:%d",
                   nfa_ee_cb.discv_timer.in_use);
  if (nfa_ee_cb.discv_timer.in_use) {
    nfa_sys_stop_timer(&nfa_ee_cb.discv_timer);
    if(NFA_GetNCIVersion() != NCI_VERSION_2_0)
      NFC_NfceeDiscover(false);
    else {
        evt_data.status = NFA_STATUS_FAILED;
        nfa_ee_report_event(p_cback, NFA_EE_DISCOVER_EVT, &evt_data);
        return;
    }
  }
  nfa_ee_get_num_nfcee_configured(nfa_ee_read_num_nfcee_config_cb);
  if (nfa_ee_cb.p_ee_disc_cback == NULL &&
      NFC_NfceeDiscover(true) == NFC_STATUS_OK) {
    nfa_ee_cb.p_ee_disc_cback = p_cback;
  } else {
    evt_data.status = NFA_STATUS_FAILED;
    nfa_ee_report_event(p_cback, NFA_EE_DISCOVER_EVT, &evt_data);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_api_register
**
** Description      process register command from user
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_register(tNFA_EE_MSG* p_data) {
  int xx;
  tNFA_EE_CBACK* p_cback = p_data->ee_register.p_cback;
  tNFA_EE_CBACK_DATA evt_data = {0};
  bool found = false;

  evt_data.ee_register = NFA_STATUS_FAILED;
  /* loop through all entries to see if there's a matching callback */
  for (xx = 0; xx < NFA_EE_MAX_CBACKS; xx++) {
    if (nfa_ee_cb.p_ee_cback[xx] == p_cback) {
      evt_data.ee_register = NFA_STATUS_OK;
      found = true;
      break;
    }
  }

  /* If no matching callback, allocated an entry */
  if (!found) {
    for (xx = 0; xx < NFA_EE_MAX_CBACKS; xx++) {
      if (nfa_ee_cb.p_ee_cback[xx] == NULL) {
        nfa_ee_cb.p_ee_cback[xx] = p_cback;
        evt_data.ee_register = NFA_STATUS_OK;
        break;
      }
    }
  }

#if (NXP_EXTNS == TRUE)
  if((nfcFL.chipType != pn547C2) &&
          (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
      max_routing_table_size = nfc_ncif_getMaxRoutingTableSize();
      if (max_routing_table_size <
              (NFA_EE_TOTAL_TECH_ROUTE_SIZE + NFA_EE_TOTAL_PROTO_ROUTE_SIZE)) {
          max_routing_table_size = NFA_EE_ROUT_BUF_SIZE;
      }
      max_aid_config_length = nfa_ee_find_max_aid_config_length();
      max_aid_entries = max_aid_config_length / NFA_EE_MIN_AID_ENTRY_SIZE;
      NFA_TRACE_DEBUG3(
              "max_routing_table_size = %d max_aid_config_length: %d and "
              "max_aid_entries: %d",
              max_routing_table_size, max_aid_config_length, max_aid_entries);
      /* Dynamic ecb allocation based on max aid entry calculated from max. routing
       * table size( from core_init rsp )*/
      for (xx = 0; xx < NFA_EE_NUM_ECBS; xx++) {
          nfa_ee_cb.ecb[xx].aid_len = GKI_getbuf(max_aid_entries);
          nfa_ee_cb.ecb[xx].aid_pwr_cfg = GKI_getbuf(max_aid_entries);
          nfa_ee_cb.ecb[xx].aid_rt_info = GKI_getbuf(max_aid_entries);
          nfa_ee_cb.ecb[xx].aid_rt_loc = GKI_getbuf(max_aid_entries);
          nfa_ee_cb.ecb[xx].aid_info    = GKI_getbuf(max_aid_entries);
          nfa_ee_cb.ecb[xx].aid_cfg = GKI_getbuf(max_aid_config_length);
          if ((NULL != nfa_ee_cb.ecb[xx].aid_len) &&
                  (NULL != nfa_ee_cb.ecb[xx].aid_pwr_cfg) &&
                  (NULL != nfa_ee_cb.ecb[xx].aid_rt_info) &&
                  (NULL != nfa_ee_cb.ecb[xx].aid_rt_loc) &&
                  (NULL != nfa_ee_cb.ecb[xx].aid_info) &&
                  (NULL != nfa_ee_cb.ecb[xx].aid_cfg)) {
              memset(nfa_ee_cb.ecb[xx].aid_len, 0, max_aid_entries);
              memset(nfa_ee_cb.ecb[xx].aid_pwr_cfg, 0, max_aid_entries);
              memset(nfa_ee_cb.ecb[xx].aid_rt_info, 0, max_aid_entries);
              memset(nfa_ee_cb.ecb[xx].aid_rt_loc, 0, max_aid_entries);
              memset(nfa_ee_cb.ecb[xx].aid_info, 0, max_aid_entries);
              memset(nfa_ee_cb.ecb[xx].aid_cfg, 0, max_aid_config_length);
          } else {
              NFA_TRACE_ERROR0("GKI_getbuf allocation for ECB failed !");
          }
      }
  }
  else {
      for (xx = 0; xx < NFA_EE_NUM_ECBS; xx++) {
          nfa_ee_cb.ecb[xx].aid_len = nfa_ee_cb.ecb[xx].aid_len_stat;
          nfa_ee_cb.ecb[xx].aid_pwr_cfg = nfa_ee_cb.ecb[xx].aid_pwr_cfg_stat;
          nfa_ee_cb.ecb[xx].aid_rt_info = nfa_ee_cb.ecb[xx].aid_rt_info_stat;
          nfa_ee_cb.ecb[xx].aid_rt_loc = nfa_ee_cb.ecb[xx].aid_rt_loc_stat;
          nfa_ee_cb.ecb[xx].aid_cfg = nfa_ee_cb.ecb[xx].aid_cfg_stat;
      }
  }
#endif

  /* This callback is verified (not NULL) in NFA_EeRegister() */
  (*p_cback)(NFA_EE_REGISTER_EVT, &evt_data);

  /* report NFCEE Discovery Request collected during booting up */
  nfa_ee_build_discover_req_evt(&evt_data.discover_req);
  (*p_cback)(NFA_EE_DISCOVER_REQ_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_deregister
**
** Description      process de-register command from user
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_deregister(tNFA_EE_MSG* p_data) {
  tNFA_EE_CBACK* p_cback = NULL;
#if (NXP_EXTNS == TRUE)
  int index = p_data->deregister.index, xx = 0;
#else
  int index = p_data->deregister.index;
#endif
  tNFA_EE_CBACK_DATA evt_data = {0};

  NFA_TRACE_DEBUG0("nfa_ee_api_deregister");
#if (NXP_EXTNS == TRUE)
  if((nfcFL.chipType != pn547C2) &&
          (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
      for (xx = 0; xx < NFA_EE_NUM_ECBS; xx++) {
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_len);
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_pwr_cfg);
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_rt_info);
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_rt_loc);
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_info);
          GKI_freebuf(nfa_ee_cb.ecb[xx].aid_cfg);
      }
  }
#endif
  p_cback = nfa_ee_cb.p_ee_cback[index];
  nfa_ee_cb.p_ee_cback[index] = NULL;
  if (p_cback) (*p_cback)(NFA_EE_DEREGISTER_EVT, &evt_data);
}
/*******************************************************************************
**
** Function         nfa_ee_api_power_link_set
**
** Description      process power link command request
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_power_link_set(tNFA_EE_MSG* p_data) {
    tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;
    NFC_Nfcee_PwrLinkCtrl(p_data->pwr_lnk_ctrl_set.nfcee_id, p_data->pwr_lnk_ctrl_set.cfg_value);
    return;
}
/*******************************************************************************
**
** Function         nfa_ee_api_mode_set
**
** Description      process mode set command from user
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_mode_set(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;
  tNFA_EE_MODE_SET         mode_set;
  NFA_TRACE_DEBUG2("nfa_ee_api_mode_set() handle:0x%02x mode:%d",
                   p_cb->nfcee_id, p_data->mode_set.mode);
  if(NFA_GetNCIVersion() != NCI_VERSION_2_0)
  {
      NFC_NfceeModeSet(p_cb->nfcee_id, p_data->mode_set.mode);
  }else {
      mode_set.status = NFC_NfceeModeSet(p_cb->nfcee_id, p_data->mode_set.mode);
      if (mode_set.status != NFC_STATUS_OK)
      {
        /* the api is rejected at NFC layer, report the failure status right away */
        mode_set.ee_handle  = (tNFA_HANDLE)p_cb->nfcee_id | NFA_HANDLE_GROUP_EE;
        mode_set.ee_status  = p_data->mode_set.mode;
        nfa_ee_report_event(NULL, NFA_EE_MODE_SET_EVT, (tNFA_EE_CBACK_DATA *)&mode_set);
        return;
      }
  }
  /* set the NFA_EE_STATUS_PENDING bit to indicate the status is not exactly
   * active */
  if (p_data->mode_set.mode == NFC_MODE_ACTIVATE
#if (NXP_EXTNS == TRUE)
      && (p_cb->ee_status != NFA_EE_STATUS_REMOVED)
#endif
          )
#if (NXP_EXTNS == TRUE)
    p_cb->ee_status = NFA_EE_STATUS_ACTIVE;
#else
    p_cb->ee_status = NFA_EE_STATUS_PENDING | NFA_EE_STATUS_ACTIVE;
#endif
  else {
#if (NXP_EXTNS == TRUE)
    if (p_cb->ee_status != NFA_EE_STATUS_REMOVED)
#endif
      p_cb->ee_status = NFA_EE_STATUS_INACTIVE;
    /* DH should release the NCI connection before deactivate the NFCEE */
    if (p_cb->conn_st == NFA_EE_CONN_ST_CONN) {
      p_cb->conn_st = NFA_EE_CONN_ST_DISC;
      NFC_ConnClose(p_cb->conn_id);
    }
  }
  /* report the NFA_EE_MODE_SET_EVT status on the response from NFCC */
}

/*******************************************************************************
**
** Function         nfa_ee_api_set_tech_cfg
**
** Description      process set technology routing configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_set_tech_cfg(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;
  tNFA_EE_CBACK_DATA evt_data = {0};
  tNFA_TECHNOLOGY_MASK old_tech_switch_on = p_cb->tech_switch_on;
  tNFA_TECHNOLOGY_MASK old_tech_switch_off = p_cb->tech_switch_off;
  tNFA_TECHNOLOGY_MASK old_tech_battery_off = p_cb->tech_battery_off;
#if (NXP_EXTNS == TRUE)
  tNFA_TECHNOLOGY_MASK old_tech_screen_lock = p_cb->tech_screen_lock;
  tNFA_TECHNOLOGY_MASK old_tech_screen_off = p_cb->tech_screen_off;
  tNFA_TECHNOLOGY_MASK old_tech_screen_off_lock = p_cb->tech_screen_off_lock;
#endif
  uint8_t old_size_mask = p_cb->size_mask;

  if ((p_cb->tech_switch_on == p_data->set_tech.technologies_switch_on) &&
      (p_cb->tech_switch_off == p_data->set_tech.technologies_switch_off) &&
      (p_cb->tech_battery_off == p_data->set_tech.technologies_battery_off)
#if (NXP_EXTNS == TRUE)
      &&
      (p_cb->tech_screen_lock == p_data->set_tech.technologies_screen_lock) &&
      (p_cb->tech_screen_off == p_data->set_tech.technologies_screen_off) &&
      (p_cb->tech_screen_off_lock == p_data->set_tech.technologies_screen_off_lock)
#endif
          ) {
    /* nothing to change */
    evt_data.status = NFA_STATUS_OK;
    nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_SET_TECH_CFG_EVT, &evt_data);
    return;
  }
  p_cb->tech_switch_on = p_data->set_tech.technologies_switch_on;
  p_cb->tech_switch_off = p_data->set_tech.technologies_switch_off;
  p_cb->tech_battery_off = p_data->set_tech.technologies_battery_off;
#if (NXP_EXTNS == TRUE)
  p_cb->tech_screen_lock = p_data->set_tech.technologies_screen_lock;
  p_cb->tech_screen_off = p_data->set_tech.technologies_screen_off;
  p_cb->tech_screen_off_lock = p_data->set_tech.technologies_screen_off_lock;
#endif
  nfa_ee_update_route_size(p_cb);
  if (nfa_ee_total_lmrt_size() > NFC_GetLmrtSize()) {
    NFA_TRACE_ERROR0("nfa_ee_api_set_tech_cfg Exceed LMRT size");
    evt_data.status = NFA_STATUS_BUFFER_FULL;
    p_cb->tech_switch_on = old_tech_switch_on;
    p_cb->tech_switch_off = old_tech_switch_off;
    p_cb->tech_battery_off = old_tech_battery_off;
#if (NXP_EXTNS == TRUE)
    p_cb->tech_screen_lock = old_tech_screen_lock;
    p_cb->tech_screen_off = old_tech_screen_off;
    p_cb->tech_screen_off_lock = old_tech_screen_off_lock;
#endif
    p_cb->size_mask = old_size_mask;
  } else {
    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_TECH;
    if (p_cb->tech_switch_on | p_cb->tech_switch_off | p_cb->tech_battery_off
#if (NXP_EXTNS == TRUE)
        | p_cb->tech_screen_lock | p_cb->tech_screen_off | p_cb->tech_screen_off_lock
#endif
        ) {
      /* if any technology in any power mode is configured, mark this entry as
       * configured */
      nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    }
    nfa_ee_start_timer();
  }
  nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_SET_TECH_CFG_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_set_proto_cfg
**
** Description      process set protocol routing configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_set_proto_cfg(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;
  tNFA_EE_CBACK_DATA evt_data = {0};
  tNFA_PROTOCOL_MASK old_proto_switch_on = p_cb->proto_switch_on;
  tNFA_PROTOCOL_MASK old_proto_switch_off = p_cb->proto_switch_off;
  tNFA_PROTOCOL_MASK old_proto_battery_off = p_cb->proto_battery_off;
#if (NXP_EXTNS == TRUE)
  tNFA_PROTOCOL_MASK old_proto_screen_lock = p_cb->proto_screen_lock;
  tNFA_PROTOCOL_MASK old_proto_screen_off = p_cb->proto_screen_off;
  tNFA_PROTOCOL_MASK old_proto_screen_off_lock = p_cb->proto_screen_off_lock;
#endif
  uint8_t old_size_mask = p_cb->size_mask;

  if ((p_cb->proto_switch_on == p_data->set_proto.protocols_switch_on) &&
      (p_cb->proto_switch_off == p_data->set_proto.protocols_switch_off) &&
      (p_cb->proto_battery_off == p_data->set_proto.protocols_battery_off)
#if (NXP_EXTNS == TRUE)
      && (p_cb->proto_screen_lock == p_data->set_proto.protocols_screen_lock) &&
      (p_cb->proto_screen_off == p_data->set_proto.protocols_screen_off) &&
      (p_cb->proto_screen_off_lock == p_data->set_proto.protocols_screen_off_lock)
#endif
          ) {
    /* nothing to change */
    evt_data.status = NFA_STATUS_OK;
    nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_SET_PROTO_CFG_EVT, &evt_data);
    return;
  }
  p_cb->proto_switch_on = p_data->set_proto.protocols_switch_on;
  p_cb->proto_switch_off = p_data->set_proto.protocols_switch_off;
  p_cb->proto_battery_off = p_data->set_proto.protocols_battery_off;
#if (NXP_EXTNS == TRUE)
  p_cb->proto_screen_lock = p_data->set_proto.protocols_screen_lock;
  p_cb->proto_screen_off = p_data->set_proto.protocols_screen_off;
  p_cb->proto_screen_off_lock = p_data->set_proto.protocols_screen_off_lock;
#endif
  nfa_ee_update_route_size(p_cb);
  if (nfa_ee_total_lmrt_size() > NFC_GetLmrtSize()) {
    NFA_TRACE_ERROR0("nfa_ee_api_set_proto_cfg Exceed LMRT size");
    evt_data.status = NFA_STATUS_BUFFER_FULL;
    p_cb->proto_switch_on = old_proto_switch_on;
    p_cb->proto_switch_off = old_proto_switch_off;
    p_cb->proto_battery_off = old_proto_battery_off;
#if (NXP_EXTNS == TRUE)
    p_cb->proto_screen_lock = old_proto_screen_lock;
    p_cb->proto_screen_off = old_proto_screen_off;
    p_cb->proto_screen_off_lock = old_proto_screen_off_lock;
#endif
    p_cb->size_mask = old_size_mask;
  } else {
    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_PROTO;
    if (p_cb->proto_switch_on | p_cb->proto_switch_off | p_cb->proto_battery_off
#if (NXP_EXTNS == TRUE)
        | p_cb->proto_screen_lock | p_cb->proto_screen_off | p_cb->proto_screen_off_lock
#endif
        ) {
      /* if any protocol in any power mode is configured, mark this entry as
       * configured */
      nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    }
    nfa_ee_start_timer();
  }
  nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_SET_PROTO_CFG_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_add_aid
**
** Description      process add an AID routing configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_add_aid(tNFA_EE_MSG* p_data) {
  tNFA_EE_API_ADD_AID* p_add = &p_data->add_aid;
  tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;

#if (NXP_EXTNS == TRUE)
  tNFA_EE_ECB* dh_ecb = NULL;
  uint8_t aid_info = p_add->aidInfo;
  uint16_t aid_config_length_max = 0;
  uint16_t aid_entries_max = 0;
#endif
  tNFA_EE_ECB* p_chk_cb;
  uint8_t* p, *p_start;
  int len, len_needed;
  tNFA_EE_CBACK_DATA evt_data = {0};
  int offset = 0, entry = 0;
  uint16_t new_size;

  nfa_ee_trace_aid("nfa_ee_api_add_aid", p_cb->nfcee_id, p_add->aid_len,
                   p_add->p_aid);
  p_chk_cb =
      nfa_ee_find_aid_offset(p_add->aid_len, p_add->p_aid, &offset, &entry);
  if(NFA_GetNCIVersion() == NCI_VERSION_1_0) {
    if((p_add->power_state & 0x08) || (p_add->power_state & 0x20)) {
      p_add->power_state &= ~0x08;
      p_add->power_state &= ~0x20;
      p_add->power_state |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
    }
    if(p_add->power_state & 0x10){
      p_add->power_state &= ~0x10;
      p_add->power_state |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
    }
  }
  if (p_chk_cb) {
    NFA_TRACE_DEBUG0(
        "nfa_ee_api_add_aid The AID entry is already in the database");
    if (p_chk_cb == p_cb) {
      p_cb->aid_rt_info[entry] |= NFA_EE_AE_ROUTE;
      p_cb->aid_info[entry] = p_add->aidInfo;
      new_size = nfa_ee_total_lmrt_size();
      if (new_size > NFC_GetLmrtSize()) {
        NFA_TRACE_ERROR1("Exceed LMRT size:%d (add ROUTE)", new_size);
        evt_data.status = NFA_STATUS_BUFFER_FULL;
        p_cb->aid_rt_info[entry] &= ~NFA_EE_AE_ROUTE;
      } else {
        p_cb->aid_pwr_cfg[entry] = p_add->power_state;
#if (NXP_EXTNS == TRUE)
        p_cb->aid_info[entry] = p_add->aidInfo;
#endif
      }
    } else {
      NFA_TRACE_ERROR1(
          "The AID entry is already in the database for different NFCEE "
          "ID:0x%02x",
          p_chk_cb->nfcee_id);
      evt_data.status = NFA_STATUS_SEMANTIC_ERROR;
    }
  } else {
/* Find the total length so far */
#if (NXP_EXTNS == TRUE)
    len = nfa_all_ee_find_total_aid_len();
    len += nfa_all_ee_find_total_apdu_pattern_len();
    dh_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
#else
    len = nfa_ee_find_total_aid_len(p_cb, 0);
#endif

    /* make sure the control block has enough room to hold this entry */
    len_needed = p_add->aid_len + 2; /* tag/len */
    if((nfcFL.chipType != pn547C2) &&
            (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
        aid_config_length_max = max_aid_config_length;
        aid_entries_max = max_aid_entries;
    }
    else{
        aid_config_length_max = NFA_EE_MAX_AID_CFG_LEN_STAT;
        aid_entries_max = nfcFL.nfcMwFL._NFA_EE_MAX_AID_ENTRIES;
    }
    if ((len_needed + len) >
#if (NXP_EXTNS == TRUE)
    aid_config_length_max
#else
    NFA_EE_MAX_AID_CFG_LEN
#endif
        ) {
#if (NXP_EXTNS == TRUE)
      NFA_TRACE_ERROR3(
          "Exceed capacity: (len_needed:%d + len:%d) > "
          "max_aid_config_length:%d",
          len_needed, len, aid_config_length_max);
#else
      NFA_TRACE_ERROR3(
          "Exceed capacity: (len_needed:%d + len:%d) > "
          "NFA_EE_MAX_AID_CFG_LEN:%d",
          len_needed, len, NFA_EE_MAX_AID_CFG_LEN);
#endif
      evt_data.status = NFA_STATUS_BUFFER_FULL;
    }
#if (NXP_EXTNS == TRUE)
    else if (dh_ecb->aid_entries < aid_entries_max)
#else
    else if (p_cb->aid_entries < NFA_EE_MAX_AID_ENTRIES)
#endif
    {
      /* 4 = 1 (tag) + 1 (len) + 1(nfcee_id) + 1(power cfg) */
      new_size = nfa_ee_total_lmrt_size() + 4 + p_add->aid_len;
      if (new_size > NFC_GetLmrtSize()) {
        NFA_TRACE_ERROR1("Exceed LMRT size:%d", new_size);
        evt_data.status = NFA_STATUS_BUFFER_FULL;
      } else {
/* add AID */
#if (NXP_EXTNS == TRUE)
        len = nfa_ee_find_total_aid_len(dh_ecb, 0);
        // Always use single aid_cfg buffer to keep the aid order intact.
        dh_ecb->aid_pwr_cfg[dh_ecb->aid_entries] = p_add->power_state;
        dh_ecb->aid_rt_info[dh_ecb->aid_entries] = NFA_EE_AE_ROUTE;
        dh_ecb->aid_rt_loc[dh_ecb->aid_entries] = p_cb->nfcee_id;
        dh_ecb->aid_info[dh_ecb->aid_entries] = p_add->aidInfo;
        p = dh_ecb->aid_cfg + len;
#else
        p_cb->aid_pwr_cfg[p_cb->aid_entries] = p_add->power_state;
        p_cb->aid_info[p_cb->aid_entries] = p_add->aidInfo;
        p_cb->aid_rt_info[p_cb->aid_entries] = NFA_EE_AE_ROUTE;
        p = p_cb->aid_cfg + len;
#endif
        p_start = p;
        *p++ = NFA_EE_AID_CFG_TAG_NAME;
        *p++ = p_add->aid_len;
        memcpy(p, p_add->p_aid, p_add->aid_len);
        p += p_add->aid_len;

#if (NXP_EXTNS == TRUE)
        dh_ecb->aid_len[dh_ecb->aid_entries++] = (uint8_t)(p - p_start);
#else
        p_cb->aid_len[p_cb->aid_entries++] = (uint8_t)(p - p_start);
#endif
      }
    } else {
      NFA_TRACE_ERROR1("Exceed NFA_EE_MAX_AID_ENTRIES:%d",
                       NFA_EE_MAX_AID_ENTRIES);
      evt_data.status = NFA_STATUS_BUFFER_FULL;
    }
  }

  if (evt_data.status == NFA_STATUS_OK) {
    /* mark AID changed */
    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_AID;
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    nfa_ee_update_route_aid_size(p_cb);
    nfa_ee_start_timer();
  }
  NFA_TRACE_DEBUG2("status:%d ee_cfged:0x%02x ", evt_data.status,
                   nfa_ee_cb.ee_cfged);
  /* report the status of this operation */
  nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_ADD_AID_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_remove_aid
**
** Description      process remove an AID routing configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_remove_aid(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_CBACK_DATA evt_data = {0};
  int offset = 0, entry = 0, len;
  int rest_len;
  tNFA_EE_CBACK* p_cback = NULL;

  nfa_ee_trace_aid("nfa_ee_api_remove_aid", 0, p_data->rm_aid.aid_len,
                   p_data->rm_aid.p_aid);
  p_cb = nfa_ee_find_aid_offset(p_data->rm_aid.aid_len, p_data->rm_aid.p_aid,
                                &offset, &entry);
  if (p_cb && p_cb->aid_entries) {
    NFA_TRACE_DEBUG2("aid_rt_info[%d]: 0x%02x", entry,
                     p_cb->aid_rt_info[entry]);
    /* mark routing and VS changed */
    if (p_cb->aid_rt_info[entry] & NFA_EE_AE_ROUTE)
      p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_AID;

    if (p_cb->aid_rt_info[entry] & NFA_EE_AE_VS)
      p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_VS;

    /* remove the aid */
    if ((entry + 1) < p_cb->aid_entries) {
      /* not the last entry, move the aid entries in control block */
      /* Find the total len from the next entry to the last one */
      rest_len = nfa_ee_find_total_aid_len(p_cb, entry + 1);

      len = p_cb->aid_len[entry];
      NFA_TRACE_DEBUG2("nfa_ee_api_remove_aid len:%d, rest_len:%d", len,
                       rest_len);
      GKI_shiftup(&p_cb->aid_cfg[offset], &p_cb->aid_cfg[offset + len],
                  rest_len);
      rest_len = p_cb->aid_entries - entry;
      GKI_shiftup(&p_cb->aid_len[entry], &p_cb->aid_len[entry + 1], rest_len);
      GKI_shiftup(&p_cb->aid_pwr_cfg[entry], &p_cb->aid_pwr_cfg[entry + 1],
                  rest_len);
      GKI_shiftup(&p_cb->aid_info[entry], &p_cb->aid_info[entry + 1],
                  rest_len);
      GKI_shiftup(&p_cb->aid_rt_info[entry], &p_cb->aid_rt_info[entry + 1],
                  rest_len);
#if (NXP_EXTNS == TRUE)
      GKI_shiftup(&p_cb->aid_rt_loc[entry], &p_cb->aid_rt_loc[entry + 1],
                  rest_len);
#endif
    }
    /* else the last entry, just reduce the aid_entries by 1 */
    p_cb->aid_entries--;
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    nfa_ee_update_route_aid_size(p_cb);
    nfa_ee_start_timer();
    /* report NFA_EE_REMOVE_AID_EVT to the callback associated the NFCEE */
    p_cback = p_cb->p_ee_cback;
  }
#if (NXP_EXTNS == TRUE)
  /*Clear All AIDs*/
  else if (0 == memcmp(NFA_REMOVE_ALL_AID, p_data->rm_aid.p_aid,
                       p_data->rm_aid.aid_len)) {
    uint32_t xx;
    tNFA_EE_ECB* p_cb = nfa_ee_cb.ecb;
    for (xx = 0; xx < nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED; xx++, p_cb++) {
#if (NXP_EXTNS == TRUE)
        if((nfcFL.chipType != pn547C2) &&
                (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
            memset(&p_cb->aid_cfg[0], 0x00, max_aid_config_length);
            memset(&p_cb->aid_len[0], 0x00, max_aid_entries);
            memset(&p_cb->aid_pwr_cfg[0], 0x00, max_aid_entries);
            memset(&p_cb->aid_info[0], 0x00, max_aid_entries);
            memset(&p_cb->aid_rt_info[0], 0x00, max_aid_entries);
        }
        else {
            memset(&p_cb->aid_cfg[0], 0x00, sizeof(p_cb->aid_cfg));
            memset(&p_cb->aid_len[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
            memset(&p_cb->aid_pwr_cfg[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
            memset(&p_cb->aid_rt_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        }
#else
        memset(&p_cb->aid_cfg[0], 0x00, sizeof(p_cb->aid_cfg));
        memset(&p_cb->aid_len[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        memset(&p_cb->aid_pwr_cfg[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        memset(&p_cb->aid_rt_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        memset(&p_cb->aid_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
#endif
        p_cb->aid_entries = 0;
        nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    }

    tNFA_EE_ECB* p_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];

#if (NXP_EXTNS == TRUE)
    if((nfcFL.chipType != pn547C2) &&
            (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
        memset(&p_ecb->aid_cfg[0], 0x00, max_aid_config_length);
        memset(&p_ecb->aid_len[0], 0x00, max_aid_entries);
        memset(&p_ecb->aid_pwr_cfg[0], 0x00, max_aid_entries);
        memset(&p_ecb->aid_info[0], 0x00, max_aid_entries);
        memset(&p_ecb->aid_rt_info[0], 0x00, max_aid_entries);
    }
    else {
        memset(&p_ecb->aid_cfg[0], 0x00, sizeof(p_ecb->aid_cfg));
        memset(&p_ecb->aid_len[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        memset(&p_ecb->aid_pwr_cfg[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
        memset(&p_ecb->aid_rt_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
    }
#else
    memset(&p_ecb->aid_cfg[0], 0x00, sizeof(p_ecb->aid_cfg));
    memset(&p_ecb->aid_len[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
    memset(&p_ecb->aid_pwr_cfg[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
    memset(&p_ecb->aid_rt_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
    memset(&p_ecb->aid_info[0], 0x00, NFA_EE_MAX_AID_ENTRIES);
#endif
    p_ecb->aid_entries = 0;
    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_AID;
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_ecb);
  }
#endif
  else {
    NFA_TRACE_ERROR0(
        "nfa_ee_api_remove_aid The AID entry is not in the database");
    evt_data.status = NFA_STATUS_INVALID_PARAM;
  }
  nfa_ee_report_event(p_cback, NFA_EE_REMOVE_AID_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_add_apdu
**
** Description      process add an APDU pattern data configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_add_apdu(tNFA_EE_MSG* p_data) {
  tNFA_EE_API_ADD_APDU* p_add = &p_data->add_apdu;
  tNFA_EE_ECB* p_cb = p_data->cfg_hdr.p_cb;

#if (NXP_EXTNS == TRUE)
  tNFA_EE_ECB* dh_ecb = NULL;
#endif
  tNFA_EE_ECB* p_chk_cb;
  uint8_t* p, *p_start;
  int aid_len,apdu_len, len_needed;
  tNFA_EE_CBACK_DATA evt_data = {0};
  int offset = 0, entry = 0;
  uint16_t new_size;

  nfa_ee_trace_apdu("nfa_ee_api_add_apdu", p_add);
  p_chk_cb =
      nfa_ee_find_apdu_offset(p_add->apdu_len, p_add->p_apdu, &offset, &entry);
  if(NFA_GetNCIVersion() == NCI_VERSION_1_0) {
    if((p_add->power_state & 0x08) || (p_add->power_state & 0x20)) {
      p_add->power_state &= ~0x08;
      p_add->power_state &= ~0x20;
      p_add->power_state |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
    } else if(p_add->power_state & 0x10){
      p_add->power_state &= ~0x10;
      p_add->power_state |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
    }
  }
  if (p_chk_cb) {
    NFA_TRACE_DEBUG0(
        "nfa_ee_api_add_apdu The APDU entry is already in the database");
    if (p_chk_cb == p_cb) {
      p_cb->apdu_rt_info[entry] |= NFA_EE_AE_ROUTE;
      new_size = nfa_ee_total_lmrt_size();
#if (NXP_EXTNS == TRUE)
      if (((nfcFL.chipType != pn547C2) &&
              nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN) && new_size > max_aid_config_length ||
              ((nfcFL.chipType == pn547C2) &&
                      !nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN) && (new_size > NFC_GetLmrtSize())) {
#else
      if (new_size > NFC_GetLmrtSize()) {
#endif
        NFA_TRACE_ERROR1("Exceed LMRT size:%d (add ROUTE)", new_size);
        evt_data.status = NFA_STATUS_BUFFER_FULL;
        p_cb->apdu_rt_info[entry] &= ~NFA_EE_AE_ROUTE;
      } else {
        p_cb->apdu_pwr_cfg[entry] = p_add->power_state;
      }
    } else {
      NFA_TRACE_ERROR1(
          "The APDU entry is already in the database for different NFCEE "
          "ID:0x%02x",
          p_chk_cb->nfcee_id);
      evt_data.status = NFA_STATUS_SEMANTIC_ERROR;
    }
  } else {
/* Find the total length so far */
#if (NXP_EXTNS == TRUE)
    aid_len = nfa_all_ee_find_total_aid_len();
    dh_ecb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
#else
    aid_len = nfa_ee_find_total_aid_len(p_cb, 0);
#endif
    apdu_len = nfa_all_ee_find_total_apdu_pattern_len();
    /* make sure the control block has enough room to hold this entry */
    len_needed = p_add->apdu_len + p_add->mask_len + 2 + 2; /*tag/len/apdu_mask/apdu/power_state/nfceeid*/

    if ((len_needed + aid_len + apdu_len) >
#if (NXP_EXTNS == TRUE)
    ((((nfcFL.chipType != pn547C2) &&
            nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN) && max_aid_config_length) ||
            (((nfcFL.chipType == pn547C2) &&
                    !nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN) && NFC_GetLmrtSize()))
#else
        NFC_GetLmrtSize()
#endif
        ||((dh_ecb->size_apdu + p_add->apdu_len + p_add->mask_len + 2) >= NFA_EE_TOTAL_APDU_PATTERN_SIZE)) {
#if (NXP_EXTNS == TRUE)
        if((nfcFL.chipType != pn547C2) &&
                nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN) {
            NFA_TRACE_ERROR4(
                    "Exceed capacity: (len_needed:%d + len:%d) > "
                    "max_aid_config_length:%d",
                    len_needed, aid_len,apdu_len, max_aid_config_length);
        }
        else {
            NFA_TRACE_ERROR4(
                    "Exceed capacity: (len_needed:%d + len:%d) > "
                    "NFA_EE_TOTAL_APDU_PATTERN_SIZE:%d",
                    len_needed, aid_len,apdu_len, NFA_EE_TOTAL_APDU_PATTERN_SIZE);
        }
#else
      NFA_TRACE_ERROR4(
          "Exceed capacity: (len_needed:%d + len:%d) > "
          "NFA_EE_TOTAL_APDU_PATTERN_SIZE:%d",
          len_needed, aid_len,apdu_len, NFA_EE_TOTAL_APDU_PATTERN_SIZE);
#endif
      evt_data.status = NFA_STATUS_BUFFER_FULL;
    }
#if (NXP_EXTNS == TRUE)
    else if (dh_ecb->apdu_pattern_entries < NFA_EE_MAX_APDU_PATTERN_ENTRIES)
#else
    else if (p_cb->apdu_pattern_entries < NFA_EE_MAX_APDU_PATTERN_ENTRIES)
#endif
    {
/* add APDU */
#if (NXP_EXTNS == TRUE)
        apdu_len = nfa_ee_find_total_apdu_len(dh_ecb, 0);
        // Always use single apdu_cfg buffer to keep the apdu order intact.
        dh_ecb->apdu_pwr_cfg[dh_ecb->apdu_pattern_entries] = p_add->power_state;
        dh_ecb->apdu_rt_info[dh_ecb->apdu_pattern_entries] = NFA_EE_AE_ROUTE;
        dh_ecb->apdu_rt_loc[dh_ecb->apdu_pattern_entries] = p_cb->nfcee_id;
        p = dh_ecb->apdu_cfg + apdu_len;
#else
        p_cb->apdu_pwr_cfg[p_cb->apdu_pattern_entries] = p_add->power_state;
        p_cb->apdu_rt_info[p_cb->apdu_pattern_entries] = NFA_EE_AE_ROUTE;
        p = p_cb->apdu_cfg + apdu_len;
#endif

        p_start = p;
        *p++ = NFA_EE_AID_CFG_TAG_NAME;
        *p++ = p_add->apdu_len + p_add->mask_len;
        memcpy(p, p_add->p_apdu, p_add->apdu_len);
        p += p_add->apdu_len;
        memcpy(p, p_add->p_mask, p_add->mask_len);
        p += p_add->mask_len;

#if (NXP_EXTNS == TRUE)
        dh_ecb->apdu_len[dh_ecb->apdu_pattern_entries++] = (uint8_t)(p - p_start);
#else
        p_cb->apdu_len[p_cb->apdu_pattern_entries++] = (uint8_t)(p - p_start);
#endif
    }
    else
    {
       NFA_TRACE_ERROR1("Exceed NFA_EE_MAX_APDU_PATTERN_ENTRIES:%d", NFA_EE_MAX_APDU_PATTERN_ENTRIES);
       evt_data.status = NFA_STATUS_BUFFER_FULL;
    }
  }

  if (evt_data.status == NFA_STATUS_OK) {
    /* mark APDU changed */
    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_APDU;
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    nfa_ee_update_route_apdu_size(p_cb);
    nfa_ee_start_timer();
  }
  NFA_TRACE_DEBUG2("status:%d ee_cfged:0x%02x ", evt_data.status,
                   nfa_ee_cb.ee_cfged);
  /* report the status of this operation */
  nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_ADD_APDU_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_remove_apdu
**
** Description      process remove an APDU routing configuration from user
**                  start a 1 second timer. When the timer expires,
**                  the configuration collected in control block is sent to NFCC
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_remove_apdu(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_CBACK_DATA evt_data = {0};
  int offset = 0, entry = 0, len;
  int rest_len;
  tNFA_EE_CBACK* p_cback = NULL;

  p_cb = nfa_ee_find_apdu_offset(p_data->rm_apdu.apdu_len, p_data->rm_apdu.p_apdu,
                                    &offset, &entry);
  if (p_cb && p_cb->apdu_pattern_entries) {
    NFA_TRACE_DEBUG2("apdu_rt_info[%d]: 0x%02x", entry,
                         p_cb->apdu_rt_info[entry]);
   /* mark routing and VS changed */
    if (p_cb->apdu_rt_info[entry] & NFA_EE_AE_ROUTE)
      p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_APDU;

    if (p_cb->apdu_rt_info[entry] & NFA_EE_AE_VS)
      p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_VS;

    /* remove the apdu */
    if ((entry + 1) < p_cb->apdu_pattern_entries) {
      /* not the last entry, move the apdu entries in control block */
      /* Find the total len from the next entry to the last one */
      rest_len = nfa_ee_find_total_apdu_len(p_cb, entry + 1);

      len = p_cb->apdu_len[entry];
      NFA_TRACE_DEBUG2("nfa_ee_api_remove_apdu len:%d, rest_len:%d", len,
                       rest_len);
      GKI_shiftup(&p_cb->apdu_cfg[offset], &p_cb->apdu_cfg[offset + len],
                 rest_len);
      rest_len = p_cb->apdu_pattern_entries - entry;
      GKI_shiftup(&p_cb->apdu_len[entry], &p_cb->apdu_len[entry + 1], rest_len);
      GKI_shiftup(&p_cb->apdu_pwr_cfg[entry], &p_cb->apdu_pwr_cfg[entry + 1],
                 rest_len);
      GKI_shiftup(&p_cb->apdu_rt_info[entry], &p_cb->apdu_rt_info[entry + 1],
                      rest_len);
      GKI_shiftup(&p_cb->apdu_rt_loc[entry], &p_cb->apdu_rt_loc[entry + 1],
                      rest_len);
    }
    /* else the last entry, just reduce the apdu_entries by 1 */
    p_cb->apdu_pattern_entries--;
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
    nfa_ee_update_route_apdu_size(p_cb);
    nfa_ee_start_timer();
   /* report NFA_EE_REMOVE_APDU_EVT to the callback associated the NFCEE */
    p_cback = p_cb->p_ee_cback;
  }
#if (NXP_EXTNS == TRUE)
      /*Clear All APDUs*/
    else if (0 == memcmp(NFA_REMOVE_ALL_APDU, p_data->rm_apdu.p_apdu,
                           p_data->rm_apdu.apdu_len)) {
      uint32_t xx;
      for (xx = 0; xx < NFA_EE_NUM_ECBS; xx++) {
        memset(&nfa_ee_cb.ecb[xx].apdu_cfg[0], 0x00, sizeof(nfa_ee_cb.ecb[xx].apdu_cfg));
        memset(&nfa_ee_cb.ecb[xx].apdu_len[0], 0x00, NFA_EE_MAX_APDU_PATTERN_ENTRIES);
        memset(&nfa_ee_cb.ecb[xx].apdu_pwr_cfg[0], 0x00, NFA_EE_MAX_APDU_PATTERN_ENTRIES);
        memset(&nfa_ee_cb.ecb[xx].apdu_rt_info[0], 0x00, NFA_EE_MAX_APDU_PATTERN_ENTRIES);
        memset(&nfa_ee_cb.ecb[xx].apdu_rt_loc[0], 0x00, NFA_EE_MAX_APDU_PATTERN_ENTRIES);
        nfa_ee_cb.ecb[xx].apdu_pattern_entries = 0;
        nfa_ee_cb.ecb[xx].ecb_flags |= NFA_EE_ECB_FLAGS_APDU;
        nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(&nfa_ee_cb.ecb[xx]);
      }
    }
#endif
    else {
      NFA_TRACE_ERROR0(
          "nfa_ee_api_remove_apdu The APDU entry is not in the database");
      evt_data.status = NFA_STATUS_INVALID_PARAM;
    }
    nfa_ee_report_event(p_cback, NFA_EE_REMOVE_APDU_EVT, &evt_data);
  }

/*******************************************************************************
**
** Function         nfa_ee_api_lmrt_size
**
** Description      Reports the remaining size in the Listen Mode Routing Table
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_lmrt_size(tNFA_EE_MSG* p_data) {
  tNFA_EE_CBACK_DATA evt_data = {0};
  uint16_t total_size = NFC_GetLmrtSize();
  (void)p_data;
  evt_data.size = total_size - nfa_ee_total_lmrt_size();
  NFA_TRACE_DEBUG2("nfa_ee_api_lmrt_size total size:%d remaining size:%d",
                   total_size, evt_data.size);

  nfa_ee_report_event(NULL, NFA_EE_REMAINING_SIZE_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_update_now
**
** Description      Initiates connection creation process to the given NFCEE
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_update_now(tNFA_EE_MSG* p_data) {
  tNFA_EE_CBACK_DATA evt_data;

  if (nfa_ee_cb.ee_wait_evt & NFA_EE_WAIT_UPDATE_ALL) {
    NFA_TRACE_ERROR2(
        "nfa_ee_api_update_now still waiting for update complete "
        "ee_wait_evt:0x%x wait_rsp:%d",
        nfa_ee_cb.ee_wait_evt, nfa_ee_cb.wait_rsp);
    evt_data.status = NFA_STATUS_SEMANTIC_ERROR;
    nfa_ee_report_event(NULL, NFA_EE_UPDATED_EVT, &evt_data);
    return;
  }
  nfa_sys_stop_timer(&nfa_ee_cb.timer);
  nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_UPDATE_NOW;
  nfa_ee_rout_timeout(p_data);
}

/*******************************************************************************
**
** Function         nfa_ee_api_connect
**
** Description      Initiates connection creation process to the given NFCEE
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_connect(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->connect.p_cb;
  int xx;
  tNFA_EE_CBACK_DATA evt_data = {0};

  evt_data.connect.status = NFA_STATUS_FAILED;
  if (p_cb->conn_st == NFA_EE_CONN_ST_NONE) {
    for (xx = 0; xx < p_cb->num_interface; xx++) {
      if (p_data->connect.ee_interface == p_cb->ee_interface[xx]) {
          nfa_ee_get_num_nfcee_configured(nfa_ee_read_num_nfcee_config_cb);
          p_cb->p_ee_cback = p_data->connect.p_cback;
        p_cb->conn_st = NFA_EE_CONN_ST_WAIT;
        p_cb->use_interface = p_data->connect.ee_interface;
        evt_data.connect.status =
            NFC_ConnCreate(NCI_DEST_TYPE_NFCEE, p_data->connect.nfcee_id,
#if (NXP_EXTNS == TRUE)
                           p_data->connect.ee_interface, nfa_hci_conn_cback);
#else
                           p_data->connect.ee_interface, nfa_ee_conn_cback);
#endif
        /* report the NFA_EE_CONNECT_EVT status on the response from NFCC */
        break;
      }
    }
  }

  if (evt_data.connect.status != NCI_STATUS_OK) {
    evt_data.connect.ee_handle =
        (tNFA_HANDLE)p_data->connect.nfcee_id | NFA_HANDLE_GROUP_EE;
    evt_data.connect.status = NFA_STATUS_INVALID_PARAM;
    evt_data.connect.ee_interface = p_data->connect.ee_interface;
    nfa_ee_report_event(p_data->connect.p_cback, NFA_EE_CONNECT_EVT, &evt_data);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_api_send_data
**
** Description      Send the given data packet to the given NFCEE
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_send_data(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->send_data.p_cb;
  NFC_HDR* p_pkt;
  uint16_t size = NCI_MSG_OFFSET_SIZE + NCI_DATA_HDR_SIZE +
                  p_data->send_data.data_len + NFC_HDR_SIZE;
  uint8_t* p;
  tNFA_STATUS status = NFA_STATUS_FAILED;

  if (p_cb->conn_st == NFA_EE_CONN_ST_CONN) {
    p_pkt = (NFC_HDR*)GKI_getbuf(size);
    if (p_pkt) {
      p_pkt->offset = NCI_MSG_OFFSET_SIZE + NCI_DATA_HDR_SIZE;
      p_pkt->len = p_data->send_data.data_len;
      p = (uint8_t*)(p_pkt + 1) + p_pkt->offset;
      memcpy(p, p_data->send_data.p_data, p_pkt->len);
      NFC_SendData(p_cb->conn_id, p_pkt);
    } else {
      nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_NO_MEM_ERR_EVT,
                          (void*)&status);
    }
  } else {
    nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_NO_CB_ERR_EVT, (void*)&status);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_api_disconnect
**
** Description      Initiates closing of the connection to the given NFCEE
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_api_disconnect(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb = p_data->disconnect.p_cb;
  tNFA_EE_CBACK_DATA evt_data = {0};

#if(NXP_EXTNS == TRUE)
  nfa_ee_cb.ee_flags |= NFA_EE_HCI_CONN_CLOSE;
#endif

  if (p_cb->conn_st == NFA_EE_CONN_ST_CONN) {
    p_cb->conn_st = NFA_EE_CONN_ST_DISC;
    NFC_ConnClose(p_cb->conn_id);
  }
  evt_data.handle = (tNFA_HANDLE)p_cb->nfcee_id | NFA_HANDLE_GROUP_EE;
  nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_DISCONNECT_EVT, &evt_data);
}

/*******************************************************************************
**
** Function         nfa_ee_report_disc_done
**
** Description      Process the callback for NFCEE discovery response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_report_disc_done(bool notify_enable_done) {
  tNFA_EE_CBACK* p_cback;
  tNFA_EE_CBACK_DATA evt_data = {0};

  NFA_TRACE_DEBUG3(
      "nfa_ee_report_disc_done() em_state:%d num_ee_expecting:%d "
      "notify_enable_done:%d",
      nfa_ee_cb.em_state, nfa_ee_cb.num_ee_expecting, notify_enable_done);
  if (nfa_ee_cb.num_ee_expecting == 0) {
    if (notify_enable_done) {
      if (nfa_ee_cb.em_state == NFA_EE_EM_STATE_INIT_DONE) {
        if (nfa_ee_cb.discv_timer.in_use && NFA_GetNCIVersion() == NCI_VERSION_2_0) {
          nfa_sys_stop_timer(&nfa_ee_cb.discv_timer);
        }
        nfa_sys_cback_notify_enable_complete(NFA_ID_EE);
        if (nfa_ee_cb.p_enable_cback)
          (*nfa_ee_cb.p_enable_cback)(NFA_EE_DISC_STS_ON);
      } else if ((nfa_ee_cb.em_state == NFA_EE_EM_STATE_RESTORING) &&
                 (nfa_ee_cb.ee_flags & NFA_EE_FLAG_NOTIFY_HCI)) {
        nfa_ee_cb.ee_flags &= ~NFA_EE_FLAG_NOTIFY_HCI;
        if (nfa_ee_cb.p_enable_cback)
          (*nfa_ee_cb.p_enable_cback)(NFA_EE_DISC_STS_ON);
      }
    }

    if (nfa_ee_cb.p_ee_disc_cback) {
      /* notify API callback */
      p_cback = nfa_ee_cb.p_ee_disc_cback;
      nfa_ee_cb.p_ee_disc_cback = NULL;
      evt_data.status = NFA_STATUS_OK;
      evt_data.ee_discover.num_ee = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
      NFA_EeGetInfo(&evt_data.ee_discover.num_ee, evt_data.ee_discover.ee_info);
      nfa_ee_report_event(p_cback, NFA_EE_DISCOVER_EVT, &evt_data);
    }
#if (NXP_EXTNS == TRUE)
    else if(nfcFL.nfccFL._NFCEE_REMOVED_NTF_RECOVERY == true) {
      evt_data.status = NFA_STATUS_OK;
      evt_data.ee_discover.num_ee = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
      NFA_AllEeGetInfo(&evt_data.ee_discover.num_ee,
                       evt_data.ee_discover.ee_info);
      nfa_ee_report_event(NULL, NFA_EE_DISCOVER_EVT, &evt_data);
    }
    if (nfa_ee_cb.p_enable_cback)
                (*nfa_ee_cb.p_enable_cback) (NFA_EE_MODE_SET_COMPLETE);
#endif
  }
}

/*******************************************************************************
**
** Function         nfa_ee_restore_ntf_done
**
** Description      check if any ee_status still has NFA_EE_STATUS_PENDING bit
**
** Returns          true, if all NFA_EE_STATUS_PENDING bits are removed
**
*******************************************************************************/
bool nfa_ee_restore_ntf_done(void) {
  tNFA_EE_ECB* p_cb;
  bool is_done = true;
  int xx;

  p_cb = nfa_ee_cb.ecb;
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if ((p_cb->nfcee_id != NFA_EE_INVALID) &&
        (p_cb->ee_old_status & NFA_EE_STATUS_RESTORING)) {
      is_done = false;
      break;
    }
  }
  return is_done;
}

/*******************************************************************************
**
** Function         nfa_ee_remove_pending
**
** Description      check if any ee_status still has NFA_EE_STATUS_RESTORING bit
**
** Returns          true, if all NFA_EE_STATUS_RESTORING bits are removed
**
*******************************************************************************/
static void nfa_ee_remove_pending(void) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_ECB* p_cb_n, *p_cb_end;
  int xx, num_removed = 0;
  int first_removed = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;

  p_cb = nfa_ee_cb.ecb;
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if ((p_cb->nfcee_id != NFA_EE_INVALID) &&
        (p_cb->ee_status & NFA_EE_STATUS_RESTORING)) {
      p_cb->nfcee_id = NFA_EE_INVALID;
      num_removed++;
      if (first_removed == nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED) first_removed = xx;
    }
  }

  NFA_TRACE_DEBUG3(
      "nfa_ee_remove_pending() cur_ee:%d, num_removed:%d first_removed:%d",
      nfa_ee_cb.cur_ee, num_removed, first_removed);
  if (num_removed && (first_removed != (nfa_ee_cb.cur_ee - num_removed))) {
    /* if the removes ECB entried are not at the end, move the entries up */
    p_cb_end = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee - 1];
    p_cb = &nfa_ee_cb.ecb[first_removed];
    for (p_cb_n = p_cb + 1; p_cb_n <= p_cb_end;) {
      while ((p_cb_n->nfcee_id == NFA_EE_INVALID) && (p_cb_n <= p_cb_end)) {
        p_cb_n++;
      }

      if (p_cb_n <= p_cb_end) {
        memcpy(p_cb, p_cb_n, sizeof(tNFA_EE_ECB));
        p_cb_n->nfcee_id = NFA_EE_INVALID;
      }
      p_cb++;
      p_cb_n++;
    }
  }
  nfa_ee_cb.cur_ee -= (uint8_t)num_removed;
}

/*******************************************************************************
**
** Function         nfa_ee_nci_disc_rsp
**
** Description      Process the callback for NFCEE discovery response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_disc_rsp(tNFA_EE_MSG* p_data) {
  tNFC_NFCEE_DISCOVER_REVT* p_evt = p_data->disc_rsp.p_data;
  tNFA_EE_ECB* p_cb;
  uint8_t xx;
  uint8_t num_nfcee = p_evt->num_nfcee;
  bool notify_enable_done = false;

  NFA_TRACE_DEBUG3("nfa_ee_nci_disc_rsp() em_state:%d cur_ee:%d, num_nfcee:%d",
                   nfa_ee_cb.em_state, nfa_ee_cb.cur_ee, num_nfcee);
  switch (nfa_ee_cb.em_state) {
    case NFA_EE_EM_STATE_INIT:
      nfa_ee_cb.cur_ee = 0;
      nfa_ee_cb.num_ee_expecting = 0;
      if (num_nfcee == 0) {
        nfa_ee_cb.em_state = NFA_EE_EM_STATE_INIT_DONE;
        notify_enable_done = true;
        if (p_evt->status != NFC_STATUS_OK) {
          nfa_sys_stop_timer(&nfa_ee_cb.discv_timer);
        }
      }
      break;

    case NFA_EE_EM_STATE_INIT_DONE:
      if (num_nfcee) {
        /* if this is initiated by api function,
         * check if the number of NFCEE expected is more than what's currently
         * in CB */
        if (num_nfcee > nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED)
          num_nfcee = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
        if (nfa_ee_cb.cur_ee < num_nfcee) {
          p_cb = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee];
          for (xx = nfa_ee_cb.cur_ee; xx < num_nfcee; xx++, p_cb++) {
            /* mark the new entries as a new one */
            p_cb->nfcee_id = NFA_EE_INVALID;
          }
        }
      }
      break;

    case NFA_EE_EM_STATE_RESTORING:
      if (num_nfcee == 0) {
        nfa_ee_cb.em_state = NFA_EE_EM_STATE_INIT_DONE;
        nfa_ee_remove_pending();
        nfa_ee_check_restore_complete();
        if (p_evt->status != NFC_STATUS_OK) {
          nfa_sys_stop_timer(&nfa_ee_cb.discv_timer);
        }
      }
      break;
  }

  if (p_evt->status == NFC_STATUS_OK) {
    nfa_ee_cb.num_ee_expecting = p_evt->num_nfcee;
    if (nfa_ee_cb.num_ee_expecting > nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED) {
      NFA_TRACE_ERROR2("NFA-EE num_ee_expecting:%d > max:%d",
                       nfa_ee_cb.num_ee_expecting, nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED);
    }
  }
  nfa_ee_report_disc_done(notify_enable_done);
  NFA_TRACE_DEBUG3(
      "nfa_ee_nci_disc_rsp() em_state:%d cur_ee:%d num_ee_expecting:%d",
      nfa_ee_cb.em_state, nfa_ee_cb.cur_ee, nfa_ee_cb.num_ee_expecting);
}

/*******************************************************************************
**
** Function         nfa_ee_nci_disc_ntf
**
** Description      Process the callback for NFCEE discovery notification
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_disc_ntf(tNFA_EE_MSG* p_data) {
  tNFC_NFCEE_INFO_REVT* p_ee = p_data->disc_ntf.p_data;
  tNFA_EE_ECB* p_cb = NULL;
  bool notify_enable_done = false;
  bool notify_new_ee = false;
  tNFA_EE_CBACK_DATA evt_data = {0};
  tNFA_EE_INFO* p_info;
  tNFA_EE_EM_STATE new_em_state = NFA_EE_EM_STATE_MAX;

  NFA_TRACE_DEBUG5(
      "nfa_ee_nci_disc_ntf() em_state:%d ee_flags:0x%x cur_ee:%d "
      "num_ee_expecting:%d, nfa_ee_max_ee_cfg : %d",
      nfa_ee_cb.em_state, nfa_ee_cb.ee_flags, nfa_ee_cb.cur_ee,
      nfa_ee_cb.num_ee_expecting, nfa_ee_max_ee_cfg);
  if (nfa_ee_cb.num_ee_expecting) {
    nfa_ee_cb.num_ee_expecting--;
    if ((nfa_ee_cb.num_ee_expecting == 0) &&
        (nfa_ee_cb.p_ee_disc_cback != NULL)) {
      /* Discovery triggered by API function */
      if(NFA_GetNCIVersion() != NCI_VERSION_2_0)
          NFC_NfceeDiscover(false);
    }
  }
  switch (nfa_ee_cb.em_state) {
    case NFA_EE_EM_STATE_INIT:
      if (nfa_ee_cb.cur_ee < nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED) {
        /* the cb can collect up to NFA_EE_MAX_EE_SUPPORTED ee_info */
        p_cb = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee++];
      }

      if (nfa_ee_cb.num_ee_expecting == 0) {
        /* notify init_done callback */
        nfa_ee_cb.em_state = NFA_EE_EM_STATE_INIT_DONE;
        notify_enable_done = true;
      }
      break;

    case NFA_EE_EM_STATE_INIT_DONE:
      p_cb = nfa_ee_find_ecb(p_ee->nfcee_id);
      if (p_cb == NULL) {
        /* the NFCEE ID is not in the last NFCEE discovery
         * maybe it's a new one */
        p_cb = nfa_ee_find_ecb(NFA_EE_INVALID);
        if (p_cb) {
          nfa_ee_cb.cur_ee++;
          notify_new_ee = true;
        }
      } else if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_ORDER) {
        nfa_ee_cb.cur_ee++;
        notify_new_ee = true;
      } else {
        NFA_TRACE_DEBUG3("cur_ee:%d ecb_flags=0x%02x  ee_status=0x%x",
                         nfa_ee_cb.cur_ee, p_cb->ecb_flags, p_cb->ee_status);
      }
      break;

    case NFA_EE_EM_STATE_RESTORING:
      p_cb = nfa_ee_find_ecb(p_ee->nfcee_id);
      if (p_cb == NULL) {
        /* the NFCEE ID is not in the last NFCEE discovery
         * maybe it's a new one */
        p_cb = nfa_ee_find_ecb(NFA_EE_INVALID);
        if (p_cb) {
          nfa_ee_cb.cur_ee++;
          notify_new_ee = true;
        }
      }
      if (nfa_ee_cb.num_ee_expecting == 0) {
        /* notify init_done callback */
        notify_enable_done = true;
        if (nfa_ee_restore_ntf_done()) {
          new_em_state = NFA_EE_EM_STATE_INIT_DONE;
        }
      }
      break;
  }
  NFA_TRACE_DEBUG1("nfa_ee_nci_disc_ntf cur_ee:%d", nfa_ee_cb.cur_ee);

  if (p_cb) {
    p_cb->nfcee_id = p_ee->nfcee_id;
    p_cb->ee_status = p_ee->ee_status;
    p_cb->num_interface = p_ee->num_interface;
    memcpy(p_cb->ee_interface, p_ee->ee_interface, p_ee->num_interface);
    p_cb->num_tlvs = p_ee->num_tlvs;
    memcpy(p_cb->ee_tlv, p_ee->ee_tlv, p_ee->num_tlvs * sizeof(tNFA_EE_TLV));


    p_cb->ee_power_supply_status = p_ee->nfcee_power_ctrl;
    if (nfa_ee_cb.em_state == NFA_EE_EM_STATE_RESTORING) {
      /* NCI spec says: An NFCEE_DISCOVER_NTF that contains a Protocol type of
       * "HCI Access"
       * SHALL NOT contain any other additional Protocol
       * i.e. check only first supported NFCEE interface is HCI access */
      /* NFA_HCI module handles restoring configurations for HCI access */
      if (p_cb->ee_interface[0] != NFC_NFCEE_INTERFACE_HCI_ACCESS) {
        if ((nfa_ee_cb.ee_flags & NFA_EE_FLAG_WAIT_HCI) == 0) {
          nfa_ee_restore_one_ecb(p_cb);
        }
        /* else wait for NFA-HCI module to restore the HCI network information
         * before enabling the NFCEE */
      }
    }
    NFA_TRACE_DEBUG4("nfa_ee_cb.p_ee_disc_cback : %d notify_new_ee :%d "
    "nfa_dm_is_active() : %d p_cb->ee_status : %d", nfa_ee_cb.p_ee_disc_cback, notify_new_ee,
     nfa_dm_is_active(), p_cb->ee_status);

    if ((nfa_ee_cb.p_ee_disc_cback == NULL) && (notify_new_ee == true)) {
      if (nfa_dm_is_active() && (p_cb->ee_status != NFA_EE_STATUS_REMOVED)) {
        /* report this NFA_EE_NEW_EE_EVT only after NFA_DM_ENABLE_EVT is
         * reported */
        p_info = &evt_data.new_ee;
        p_info->ee_handle = NFA_HANDLE_GROUP_EE | (tNFA_HANDLE)p_cb->nfcee_id;
        p_info->ee_status = p_cb->ee_status;
        p_info->num_interface = p_cb->num_interface;
        p_info->num_tlvs = p_cb->num_tlvs;
        memcpy(p_info->ee_interface, p_cb->ee_interface, p_cb->num_interface);
        memcpy(p_info->ee_tlv, p_cb->ee_tlv,
               p_cb->num_tlvs * sizeof(tNFA_EE_TLV));
        if(NFA_GetNCIVersion() == NCI_VERSION_2_0)
          p_info->ee_power_supply_status = p_cb->ee_power_supply_status;
        nfa_ee_report_event(NULL, NFA_EE_NEW_EE_EVT, &evt_data);
      }

#if (NXP_EXTNS == TRUE)
      if (nfa_dm_is_active()) {
        evt_data.status = p_cb->ee_status;
        evt_data.ee_discover.num_ee = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
        NFA_EeGetInfo(&evt_data.ee_discover.num_ee,
                      evt_data.ee_discover.ee_info);
        nfa_ee_report_event(NULL, NFA_EE_DISCOVER_EVT, &evt_data);
      }
#endif
    } else
      nfa_ee_report_disc_done(notify_enable_done);

    if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_ORDER) {
      NFA_TRACE_DEBUG0("NFA_EE_ECB_FLAGS_ORDER");
      p_cb->ecb_flags &= ~NFA_EE_ECB_FLAGS_ORDER;
      nfa_ee_report_discover_req_evt();
    }
  }

  if (new_em_state != NFA_EE_EM_STATE_MAX) {
    nfa_ee_cb.em_state = new_em_state;
    nfa_ee_check_restore_complete();
  }

  if ((nfa_ee_cb.cur_ee == nfa_ee_max_ee_cfg) &&
      (nfa_ee_cb.em_state == NFA_EE_EM_STATE_INIT_DONE)) {
    if (nfa_ee_cb.discv_timer.in_use) {
      nfa_sys_stop_timer(&nfa_ee_cb.discv_timer);
      p_data->hdr.event = NFA_EE_DISCV_TIMEOUT_EVT;
      NFA_TRACE_DEBUG0("ee_disc_timeout");
      nfa_ee_evt_hdlr((NFC_HDR*)p_data);
    }
  }
  NFA_TRACE_DEBUG0("after nfa_ee_nci_disc_ntf");
#if (NXP_EXTNS == TRUE)
  if (p_cb) {
    if ((nfa_hci_cb.hci_state == NFA_HCI_STATE_WAIT_NETWK_ENABLE) &&
        (p_cb->ee_status != NFA_EE_STATUS_ACTIVE)) {
      NFA_TRACE_DEBUG2(
          "nfa_ee_nci_disc_ntf hci_state : 0x%02x  ee_status : 0x%02x",
          nfa_hci_cb.hci_state, p_cb->ee_status);
      nfa_sys_stop_timer(&nfa_hci_cb.timer);
      nfa_sys_start_timer(&nfa_hci_cb.timer, NFA_HCI_RSP_TIMEOUT_EVT, 150);
    }
  }
#endif
  NFA_TRACE_DEBUG0("nfa_ee_nci_disc_ntf last");
}

/*******************************************************************************
**
** Function         nfa_ee_nci_nfcee_status_ntf
**
** Description      Process the callback for NFCEE status notification
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_nfcee_status_ntf(tNFA_EE_MSG* p_data) {
  tNFC_NFCEE_STATUS_REVT* p_ee = p_data->nfcee_status_ntf.p_data;

  NFA_TRACE_DEBUG3(
      "nfa_ee_nci_nfcee_status_ntf() em_state:%d, nfcee_id:%d nfcee_status:%d",
      nfa_ee_cb.em_state, p_ee->nfcee_id, p_ee->nfcee_status);
  tNFA_EE_ECB* p_cb = nfa_ee_find_ecb(p_ee->nfcee_id);
  if(p_ee->nfcee_status == NFC_NFCEE_STS_UNRECOVERABLE_ERROR) {
    if(p_cb != NULL) {
        if (nfa_ee_cb.p_enable_cback)
                    (*nfa_ee_cb.p_enable_cback) (NFA_EE_RECOVERY);
    }
  }
}

/*******************************************************************************
**
** Function         nfa_ee_check_restore_complete
**
** Description      Check if restore the NFA-EE related configuration to the
**                  state prior to low power mode is complete.
**                  If complete, notify sys.
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_check_restore_complete(void) {
  uint32_t xx;
  tNFA_EE_ECB* p_cb;
  bool proc_complete = true;

  p_cb = nfa_ee_cb.ecb;
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_RESTORE) {
      /* NFA_HCI module handles restoring configurations for HCI access.
       * ignore the restoring status for HCI Access */
      if (p_cb->ee_interface[0] != NFC_NFCEE_INTERFACE_HCI_ACCESS) {
        proc_complete = false;
        break;
      }
    }
  }

  NFA_TRACE_DEBUG2(
      "nfa_ee_check_restore_complete nfa_ee_cb.ee_cfg_sts:0x%02x "
      "proc_complete:%d",
      nfa_ee_cb.ee_cfg_sts, proc_complete);
  if (proc_complete) {
    /* update routing table when NFA_EE_ROUT_TIMEOUT_EVT is received */
    if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_PREV_ROUTING)
      nfa_ee_api_update_now(NULL);

    nfa_ee_cb.em_state = NFA_EE_EM_STATE_INIT_DONE;
    nfa_sys_cback_notify_nfcc_power_mode_proc_complete(NFA_ID_EE);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_build_discover_req_evt
**
** Description      Build NFA_EE_DISCOVER_REQ_EVT for all active NFCEE
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_build_discover_req_evt(tNFA_EE_DISCOVER_REQ* p_evt_data) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_DISCOVER_INFO* p_info;
  uint8_t xx;

  if (!p_evt_data) return;

  p_evt_data->num_ee = 0;
  p_cb = nfa_ee_cb.ecb;
  p_info = p_evt_data->ee_disc_info;

  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if ((p_cb->ee_status & NFA_EE_STATUS_INT_MASK) ||
        (p_cb->ee_status != NFA_EE_STATUS_ACTIVE) ||
        ((p_cb->ecb_flags & NFA_EE_ECB_FLAGS_DISC_REQ) == 0)) {
      continue;
    }
    p_info->ee_handle = (tNFA_HANDLE)p_cb->nfcee_id | NFA_HANDLE_GROUP_EE;
    p_info->la_protocol = p_cb->la_protocol;
    p_info->lb_protocol = p_cb->lb_protocol;
    p_info->lf_protocol = p_cb->lf_protocol;
    p_info->lbp_protocol = p_cb->lbp_protocol;
#if (NXP_EXTNS == TRUE)
    // code to handle and store Reader type(A/B) requested for Reader over SWP.
    /*Reader over SWP*/
    p_info->pa_protocol = p_cb->pa_protocol;
    p_info->pb_protocol = p_cb->pb_protocol;
    p_info->ee_req_op = p_cb->ee_req_op;
#endif
    p_evt_data->num_ee++;
    p_info++;

    NFA_TRACE_DEBUG6(
        "[%d] ee_handle:0x%x, listen protocol A:%d, B:%d, F:%d, BP:%d",
        p_evt_data->num_ee, p_cb->nfcee_id, p_cb->la_protocol,
        p_cb->lb_protocol, p_cb->lf_protocol, p_cb->lbp_protocol);
  }

  p_evt_data->status = NFA_STATUS_OK;
}

/*******************************************************************************
**
** Function         nfa_ee_report_discover_req_evt
**
** Description      Report NFA_EE_DISCOVER_REQ_EVT for all active NFCEE
**
** Returns          void
**
*******************************************************************************/
static void nfa_ee_report_discover_req_evt(void) {
  tNFA_EE_DISCOVER_REQ evt_data;

  if (nfa_ee_cb.p_enable_cback)
    (*nfa_ee_cb.p_enable_cback)(NFA_EE_DISC_STS_REQ);

  /* if this is restoring NFCC */
  if (!nfa_dm_is_active()) {
    NFA_TRACE_DEBUG0("nfa_ee_report_discover_req_evt DM is not active");
    return;
  }

  nfa_ee_build_discover_req_evt(&evt_data);
  nfa_ee_report_event(NULL, NFA_EE_DISCOVER_REQ_EVT, (void*)&evt_data);
}
#if (NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function         nfa_ee_nci_pwr_link_ctrl_rsp
**
** Description      Process the result for NFCEE PWR and link ctrl response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_pwr_link_ctrl_rsp(tNFA_EE_MSG* p_data) {
    NFA_TRACE_DEBUG0(" nfa_ee_nci_pwr_link_ctrl_rsp()");
    if(nfcFL.eseFL._WIRED_MODE_STANDBY != true) {
        NFA_TRACE_DEBUG0(" WIRED_MODE_STANDBY() not available. Returning");
        return;
    }
    tNFA_EE_ECB* p_cb;
    tNFA_EE_PWR_LNK_CTRL pwr_lnk_ctrl;
    tNFC_NFCEE_EE_PWR_LNK_REVT* p_rsp = p_data->pwr_lnk_ctrl_rsp.p_data;
    pwr_lnk_ctrl.status = p_rsp->status;
    NFA_TRACE_DEBUG1(" nfa_ee_nci_pwr_link_ctrl_rsp: status = %d ",
            pwr_lnk_ctrl.status);
    nfa_ee_report_event(NULL, NFA_EE_PWR_LINK_CTRL_EVT,
            (tNFA_EE_CBACK_DATA*)&pwr_lnk_ctrl);
}
/*******************************************************************************
**
** Function         nfa_ee_nci_set_mode_info
**
** Description      Process the result for NFCEE PWR and link ctrl response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_set_mode_info(tNFA_EE_MSG* p_data) {
  tNFA_EE_SET_MODE_INFO ee_set_mode_info;
  tNFC_NFCEE_MODE_SET_INFO* p_rsp = p_data->mode_set_info.p_data;
  ee_set_mode_info.status = p_rsp->status;
  ee_set_mode_info.nfcee_id = p_rsp->nfcee_id;
  NFA_TRACE_DEBUG1(" nfa_ee_nci_set_mode_info: status = %d ",
                   ee_set_mode_info.status);
  if (nfa_ee_cb.p_enable_cback)
    (*nfa_ee_cb.p_enable_cback)(NFA_EE_MODE_SET_NTF);
  nfa_ee_report_event(NULL, NFA_EE_SET_MODE_INFO_EVT,
                      (tNFA_EE_CBACK_DATA*)&ee_set_mode_info);
}

#endif
/*******************************************************************************
**
** Function         nfa_ee_nci_mode_set_rsp
**
** Description      Process the result for NFCEE ModeSet response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_mode_set_rsp(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_MODE_SET mode_set;
  tNFC_NFCEE_MODE_SET_REVT* p_rsp = p_data->mode_set_rsp.p_data;

  NFA_TRACE_DEBUG3("nfa_ee_nci_mode_set_rsp() handle:0x%02x mode:%d statu : %d",
                   p_rsp->nfcee_id, p_rsp->mode, p_rsp->status);
  p_cb = nfa_ee_find_ecb(p_rsp->nfcee_id);

  if (p_cb == NULL) {
    NFA_TRACE_ERROR1(
        "nfa_ee_nci_mode_set_rsp() Can not find cb for handle:0x%02x",
        p_rsp->nfcee_id);
    return;
  }
#if (NXP_EXTNS == TRUE)
/* Do not update routing table on secure element enable/disable. */
#else
  /* update routing table and vs on mode change */
  nfa_ee_start_timer();
#endif
  if (p_rsp->status == NFA_STATUS_OK) {
    if (p_rsp->mode == NFA_EE_MD_ACTIVATE) {
#if (NXP_EXTNS == TRUE)
      if (p_cb->ee_status != NFC_NFCEE_STATUS_REMOVED)
#endif
        p_cb->ee_status = NFC_NFCEE_STATUS_ACTIVE;
    } else {
#if (NXP_EXTNS == TRUE)
/* Do not update routing table on secure element enable/disable. */
#else
      if (p_cb->tech_switch_on | p_cb->tech_switch_off |
          p_cb->tech_battery_off | p_cb->proto_switch_on |
          p_cb->proto_switch_off | p_cb->proto_battery_off |
          p_cb->aid_entries | p_cb->apdu_pattern_entries) {
        /* this NFCEE still has configuration when deactivated. clear the
         * configuration */
        nfa_ee_cb.ee_cfged &= ~nfa_ee_ecb_to_mask(p_cb);
        nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_CHANGED_ROUTING;
        NFA_TRACE_DEBUG0("deactivating/still configured. Force update");
      }
      p_cb->tech_switch_on = p_cb->tech_switch_off = p_cb->tech_battery_off = 0;
      p_cb->proto_switch_on = p_cb->proto_switch_off = p_cb->proto_battery_off =
          0;
      p_cb->apdu_pattern_entries = p_cb->aid_entries = 0;
#endif
#if (NXP_EXTNS == TRUE)
      if (p_cb->ee_status != NFC_NFCEE_STATUS_REMOVED)
#endif
        p_cb->ee_status = NFC_NFCEE_STATUS_INACTIVE;
    }
  } else {
    if (p_rsp->mode == NFA_EE_MD_ACTIVATE) {
      p_cb->ee_status = NFC_NFCEE_STATUS_REMOVED;
    }
  }
  NFA_TRACE_DEBUG4("status:%d ecb_flags  :0x%02x ee_cfged:0x%02x ee_status:%d",
                   p_rsp->status, p_cb->ecb_flags, nfa_ee_cb.ee_cfged,
                   p_cb->ee_status);
  if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_RESTORE) {
    if (p_cb->conn_st == NFA_EE_CONN_ST_CONN) {
      /* NFA_HCI module handles restoring configurations for HCI access */
      if (p_cb->ee_interface[0] != NFC_NFCEE_INTERFACE_HCI_ACCESS) {
        NFC_ConnCreate(NCI_DEST_TYPE_NFCEE, p_cb->nfcee_id, p_cb->use_interface,
                       nfa_ee_conn_cback);
      }
    } else {
      p_cb->ecb_flags &= ~NFA_EE_ECB_FLAGS_RESTORE;
      nfa_ee_check_restore_complete();
    }
  } else {
    mode_set.status = p_rsp->status;
    mode_set.ee_handle = (tNFA_HANDLE)p_rsp->nfcee_id | NFA_HANDLE_GROUP_EE;
    mode_set.ee_status = p_cb->ee_status;

    nfa_ee_report_event(p_cb->p_ee_cback, NFA_EE_MODE_SET_EVT,
                        (void*)&mode_set);

#ifdef NXP_NCI20_DUAL_UICC
    if ((p_cb->ee_status == NFC_NFCEE_STATUS_INACTIVE) ||
        (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE)) {
      /* Report NFA_EE_DISCOVER_REQ_EVT for all active NFCEE */
      nfa_ee_report_discover_req_evt();
    }
#endif
  }
  if(NFA_GetNCIVersion() == NCI_VERSION_2_0)
  {
      if (nfa_ee_cb.p_enable_cback)
                  (*nfa_ee_cb.p_enable_cback) (NFA_EE_MODE_SET_COMPLETE);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_report_update_evt
**
** Description      Check if need to report NFA_EE_UPDATED_EVT
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_report_update_evt(void) {
  tNFA_EE_CBACK_DATA evt_data;

  NFA_TRACE_DEBUG2("nfa_ee_report_update_evt ee_wait_evt:0x%x wait_rsp:%d",
                   nfa_ee_cb.ee_wait_evt, nfa_ee_cb.wait_rsp);
  if (nfa_ee_cb.wait_rsp == 0) {
    nfa_ee_cb.ee_wait_evt &= ~NFA_EE_WAIT_UPDATE_RSP;

    if (nfa_ee_cb.ee_wait_evt & NFA_EE_WAIT_UPDATE) {
      nfa_ee_cb.ee_wait_evt &= ~NFA_EE_WAIT_UPDATE;
      /* finished updating NFCC; report NFA_EE_UPDATED_EVT now */
      evt_data.status = NFA_STATUS_OK;
      nfa_ee_report_event(NULL, NFA_EE_UPDATED_EVT, &evt_data);
    }
  }
}

/*******************************************************************************
**
** Function         nfa_ee_nci_wait_rsp
**
** Description      Process the result for NCI response
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_wait_rsp(tNFA_EE_MSG* p_data) {
  tNFA_EE_NCI_WAIT_RSP* p_rsp = &p_data->wait_rsp;

  NFA_TRACE_DEBUG2("nfa_ee_nci_wait_rsp() ee_wait_evt:0x%x wait_rsp:%d",
                   nfa_ee_cb.ee_wait_evt, nfa_ee_cb.wait_rsp);
  if (nfa_ee_cb.wait_rsp) {
    if (p_rsp->opcode == NCI_MSG_RF_SET_ROUTING) nfa_ee_cb.wait_rsp--;
  }
  nfa_ee_report_update_evt();
}

/*******************************************************************************
**
** Function         nfa_ee_nci_conn
**
** Description      process the connection callback events
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_conn(tNFA_EE_MSG* p_data) {
  tNFA_EE_ECB* p_cb;
  tNFA_EE_NCI_CONN* p_cbk = &p_data->conn;
  tNFC_CONN* p_conn = p_data->conn.p_data;
  NFC_HDR* p_pkt = NULL;
  tNFA_EE_CBACK_DATA evt_data = {0};
  tNFA_EE_EVT event = NFA_EE_INVALID;
  tNFA_EE_CBACK* p_cback = NULL;

  if (p_cbk->event == NFC_CONN_CREATE_CEVT) {
    p_cb = nfa_ee_find_ecb(p_cbk->p_data->conn_create.id);
  } else {
    p_cb = nfa_ee_find_ecb_by_conn_id(p_cbk->conn_id);
    if (p_cbk->event == NFC_DATA_CEVT) p_pkt = p_conn->data.p_data;
  }

  if (p_cb) {
    p_cback = p_cb->p_ee_cback;
    evt_data.handle = (tNFA_HANDLE)p_cb->nfcee_id | NFA_HANDLE_GROUP_EE;
    switch (p_cbk->event) {
      case NFC_CONN_CREATE_CEVT:
        if (p_conn->conn_create.status == NFC_STATUS_OK) {
          p_cb->conn_id = p_cbk->conn_id;
          p_cb->conn_st = NFA_EE_CONN_ST_CONN;
        } else {
          p_cb->conn_st = NFA_EE_CONN_ST_NONE;
        }
        if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_RESTORE) {
          p_cb->ecb_flags &= ~NFA_EE_ECB_FLAGS_RESTORE;
          nfa_ee_check_restore_complete();
        } else {
          evt_data.connect.status = p_conn->conn_create.status;
          evt_data.connect.ee_interface = p_cb->use_interface;
          event = NFA_EE_CONNECT_EVT;
        }
        break;

      case NFC_CONN_CLOSE_CEVT:
        if (p_cb->conn_st != NFA_EE_CONN_ST_DISC) event = NFA_EE_DISCONNECT_EVT;
        p_cb->conn_st = NFA_EE_CONN_ST_NONE;
        p_cb->p_ee_cback = NULL;
        p_cb->conn_id = 0;

#if(NXP_EXTNS == TRUE)
        nfa_ee_cb.ee_flags &= ~NFA_EE_HCI_CONN_CLOSE;
#endif
        if (nfa_ee_cb.em_state == NFA_EE_EM_STATE_DISABLING) {
          if (nfa_ee_cb.ee_flags & NFA_EE_FLAG_WAIT_DISCONN) {
            if (nfa_ee_cb.num_ee_expecting) {
              nfa_ee_cb.num_ee_expecting--;
            }
          }
          if (nfa_ee_cb.num_ee_expecting == 0) {
            nfa_ee_cb.ee_flags &= ~NFA_EE_FLAG_WAIT_DISCONN;
            nfa_ee_check_disable();
          }
        }
        break;

      case NFC_DATA_CEVT:
        if (p_cb->conn_st == NFA_EE_CONN_ST_CONN) {
          /* report data event only in connected state */
          if (p_cb->p_ee_cback && p_pkt) {
            evt_data.data.len = p_pkt->len;
            evt_data.data.p_buf = (uint8_t*)(p_pkt + 1) + p_pkt->offset;
            event = NFA_EE_DATA_EVT;
            p_pkt = NULL; /* so this function does not free this GKI buffer */
          }
        }
        break;
    }

    if ((event != NFA_EE_INVALID) && (p_cback)) (*p_cback)(event, &evt_data);
  }
  if (p_pkt) GKI_freebuf(p_pkt);
}

/*******************************************************************************
**
** Function         nfa_ee_nci_action_ntf
**
** Description      process the NFCEE action callback event
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_action_ntf(tNFA_EE_MSG* p_data) {
  tNFC_EE_ACTION_REVT* p_cbk = p_data->act.p_data;
  tNFA_EE_ACTION evt_data;

  evt_data.ee_handle = (tNFA_HANDLE)p_cbk->nfcee_id | NFA_HANDLE_GROUP_EE;
  evt_data.trigger = p_cbk->act_data.trigger;
  memcpy(&(evt_data.param), &(p_cbk->act_data.param),
         sizeof(tNFA_EE_ACTION_PARAM));
#if (NXP_EXTNS == TRUE)
  /*If DWP CL started inform to HCI*/
  if (evt_data.ee_handle == 0x4C0) {
    nfa_ee_ce_p61_active = 0x01;
  }
#endif
  nfa_ee_report_event(NULL, NFA_EE_ACTION_EVT, (void*)&evt_data);
}

#if (NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function         nfa_ee_get_supported_tech_list
**
** Description      provides the supported technology list of given nfcee id
**
** Returns          uint8_t
**
*******************************************************************************/
uint8_t nfa_ee_get_supported_tech_list(uint8_t nfcee_id) {
  uint8_t tech_list = 0;
  tNFA_EE_ECB* p_cb = NULL;

  p_cb = nfa_ee_find_ecb(nfcee_id);
  if (p_cb) {
    if (p_cb->la_protocol) tech_list |= NFA_TECHNOLOGY_MASK_A;
    if (p_cb->lb_protocol) tech_list |= NFA_TECHNOLOGY_MASK_B;
    if (p_cb->lf_protocol) tech_list |= NFA_TECHNOLOGY_MASK_F;
  } else {
    NFA_TRACE_DEBUG1("Cannot find cb for given nfcee_id: 0x%x", nfcee_id);
  }
  NFA_TRACE_DEBUG2("supported tech list is 0x0%x for given nfcee_id: 0x%x ",
                   tech_list, nfcee_id);
  return tech_list;
}
#endif

/*******************************************************************************
**
** Function         nfa_ee_nci_disc_req_ntf
**
** Description      process the NFCEE discover request callback event
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_nci_disc_req_ntf(tNFA_EE_MSG* p_data) {
  tNFC_EE_DISCOVER_REQ_REVT* p_cbk = p_data->disc_req.p_data;
  tNFA_HANDLE ee_handle;
  tNFA_EE_ECB* p_cb = NULL;
  uint8_t report_ntf = 0;
  uint8_t xx;

  NFA_TRACE_DEBUG2("nfa_ee_nci_disc_req_ntf () num_info: %d cur_ee:%d",
                   p_cbk->num_info, nfa_ee_cb.cur_ee);

  for (xx = 0; xx < p_cbk->num_info; xx++) {
    ee_handle = NFA_HANDLE_GROUP_EE | p_cbk->info[xx].nfcee_id;

    p_cb = nfa_ee_find_ecb(p_cbk->info[xx].nfcee_id);
    if (!p_cb) {
      NFA_TRACE_DEBUG1("Cannot find cb for NFCEE: 0x%x",
                       p_cbk->info[xx].nfcee_id);
      p_cb = nfa_ee_find_ecb(NFA_EE_INVALID);
      if (p_cb) {
        p_cb->nfcee_id = p_cbk->info[xx].nfcee_id;
        p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_ORDER;
      } else {
        NFA_TRACE_ERROR1("Cannot allocate cb for NFCEE: 0x%x",
                         p_cbk->info[xx].nfcee_id);
        continue;
      }
    } else {
      report_ntf |= nfa_ee_ecb_to_mask(p_cb);
    }

    p_cb->ecb_flags |= NFA_EE_ECB_FLAGS_DISC_REQ;
#if (NXP_EXTNS == TRUE)
    p_cb->ee_req_op = p_cbk->info[xx].op;
#endif
    if (p_cbk->info[xx].op == NFC_EE_DISC_OP_ADD) {
      if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_A) {
        p_cb->la_protocol = p_cbk->info[xx].protocol;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_B) {
        p_cb->lb_protocol = p_cbk->info[xx].protocol;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_F) {
        p_cb->lf_protocol = p_cbk->info[xx].protocol;
      } else if (p_cbk->info[xx].tech_n_mode ==
                 NFC_DISCOVERY_TYPE_LISTEN_B_PRIME) {
        p_cb->lbp_protocol = p_cbk->info[xx].protocol;
      }
#if (NXP_EXTNS == TRUE)
      // code to handle and store Reader type(A/B) requested for Reader over
      // SWP.
      /*Reader over SWP*/
      else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_POLL_A) {
        p_cb->pa_protocol = p_cbk->info[xx].protocol;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_POLL_B) {
        p_cb->pb_protocol = p_cbk->info[xx].protocol;
      }
#endif
      NFA_TRACE_DEBUG6(
          "nfcee_id=0x%x ee_status=0x%x ecb_flags=0x%x la_protocol=0x%x "
          "lb_protocol=0x%x lf_protocol=0x%x",
          p_cb->nfcee_id, p_cb->ee_status, p_cb->ecb_flags, p_cb->la_protocol,
          p_cb->lb_protocol, p_cb->lf_protocol);
    } else {
      if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_A) {
        p_cb->la_protocol = 0;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_B) {
        p_cb->lb_protocol = 0;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_LISTEN_F) {
        p_cb->lf_protocol = 0;
      } else if (p_cbk->info[xx].tech_n_mode ==
                 NFC_DISCOVERY_TYPE_LISTEN_B_PRIME) {
        p_cb->lbp_protocol = 0;
      }
#if (NXP_EXTNS == TRUE)
      // code to handle and store Reader type(A/B) requested for Reader over
      // SWP.
      /*Reader over SWP*/
      else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_POLL_A) {
        p_cb->pa_protocol = 0xFF;
      } else if (p_cbk->info[xx].tech_n_mode == NFC_DISCOVERY_TYPE_POLL_B) {
        p_cb->pb_protocol = 0xFF;
      }
#endif
    }
  }

  /* Report NFA_EE_DISCOVER_REQ_EVT for all active NFCEE */
  if (report_ntf) nfa_ee_report_discover_req_evt();
}

/*******************************************************************************
**
** Function         nfa_ee_is_active
**
** Description      Check if the given NFCEE is active
**
** Returns          true if the given NFCEE is active
**
*******************************************************************************/
bool nfa_ee_is_active(tNFA_HANDLE nfcee_id) {
  bool is_active = false;
  int xx;
  tNFA_EE_ECB* p_cb = nfa_ee_cb.ecb;

#if (NXP_EXTNS == TRUE)
  // Added case for NFCEE_DH.
  if (nfcee_id == NFA_EE_HANDLE_DH) {
    is_active = true;
    goto TheEnd;
  }
#endif
  if ((NFA_HANDLE_GROUP_MASK & nfcee_id) == NFA_HANDLE_GROUP_EE)
    nfcee_id &= NFA_HANDLE_MASK;

  /* compose output */
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if ((tNFA_HANDLE)p_cb->nfcee_id == nfcee_id) {
      if (p_cb->ee_status == NFA_EE_STATUS_ACTIVE) {
        is_active = true;
      }
      break;
    }
  }
#if (NXP_EXTNS == TRUE)
TheEnd:
#endif
  return is_active;
}

/*******************************************************************************
**
** Function         nfa_ee_get_tech_route
**
** Description      Given a power state, find the technology routing
**                  destination. The result is filled in the given p_handles
**                  in the order of A, B, F, Bprime
**
** Returns          None
**
*******************************************************************************/
void nfa_ee_get_tech_route(uint8_t power_state, uint8_t* p_handles) {
  int xx, yy;
  tNFA_EE_ECB* p_cb;
  uint8_t tech_mask_list[NFA_EE_MAX_TECH_ROUTE] = {
      NFA_TECHNOLOGY_MASK_A, NFA_TECHNOLOGY_MASK_B, NFA_TECHNOLOGY_MASK_F,
      NFA_TECHNOLOGY_MASK_B_PRIME};

  NFA_TRACE_DEBUG1("nfa_ee_get_tech_route(): power state : %d", power_state);

  for (xx = 0; xx < NFA_EE_MAX_TECH_ROUTE; xx++) {
    p_handles[xx] = NFC_DH_ID;
    p_cb = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee - 1];
    for (yy = 0; yy < nfa_ee_cb.cur_ee; yy++, p_cb--) {
      if (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE) {
        switch (power_state) {
          case NFA_EE_PWR_STATE_ON:
            if (p_cb->tech_switch_on & tech_mask_list[xx])
              p_handles[xx] = p_cb->nfcee_id;
            break;
          case NFA_EE_PWR_STATE_SWITCH_OFF:
            if (p_cb->tech_switch_off & tech_mask_list[xx])
              p_handles[xx] = p_cb->nfcee_id;
            break;
          case NFA_EE_PWR_STATE_BATT_OFF:
            if (p_cb->tech_battery_off & tech_mask_list[xx])
              p_handles[xx] = p_cb->nfcee_id;
            break;
        }
      }
    }
  }
  NFA_TRACE_DEBUG4(
      "TechA route: 0x%x, TechB route: 0x%x, TechF route: 0x%x, TechB' route: "
      "0x%x",
      p_handles[0], p_handles[1], p_handles[2], p_handles[3]);
}

/*******************************************************************************
**
** Function         nfa_ee_check_set_routing
**
** Description      If the new size exceeds the capacity of next block,
**                  send the routing command now and reset the related
**                  parameters.
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_check_set_routing(uint16_t new_size, int* p_max_len, uint8_t* p,
                              int* p_cur_offset) {
  uint8_t max_tlv = (uint8_t)((*p_max_len > NFA_EE_ROUT_MAX_TLV_SIZE)
                                  ? NFA_EE_ROUT_MAX_TLV_SIZE
                                  : *p_max_len);
  tNFA_STATUS status = NFA_STATUS_OK;

  if (new_size + *p_cur_offset > max_tlv) {
    if (NFC_SetRouting(true, *p, *p_cur_offset, p + 1) == NFA_STATUS_OK) {
      nfa_ee_cb.wait_rsp++;
    }
    /* after the routing command is sent, re-use the same buffer to send the
     * next routing command.
     * reset the related parameters */
    if (*p_max_len > *p_cur_offset)
      *p_max_len -= *p_cur_offset; /* the max is reduced */
    else
      *p_max_len = 0;
    *p_cur_offset = 0; /* nothing is in queue any more */
    *p = 0;            /* num_tlv=0 */
  }
}

/*******************************************************************************
**
** Function         nfa_ee_route_add_one_ecb
**
** Description      Add the routing entries for one NFCEE/DH
**
** Returns          NFA_STATUS_OK, if ok to continue
**
*******************************************************************************/
tNFA_STATUS nfa_ee_route_add_one_ecb(tNFA_EE_ECB* p_cb, int* p_max_len,
                                     bool more, uint8_t* ps,
                                     int* p_cur_offset) {
  uint8_t* p, *pa;
  uint16_t tlv_size;
  uint8_t num_tlv, len;
  int xx;
  int start_offset;
  uint8_t power_cfg = 0;
  uint8_t* pp = ps + *p_cur_offset;
  uint8_t entry_size;
  uint8_t max_tlv;
  uint8_t* p_start;
  uint8_t new_size;
  tNFA_STATUS status = NFA_STATUS_OK;

  nfa_ee_check_set_routing(p_cb->size_mask, p_max_len, ps, p_cur_offset);
  max_tlv = (uint8_t)((*p_max_len > NFA_EE_ROUT_MAX_TLV_SIZE)
                          ? NFA_EE_ROUT_MAX_TLV_SIZE
                          : *p_max_len);
  /* use the first byte of the buffer (ps) to keep the num_tlv */
  num_tlv = *ps;
  NFA_TRACE_DEBUG5(
      "nfa_ee_route_add_one_ecb max_len:%d, max_tlv:%d, cur_offset:%d, "
      "more:%d, num_tlv:%d",
      *p_max_len, max_tlv, *p_cur_offset, more, num_tlv);
  pp = ps + 1 + *p_cur_offset;
  p = pp;
  tlv_size = (uint8_t)*p_cur_offset;
/* Routing entries in the order of AID entries,protocol entries  and technology
 * entries*/
#if ((NXP_EXTNS == TRUE) && (NFC_NXP_LISTEN_ROUTE_TBL_OPTIMIZATION == TRUE))
  /* add the AID routing */
  if (p_cb->nfcee_id == NFC_DH_ID && p_cb->aid_entries) {
    start_offset = 0;
    for (xx = 0; xx < p_cb->aid_entries; xx++) {
      /* rememebr the beginning of this AID routing entry, just in case we need
       * to put it in next command */
      uint8_t route_qual = 0;
      p_start = pp;
      /* add one AID entry */
      if ((p_cb->aid_rt_info[xx] & NFA_EE_AE_ROUTE) &&
          ((nfa_ee_nfeeid_active(p_cb->aid_rt_loc[xx]) == true) ||
           (p_cb->aid_rt_loc[xx] == NFC_DH_ID))) {
        num_tlv++;
        pa = &p_cb->aid_cfg[start_offset];
        pa++;        /* EMV tag */
        len = *pa++; /* aid_len */
        if(p_cb->aid_info[xx] & NCI_ROUTE_QUAL_LONG_SELECT)
          route_qual |= NCI_ROUTE_QUAL_LONG_SELECT;
        if(p_cb->aid_info[xx] & NCI_ROUTE_QUAL_SHORT_SELECT)
          route_qual |= NCI_ROUTE_QUAL_SHORT_SELECT;
        *pp = NFC_ROUTE_TAG_AID | route_qual;
         pp++;
        *pp++ = len + 2;
        *pp++ = p_cb->aid_rt_loc[xx];
        *pp++ = p_cb->aid_pwr_cfg[xx];
        /* copy the AID */
        memcpy(pp, pa, len);
        pp += len;
      }
      start_offset += p_cb->aid_len[xx];
      new_size = (uint8_t)(pp - p_start);
      nfa_ee_check_set_routing(new_size, p_max_len, ps, p_cur_offset);
      if (*ps == 0 && num_tlv > 0x00) {
        /* just sent routing command, update local */
        *ps = 1;
        num_tlv = *ps;
        *p_cur_offset = new_size;
        pp = ps + 1;
        p = pp;
        tlv_size = (uint8_t)*p_cur_offset;
        max_tlv = (uint8_t)((*p_max_len > NFA_EE_ROUT_MAX_TLV_SIZE)
                                ? NFA_EE_ROUT_MAX_TLV_SIZE
                                : *p_max_len);
        memcpy(p, p_start, new_size);
        pp += new_size;
      } else {
        /* add the new entry */
        *ps = num_tlv;
        *p_cur_offset += new_size;
      }
    }
  }

  /* Store the Protocol based routing in temporary buffer */
  for (xx = 0; xx < NFA_EE_NUM_PROTO; xx++) {
    power_cfg = 0;
    if (p_cb->proto_switch_on & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->proto_switch_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->proto_battery_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;

    if (power_cfg != 0x00) {
        if ((nfcFL.chipType == pn547C2) && (p_cb->nfcee_id == NFC_DH_ID)) {
            power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
        }
        else {
            if (p_cb->proto_screen_lock & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
            if (p_cb->proto_screen_off & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
            if (p_cb->proto_screen_off_lock & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
        }
    }
    if (power_cfg) {
        *proto_pp = NFC_ROUTE_TAG_PROTO;
        proto_pp++;
        *proto_pp++ = 3;
        *proto_pp++ = p_cb->nfcee_id;
        *proto_pp++ = power_cfg;
        *proto_pp++ = nfa_ee_proto_list[xx];
        proto_tlv_ctr++;
        if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
            nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }

  /* add NFC-DEP routing to HOST */
  if ((p_cb->nfcee_id == NFC_DH_ID) &&
      !(nfa_ee_cb.ee_flags & NFA_EE_FLAG_CFG_NFC_DEP)) {
    nfa_ee_cb.ee_flags |= NFA_EE_FLAG_CFG_NFC_DEP;
    *proto_pp++ = NFC_ROUTE_TAG_PROTO;
    *proto_pp++ = 3;
    *proto_pp++ = NFC_DH_ID;
    if (gNfaProvisionMode) {
      /* only if device is in provision mode, set power state to screen locked
       */
      *proto_pp++ = 0x41;
    } else {
      *proto_pp++ = 1;
    }
    *proto_pp++ = NFC_PROTOCOL_NFC_DEP;
    proto_tlv_ctr++;
  }
  /* store  the Technology based routing entries in temporary buffer */
  for (xx = 0; xx < NFA_EE_NUM_TECH; xx++) {
    power_cfg = 0;
    if (p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;

    if (power_cfg & NCI_ROUTE_PWR_STATE_ON) {
      if (p_cb->tech_screen_lock & nfa_ee_tech_mask_list[xx])
        power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
      if (p_cb->tech_screen_off & nfa_ee_tech_mask_list[xx])
        power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
      if (p_cb->tech_screen_off_lock & nfa_ee_tech_mask_list[xx])
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
    }
    if (power_cfg) {
      *tech_pp++ = NFC_ROUTE_TAG_TECH;
      *tech_pp++ = 3;
      *tech_pp++ = p_cb->nfcee_id;
      *tech_pp++ = power_cfg;
      *tech_pp++ = nfa_ee_tech_list[xx];
      tech_tlv_ctr++;
      if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
        nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }
#else /* +Routing Entries in the order: Technology, protocol and AID entries \
         */
  /* add the Technology based routing */
  for (xx = 0; xx < NFA_EE_NUM_TECH; xx++) {
    power_cfg = 0;
    if (p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
#if (NXP_EXTNS == TRUE)

    if (power_cfg & NCI_ROUTE_PWR_STATE_ON) {
      if (p_cb->tech_screen_lock & nfa_ee_tech_mask_list[xx])
        power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
      if (p_cb->tech_screen_off & nfa_ee_tech_mask_list[xx])
        power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF;
      if (p_cb->tech_screen_off_lock & nfa_ee_tech_mask_list[xx])
          power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
    }
#endif
    if (power_cfg) {
      *pp++ = NFC_ROUTE_TAG_TECH;
      *pp++ = 3;
      *pp++ = p_cb->nfcee_id;
      *pp++ = power_cfg;
      *pp++ = nfa_ee_tech_list[xx];
      num_tlv++;
      if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
        nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }

  /* add the Protocol based routing */
  for (xx = 0; xx < NFA_EE_NUM_PROTO; xx++) {
    power_cfg = 0;
    if (p_cb->proto_switch_on & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_ON;
    if (p_cb->proto_switch_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_SWITCH_OFF;
    if (p_cb->proto_battery_off & nfa_ee_proto_mask_list[xx])
      power_cfg |= NCI_ROUTE_PWR_STATE_BATT_OFF;
#if (NXP_EXTNS == TRUE)

    if (power_cfg != 0x00) {
        /* Blocking codes to enable protocol routing to host when screen is off.*/
        if ((nfcFL.chipType == pn547C2) && (p_cb->nfcee_id == NFC_DH_ID)) {
            power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
        }
        else
        {
            if (p_cb->proto_screen_lock & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_ON_LOCK();
            if (p_cb->proto_screen_off & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_UNLOCK();
            if (p_cb->proto_screen_off_lock & nfa_ee_proto_mask_list[xx])
                power_cfg |= NCI_ROUTE_PWR_STATE_SCREEN_OFF_LOCK();
        }
    }
#endif

    if (power_cfg) {
      *pp = NFC_ROUTE_TAG_PROTO;
      *pp++;
      *pp++ = 3;
      *pp++ = p_cb->nfcee_id;
      *pp++ = power_cfg;
      *pp++ = nfa_ee_proto_list[xx];
      num_tlv++;
      if (power_cfg != NCI_ROUTE_PWR_STATE_ON)
        nfa_ee_cb.ee_cfged |= NFA_EE_CFGED_OFF_ROUTING;
    }
  }

  /* add NFC-DEP routing to HOST */
  if ((p_cb->nfcee_id == NFC_DH_ID)
#if (NXP_EXTNS == TRUE)
      && !(nfa_ee_cb.ee_flags & NFA_EE_FLAG_CFG_NFC_DEP)
#endif
          ) {
#if (NXP_EXTNS == TRUE)
    nfa_ee_cb.ee_flags |= NFA_EE_FLAG_CFG_NFC_DEP;
#endif
    *pp++ = NFC_ROUTE_TAG_PROTO;
    *pp++ = 3;
    *pp++ = NFC_DH_ID;
    *pp++ = NCI_ROUTE_PWR_STATE_ON;
    *pp++ = NFC_PROTOCOL_NFC_DEP;
    num_tlv++;
  }

  /* update the num_tlv and current offset */
  entry_size = (uint8_t)(pp - p);
  *p_cur_offset += entry_size;
  *ps = num_tlv;
/* add the AID routing */
#if (NXP_EXTNS == TRUE)
  if (p_cb->nfcee_id == NFC_DH_ID && p_cb->aid_entries)
#else
  if (p_cb->aid_entries)
#endif
  {
    start_offset = 0;
    for (xx = 0; xx < p_cb->aid_entries; xx++) {
      p_start =
          pp; /* rememebr the beginning of this AID routing entry, just in case
                 we need to put it in next command */
/* add one AID entry */
#if (NXP_EXTNS == TRUE)
      if ((p_cb->aid_rt_info[xx] & NFA_EE_AE_ROUTE) &&
          ((nfa_ee_nfeeid_active(p_cb->aid_rt_loc[xx]) == true) ||
           (p_cb->aid_rt_loc[xx] == NFC_DH_ID)))
#else
      if (p_cb->aid_rt_info[xx] & NFA_EE_AE_ROUTE)
#endif
      {
        num_tlv++;
        pa = &p_cb->aid_cfg[start_offset];
        pa++;        /* EMV tag */
        len = *pa++; /* aid_len */
#if (NXP_EXTNS == TRUE)
        if(p_cb->aid_info[xx] & NCI_ROUTE_QUAL_LONG_SELECT)
          route_qual |= NCI_ROUTE_QUAL_LONG_SELECT;
        if(p_cb->aid_info[xx] & NCI_ROUTE_QUAL_SHORT_SELECT)
          route_qual |= NCI_ROUTE_QUAL_SHORT_SELECT;
        *pp = NFC_ROUTE_TAG_AID | route_qual;
         pp++;
#else
        *pp++ = NFC_ROUTE_TAG_AID;
#endif
        *pp++ = len + 2;
#if (NXP_EXTNS == TRUE)
        *pp++ = p_cb->aid_rt_loc[xx];
#else
        *pp++ = p_cb->nfcee_id;
#endif
        *pp++ = p_cb->aid_pwr_cfg[xx];
        /* copy the AID */
        memcpy(pp, pa, len);
        pp += len;
      }
      start_offset += p_cb->aid_len[xx];
      new_size = (uint8_t)(pp - p_start);
      nfa_ee_check_set_routing(new_size, p_max_len, ps, p_cur_offset);
      if (*ps == 0) {
        /* just sent routing command, update local */
        *ps = 1;
        num_tlv = *ps;
        *p_cur_offset = new_size;
        pp = ps + 1;
        p = pp;
        tlv_size = (uint8_t)*p_cur_offset;
        max_tlv = (uint8_t)((*p_max_len > NFA_EE_ROUT_MAX_TLV_SIZE)
                                ? NFA_EE_ROUT_MAX_TLV_SIZE
                                : *p_max_len);
        memcpy(p, p_start, new_size);
        pp += new_size;
      } else {
        /* add the new entry */
        *ps = num_tlv;
        *p_cur_offset += new_size;
      }
    }
  }
#endif /* - Routing Entries in the order: Technology, protocol and AID entries \
          */
  tlv_size = nfa_ee_total_lmrt_size();
  if (tlv_size) {
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
  }
  if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_ROUTING) {
    nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_CHANGED_ROUTING;
  }
  NFA_TRACE_DEBUG2("ee_cfg_sts:0x%02x lmrt_size:%d", nfa_ee_cb.ee_cfg_sts,
                   tlv_size);

  if (more == false) {
#if ((NXP_EXTNS == TRUE) && (NFC_NXP_LISTEN_ROUTE_TBL_OPTIMIZATION == TRUE))
    /* add the protocol entries and technology entries  after AID entries*/
    new_size = (uint8_t)(proto_pp - proto_route_buff);
    new_size += (uint8_t)(tech_pp - tech_route_buff);
    nfa_ee_check_set_routing(new_size, p_max_len, ps, p_cur_offset);
    if (*ps == 0) {
      p = ps + 1;
      num_tlv = (proto_tlv_ctr + tech_tlv_ctr);
    } else {
      num_tlv += proto_tlv_ctr;
      num_tlv += tech_tlv_ctr;
      p = ps + 1 + *p_cur_offset;
    }
    if (proto_tlv_ctr != 0) {
      new_size = (uint8_t)(proto_pp - proto_route_buff);
      memcpy(p, proto_route_buff, new_size);
      *p_cur_offset += new_size;
      p = p + new_size;
    }
    if (tech_tlv_ctr != 0) {
      new_size = (uint8_t)(tech_pp - tech_route_buff);
      memcpy(p, tech_route_buff, new_size);
      *p_cur_offset += new_size;
    }
#endif /* - Routing entries optimization */

    /* last entry. update routing table now */
    if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_CHANGED_ROUTING) {
      if (tlv_size) {
        nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_PREV_ROUTING;
      } else {
        nfa_ee_cb.ee_cfg_sts &= ~NFA_EE_STS_PREV_ROUTING;
      }
      NFA_TRACE_DEBUG2(
          "nfa_ee_route_add_one_ecb: set routing num_tlv:%d tlv_size:%d",
          num_tlv, tlv_size);
      if (NFC_SetRouting(more, num_tlv, (uint8_t)(*p_cur_offset), ps + 1) ==
          NFA_STATUS_OK) {
        nfa_ee_cb.wait_rsp++;
      }
    } else if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_PREV_ROUTING) {
      if (tlv_size == 0) {
        nfa_ee_cb.ee_cfg_sts &= ~NFA_EE_STS_PREV_ROUTING;
        /* indicated routing is configured to NFCC */
        nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_CHANGED_ROUTING;
        if (NFC_SetRouting(more, 0, 0, ps + 1) == NFA_STATUS_OK) {
          nfa_ee_cb.wait_rsp++;
        }
      }
    }
  }

  return status;
}
/*******************************************************************************
**
** Function         nfa_ee_route_add_one_ecb_order
**
** Description      Add the routing entries for NFCEE/DH in order defined
**
** Returns          NFA_STATUS_OK, if ok to continue
**
*******************************************************************************/
void nfa_ee_route_add_one_ecb_by_route_order(tNFA_EE_ECB* p_cb, int rout_type,
                                             int* p_max_len, bool more,
                                             uint8_t* ps, int* p_cur_offset) {
  nfa_ee_check_set_routing(p_cb->size_mask, p_max_len, ps, p_cur_offset);

  /* use the first byte of the buffer (ps) to keep the num_tlv */
  uint8_t num_tlv = *ps;
  NFA_TRACE_DEBUG6(
      "%s - max_len:%d, cur_offset:%d, more:%d, num_tlv:%d,rout_type:- %d",
      __func__, *p_max_len, *p_cur_offset, more, num_tlv, rout_type);
  uint8_t* pp = ps + 1 + *p_cur_offset;
  uint8_t* p = pp;
  uint16_t tlv_size = (uint8_t)*p_cur_offset;

  switch (rout_type) {
    case NCI_ROUTE_ORDER_TECHNOLOGY: {
      nfa_ee_add_tech_route_to_ecb(p_cb, pp, p, ps, p_cur_offset);
    } break;

    case NCI_ROUTE_ORDER_PROTOCOL: {
      nfa_ee_add_proto_route_to_ecb(p_cb, pp, p, ps, p_cur_offset);
    } break;
    case NCI_ROUTE_ORDER_AID: {
      nfa_ee_add_aid_route_to_ecb(p_cb, pp, p, ps, p_cur_offset, p_max_len);
    } break;
#if (NXP_EXTNS == TRUE)
        case NCI_ROUTE_ORDER_PATTERN: {
        nfa_ee_add_apdu_route_to_ecb(p_cb, pp, p, ps, p_cur_offset, p_max_len);
    } break;
#endif
    default: {
      NFA_TRACE_DEBUG2("%s -  Route type - NA:- %d", __func__, rout_type);
    }
  }

  /* update the total number of entries */
  num_tlv = *ps;

  tlv_size = nfa_ee_total_lmrt_size();
  if (tlv_size) {
    nfa_ee_cb.ee_cfged |= nfa_ee_ecb_to_mask(p_cb);
  }
  if (p_cb->ecb_flags & NFA_EE_ECB_FLAGS_ROUTING) {
    nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_CHANGED_ROUTING;
  }
  NFA_TRACE_DEBUG2("ee_cfg_sts:0x%02x lmrt_size:%d", nfa_ee_cb.ee_cfg_sts,
                   tlv_size);

  if (more == false) {
    /* last entry. update routing table now */
    if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_CHANGED_ROUTING) {
      if (tlv_size) {
        nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_PREV_ROUTING;
      } else {
        nfa_ee_cb.ee_cfg_sts &= ~NFA_EE_STS_PREV_ROUTING;
      }
      NFA_TRACE_DEBUG3("%s : set routing num_tlv:%d tlv_size:%d", __func__,
                       num_tlv, tlv_size);
      if (NFC_SetRouting(more, num_tlv, (uint8_t)(*p_cur_offset), ps + 1) ==
          NFA_STATUS_OK) {
        nfa_ee_cb.wait_rsp++;
      }
    } else if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_PREV_ROUTING) {
      if (tlv_size == 0) {
        nfa_ee_cb.ee_cfg_sts &= ~NFA_EE_STS_PREV_ROUTING;
        /* indicated routing is configured to NFCC */
        nfa_ee_cb.ee_cfg_sts |= NFA_EE_STS_CHANGED_ROUTING;
        if (NFC_SetRouting(more, 0, 0, ps + 1) == NFA_STATUS_OK) {
          nfa_ee_cb.wait_rsp++;
        }
      }
    }
  }
}

/*******************************************************************************
**
** Function         nfa_ee_need_recfg
**
** Description      Check if any API function to configure the routing table or
**                  VS is called since last update
**
**                  The algorithm for the NFCEE configuration handling is as
**                  follows:
**
**                  Each NFCEE_ID/DH has its own control block - tNFA_EE_ECB
**                  Each control block uses ecb_flags to keep track if an API
**                  that changes routing/VS is invoked. This ecb_flags is
**                  cleared at the end of nfa_ee_update_rout().
**
**                  nfa_ee_cb.ee_cfged is the bitmask of the control blocks with
**                  routing/VS configuration and NFA_EE_CFGED_UPDATE_NOW.
**                  nfa_ee_cb.ee_cfged is cleared and re-calculated at the end
**                  of nfa_ee_update_rout().
**
**                  nfa_ee_cb.ee_cfg_sts is used to check is any status is
**                  changed and the associated command is issued to NFCC.
**                  nfa_ee_cb.ee_cfg_sts is AND with NFA_EE_STS_PREV at the end
**                  of nfa_ee_update_rout() to clear the NFA_EE_STS_CHANGED bits
**                  (except NFA_EE_STS_CHANGED_CANNED_VS is cleared in
**                  nfa_ee_vs_cback)
**
** Returns          true if any configuration is changed
**
*******************************************************************************/
static bool nfa_ee_need_recfg(void) {
  bool needed = false;
  uint32_t xx;
  tNFA_EE_ECB* p_cb;
  uint8_t mask;

  NFA_TRACE_DEBUG2("nfa_ee_need_recfg() ee_cfged: 0x%02x ee_cfg_sts: 0x%02x",
                   nfa_ee_cb.ee_cfged, nfa_ee_cb.ee_cfg_sts);
  /* if no routing/vs is configured, do not need to send the info to NFCC */
  if (nfa_ee_cb.ee_cfged || nfa_ee_cb.ee_cfg_sts) {
    if (nfa_ee_cb.ee_cfg_sts & NFA_EE_STS_CHANGED) {
      NFA_TRACE_DEBUG1("%s:Routing table dirty, update required...", __func__);
      needed = true;
    } else {
      p_cb = &nfa_ee_cb.ecb[NFA_EE_CB_4_DH];
      mask = 1 << NFA_EE_CB_4_DH;
      for (xx = 0; xx <= nfa_ee_cb.cur_ee; xx++) {
        NFA_TRACE_DEBUG3("%d: ecb_flags  : 0x%02x, mask: 0x%02x", xx,
                         p_cb->ecb_flags, mask);
        if ((p_cb->ecb_flags) && (nfa_ee_cb.ee_cfged & mask)) {
          NFA_TRACE_DEBUG1("%s:Routing table dirty, update required...",
                           __func__);
          needed = true;
          break;
        }
        p_cb = &nfa_ee_cb.ecb[xx];
        mask = 1 << xx;
      }
    }
  }

  return needed;
}

/*******************************************************************************
**
** Function         nfa_ee_rout_timeout
**
** Description      Anytime VS or routing entries are changed,
**                  a 1 second timer is started. This function is called when
**                  the timer expires or NFA_EeUpdateNow() is called.
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_rout_timeout(tNFA_EE_MSG* p_data) {
  uint8_t ee_cfged = nfa_ee_cb.ee_cfged;
  (void)p_data;

  NFA_TRACE_DEBUG0("nfa_ee_rout_timeout()");
  if (nfa_ee_need_recfg()) {
    /* Send deactivated to idle command if already not sent */
    if (nfa_dm_cb.disc_cb.disc_state != NFA_DM_RFST_IDLE)
      nci_snd_deactivate_cmd(NFC_DEACTIVATE_TYPE_IDLE);
    nfa_ee_update_rout();
  }

  if (nfa_ee_cb.wait_rsp) nfa_ee_cb.ee_wait_evt |= NFA_EE_WAIT_UPDATE_RSP;
  if (ee_cfged & NFA_EE_CFGED_UPDATE_NOW) {
    /* need to report NFA_EE_UPDATED_EVT when done updating NFCC */
    nfa_ee_cb.ee_wait_evt |= NFA_EE_WAIT_UPDATE;
    if (!nfa_ee_cb.wait_rsp) {
      nfa_ee_report_update_evt();
    }
  }
}

/*******************************************************************************
**
** Function         nfa_ee_discv_timeout
**
** Description
**
**
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_discv_timeout(tNFA_EE_MSG* p_data) {
  (void)p_data;
  NFA_TRACE_DEBUG0("nfa_ee_discv_timeout ()");
  if(NFA_GetNCIVersion() != NCI_VERSION_2_0)
      NFC_NfceeDiscover(false);
  if (nfa_ee_cb.p_enable_cback)
    (*nfa_ee_cb.p_enable_cback)(NFA_EE_DISC_STS_OFF);
}

/*******************************************************************************
**
** Function         nfa_ee_lmrt_to_nfcc
**
** Description      This function would set the listen mode routing table
**                  to NFCC.
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_lmrt_to_nfcc(tNFA_EE_MSG* p_data) {
  int xx;
  tNFA_EE_ECB* p_cb;
  uint8_t* p = NULL;
  bool more = true;
  bool check = true;
  uint8_t last_active = NFA_EE_INVALID;
  int max_len;
  tNFA_STATUS status = NFA_STATUS_FAILED;
  int cur_offset;
  uint8_t max_tlv;
#if (NXP_EXTNS == TRUE)
  int rt;
  tNFA_EE_CBACK_DATA evt_data = {0};
  (void)p_data;
#endif

#if (NXP_EXTNS == TRUE)
  if((nfcFL.chipType != pn547C2) &&
          (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
      NFA_TRACE_DEBUG1("max supported routing table size: %d",
              max_routing_table_size);
      /* update routing table: DH and the activated NFCEEs */
      p = (uint8_t*)GKI_getbuf(max_routing_table_size);
  }
  else {
      p = (uint8_t*)GKI_getbuf(NFA_EE_ROUT_BUF_SIZE_STAT);
  }
#else
  /* update routing table: DH and the activated NFCEEs */
  p = (uint8_t*)GKI_getbuf(NFA_EE_ROUT_BUF_SIZE);
#endif

  if (p == NULL) {
    NFA_TRACE_ERROR0("nfa_ee_lmrt_to_nfcc() no buffer to send routing info.");
#if (NXP_EXTNS == TRUE)
    evt_data.status = status;
    nfa_ee_report_event(NULL, NFA_EE_NO_MEM_ERR_EVT,
                        (tNFA_EE_CBACK_DATA*)&evt_data);
#else
    nfa_ee_report_event(NULL, NFA_EE_NO_MEM_ERR_EVT,
                        (tNFA_EE_CBACK_DATA*)&status);
#endif
    return;
  }

#if ((NXP_EXTNS == TRUE) && (NFC_NXP_LISTEN_ROUTE_TBL_OPTIMIZATION == TRUE))
  proto_route_buff = (uint8_t*)GKI_getbuf(
      NFA_EE_PROTO_BUFF_SIZE); /* Temporary buffer to store proto route
                                  entries */
  tech_route_buff = (uint8_t*)GKI_getbuf(
      NFA_EE_TECH_BUFF_SIZE); /* Temporary buffer to store tech route entries */

  if (proto_route_buff == NULL || tech_route_buff == NULL) {
    NFA_TRACE_ERROR0(
        "nfa_ee_lmrt_to_nfcc() no temp buffer to send routing info.");
    evt_data.status = status;
    nfa_ee_report_event(NULL, NFA_EE_NO_MEM_ERR_EVT,
                        (tNFA_EE_CBACK_DATA*)&evt_data);
    return;
  }
  proto_tlv_ctr = 0;
  tech_tlv_ctr = 0;
  proto_pp = proto_route_buff;
  tech_pp = tech_route_buff;
#endif /* - Routing entries optimization */

  /* find the last active NFCEE. */
  p_cb = &nfa_ee_cb.ecb[nfa_ee_cb.cur_ee - 1];
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb--) {
    if (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE) {
      if (last_active == NFA_EE_INVALID) {
        last_active = p_cb->nfcee_id;
        NFA_TRACE_DEBUG1("last_active: 0x%x", last_active);
      }
    }
  }
  if (last_active == NFA_EE_INVALID) {
     check = false;
  }
#if (NXP_EXTNS == TRUE)
  if(nfcFL.chipType != pn547C2) {
      find_and_resolve_tech_conflict();
  }
#endif

  max_len = NFC_GetLmrtSize();
  max_tlv =
      (uint8_t)((max_len > NFA_EE_ROUT_MAX_TLV_SIZE) ? NFA_EE_ROUT_MAX_TLV_SIZE
                                                     : max_len);
  cur_offset = 0;
  /* use the first byte of the buffer (p) to keep the num_tlv */
  *p = 0;
  for (int rt = NCI_ROUTE_ORDER_AID; rt <= NCI_ROUTE_ORDER_TECHNOLOGY; rt++) {
    /* add the routing entries for NFCEEs */
    p_cb = &nfa_ee_cb.ecb[0];
    for (xx = 0; (xx < nfa_ee_cb.cur_ee) && check; xx++, p_cb++) {
      if (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE) {
        NFA_TRACE_DEBUG1("%s --add the routing for NFCEEs!!", __func__);
        nfa_ee_route_add_one_ecb_by_route_order(p_cb, rt, &max_len, more, p,
                                                &cur_offset);
      }
    }
    if (rt == NCI_ROUTE_ORDER_TECHNOLOGY) more = false;
    /* add the routing entries for DH */
    NFA_TRACE_DEBUG1("%s --add the routing for DH!!", __func__);
    nfa_ee_route_add_one_ecb_by_route_order(&nfa_ee_cb.ecb[NFA_EE_CB_4_DH], rt,
                                            &max_len, more, p, &cur_offset);
  }
#if (NXP_EXTNS == TRUE)
  nfa_ee_cb.ee_flags &= ~NFA_EE_FLAG_CFG_NFC_DEP;
  evt_data.status = status;
  if (status != NFA_STATUS_OK) {
    nfa_ee_report_event(NULL, NFA_EE_ROUT_ERR_EVT,
                        (tNFA_EE_CBACK_DATA*)&evt_data);
  }
#endif

  GKI_freebuf(p);
#if ((NXP_EXTNS == TRUE) && (NFC_NXP_LISTEN_ROUTE_TBL_OPTIMIZATION == TRUE))
  GKI_freebuf(proto_route_buff);
  GKI_freebuf(tech_route_buff);
  proto_tlv_ctr = 0;
  tech_tlv_ctr = 0;
  proto_pp = 0;
  tech_pp = 0;
#endif /* - Routing entries optimization */
}

/*******************************************************************************
**
** Function         find_and_resolve_tech_conflict
**
** Description      This function prefer the SE to resolve the technology
*conflict.
**
** Returns          void
**
*******************************************************************************/

void find_and_resolve_tech_conflict() {
    if(nfcFL.chipType == pn547C2) {
        NFA_TRACE_DEBUG1("%s: chipType : pn547C2. Returning", __func__);
    }
  int yy, xx;
  tNFA_EE_ECB* p_cb = nfa_ee_cb.ecb;
  uint8_t techA_found = 0, techF_found = 0;
  uint8_t techA_ee = 0, techF_ee = 0;
  unsigned long preferred_se = 0x01;
  bool conflict = false;
  uint8_t tech_to_rm, ee_from_rm;

  NFA_TRACE_DEBUG1("%s:Enter", __func__);

  // Finding the Technology and nfcee_id supported
  for (yy = 0; yy < nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED; yy++, p_cb++) {
    for (xx = 0; xx < 3; xx++) {
      if ((p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_A) {
        techA_found |= NFA_TECHNOLOGY_MASK_A;
        techA_ee = p_cb->nfcee_id;
      }
      if ((p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_A) {
        techA_found |= NFA_TECHNOLOGY_MASK_A;
        techA_ee = p_cb->nfcee_id;
      }
      if ((p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_A) {
        techA_found |= NFA_TECHNOLOGY_MASK_A;
        techA_ee = p_cb->nfcee_id;
      }
      if ((p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_F) {
        techF_found |= NFA_TECHNOLOGY_MASK_F;
        techF_ee = p_cb->nfcee_id;
      }
      if ((p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_F) {
        techF_found |= NFA_TECHNOLOGY_MASK_F;
        techF_ee = p_cb->nfcee_id;
      }
      if ((p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx]) ==
          NFA_TECHNOLOGY_MASK_F) {
        techF_found |= NFA_TECHNOLOGY_MASK_F;
        techF_ee = p_cb->nfcee_id;
      }
    }
  }

  NFA_TRACE_DEBUG5(
      "%s:p_cb->nfcee_id=0x%x,p_cb->tech_switch_on= "
      "0x%x,p_cb->tech_switch_off= 0x%x,p_cb->tech_battery_off= 0x%x",
      __func__, p_cb->nfcee_id, p_cb->tech_switch_on, p_cb->tech_switch_off,
      p_cb->tech_battery_off);

  // Preferred SE Selected.
  if ((GetNumValue(NAME_DEFAULT_OFFHOST_ROUTE, &preferred_se,
                   sizeof(preferred_se)))) {
    NFA_TRACE_DEBUG2("%s:NXP_DEFAULT_OFFHOST_ROUTE=0x0%lu;", __func__,
                     preferred_se);
    if (preferred_se == 0x01)
      preferred_se = 0xc0;  // Ese
    else if (preferred_se == 0x02)
      preferred_se = 0x02;  // UICC
  }
  NFA_TRACE_DEBUG3("%s:techF_found=0x%x,techF_ee= 0x%x;", __func__, techF_found,
                   techF_ee);
  NFA_TRACE_DEBUG3("%s:techA_found=0x%x,techA_ee= 0x%x;", __func__, techA_found,
                   techA_ee);

  if (techA_found == NFA_TECHNOLOGY_MASK_A &&
      techF_found == NFA_TECHNOLOGY_MASK_F) {
    if (techA_ee != 0x00 && techF_ee != 0x00) {
      // Conflict occurs when techF and techA on Different SE.
      if (techA_ee != techF_ee) conflict = true;
    }
  }

  if (conflict == true) {
    NFA_TRACE_DEBUG0("Conflict true");

    if (techF_ee == preferred_se) {
      tech_to_rm = NFA_TECHNOLOGY_MASK_A;
      ee_from_rm = techA_ee;
    } else {
      tech_to_rm = NFA_TECHNOLOGY_MASK_F;
      ee_from_rm = techF_ee;
    }

    p_cb = nfa_ee_cb.ecb;
    for (yy = 0; yy < nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED; yy++, p_cb++) {
      if (p_cb->nfcee_id == ee_from_rm) {
        for (xx = 0; xx < 3; xx++) {
          if (((p_cb->tech_switch_on & nfa_ee_tech_mask_list[xx]) ==
               tech_to_rm))
            p_cb->tech_switch_on &= ~tech_to_rm;
          if ((p_cb->tech_switch_off & nfa_ee_tech_mask_list[xx]) == tech_to_rm)
            p_cb->tech_switch_off &= ~tech_to_rm;
          if ((p_cb->tech_battery_off & nfa_ee_tech_mask_list[xx]) ==
              tech_to_rm)
            p_cb->tech_battery_off &= ~tech_to_rm;
        }
      }
    }
    NFA_TRACE_DEBUG1("%s:Exit", __func__);
  }
}

/*******************************************************************************
**
** Function         nfa_ee_update_rout
**
** Description      This function would set the VS and listen mode routing table
**                  to NFCC.
**
** Returns          void
**
*******************************************************************************/
void nfa_ee_update_rout(void) {
  int xx;
  tNFA_EE_ECB* p_cb;
  uint8_t mask;
  NFC_HDR msg;

  NFA_TRACE_DEBUG1("nfa_ee_update_rout ee_cfg_sts:0x%02x",
                   nfa_ee_cb.ee_cfg_sts);

  /* use action function to send routing and VS configuration to NFCC */
  msg.event = NFA_EE_CFG_TO_NFCC_EVT;
  nfa_ee_evt_hdlr(&msg);

  /* all configuration is updated to NFCC, clear the status mask */
  nfa_ee_cb.ee_cfg_sts &= NFA_EE_STS_PREV;
  nfa_ee_cb.ee_cfged = 0;
  p_cb = &nfa_ee_cb.ecb[0];
  for (xx = 0; xx < NFA_EE_NUM_ECBS; xx++, p_cb++) {
    p_cb->ecb_flags = 0;
    mask = (1 << xx);
    if (p_cb->tech_switch_on | p_cb->tech_switch_off | p_cb->tech_battery_off |
        p_cb->proto_switch_on | p_cb->proto_switch_off |
        p_cb->proto_battery_off | p_cb->aid_entries | p_cb->apdu_pattern_entries) {
      /* this entry has routing configuration. mark it configured */
      nfa_ee_cb.ee_cfged |= mask;
    }
  }
  NFA_TRACE_DEBUG2("nfa_ee_update_rout ee_cfg_sts:0x%02x ee_cfged:0x%02x",
                   nfa_ee_cb.ee_cfg_sts, nfa_ee_cb.ee_cfged);
}

#if (NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function         nfa_ee_lmrt_size
**
** Description      This function is called to get the AID routing table size.
**
** Returns          AID routing table currently used size.
**
*******************************************************************************/
uint16_t nfa_ee_lmrt_size() {
  NFA_TRACE_DEBUG0("nfa_ee_lmrt_size");
  int len;
  len = nfa_all_ee_find_total_aid_len() + 2 /* tag/len */ +
        2 /*route/power state*/;
  if((nfcFL.chipType != pn547C2) &&
          (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
      return len < max_aid_config_length ? len : max_aid_config_length;
  }
  else {
      return len < NFA_EE_MAX_AID_CFG_LEN_STAT ? len : NFA_EE_MAX_AID_CFG_LEN_STAT;
  }
}

bool nfa_ee_nfeeid_active(uint8_t nfee_id) {
  int xx;
  tNFA_EE_ECB* p_cb;
  bool status = false;
  p_cb = &nfa_ee_cb.ecb[0];
  for (xx = 0; xx < nfa_ee_cb.cur_ee; xx++, p_cb++) {
    if ((p_cb->nfcee_id == nfee_id) &&
        (p_cb->ee_status == NFC_NFCEE_STATUS_ACTIVE)) {
      status = true;
      break;
    }
  }
  return status;
}

/*******************************************************************************
**
** Function         nfa_ee_find_max_aid_config_length
**
** Description      This function is called to find the max AID config length.
**
** Returns          max AID config length.
**
*******************************************************************************/
uint16_t nfa_ee_find_max_aid_config_length() {
    NFA_TRACE_DEBUG0("nfa_ee_find_max_aid_config_length");
  /**
   * Max Routing Table Size = 720
   * After allocating size for Technology based routing and Protocol based
   *routing,
   * the remaining size can be used for AID based routing
   *
   * Size for 1 Technology route entry = 5 bytes (includes Type(1 byte),
   * Length (1 byte), Value (3 bytes - Power state, Tech Type, Location)
   * TOTAL TECH ROUTE SIZE = 5 * 3  = 15 (For Tech A, B, F)
   *
   * Size for 1 Protocol route entry = 5 bytes (includes Type(1 byte),
   * Length (1 byte), Value (3 bytes - Power state, Tech Type, Location)
   * TOTAL PROTOCOL ROUTE SIZE = 5 * 6 = 30 (Protocols ISO-DEP, NFC-DEP,
   *ISO-7816, T1T, T2T, T3T)
   *
   * SIZE FOR AID = 720 - 15 - 30 = 675
   * BUFFER for future extensions = 15
   * TOTAL SIZE FOR AID = 675 - 15 = 660
   */
    if((nfcFL.chipType != pn547C2) &&
            (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
        return (max_routing_table_size -
                NFA_EE_TOTAL_PROTO_TECH_FUTURE_EXT_ROUTE_SIZE);
    }
    else{
        NFA_TRACE_DEBUG0("Not allowed for chip type. Returning");
        return 0;
    }
}

/*******************************************************************************
**
** Function         nfa_ee_get_max_aid_config_length
**
** Description      This function is called to get the max AID config length.
**
** Returns          max AID config length.
**
*******************************************************************************/
uint16_t nfa_ee_api_get_max_aid_config_length() {
    NFA_TRACE_DEBUG0("nfa_ee_api_get_max_aid_config_length");
    if((nfcFL.chipType != pn547C2) &&
            (nfcFL.nfcMwFL._NFC_NXP_AID_MAX_SIZE_DYN == true)) {
        NFA_TRACE_DEBUG1("nfa_ee_api_get_max_aid_config_length: %d",
                max_aid_config_length);
        return max_aid_config_length;
    }
    else{
        NFA_TRACE_DEBUG0("Not allowed for chip type. Returning");
        return 0;
    }
}
#endif
