/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * (C) Copyright 2015-2016 Hewlett Packard Enterprise Development Company, L.P.
 * (C) Copyright 2016 Broadcom Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Hewlett-Packard Company Confidential (C) Copyright 2015 Hewlett-Packard Development Company, L.P.
 */

#include <errno.h>

#include <seq.h>
#include <coverage.h>
#include <vlan-bitmap.h>
#include <ofproto/ofproto-provider.h>
#include <ofproto/bond.h>
#include <ofproto/tunnel.h>
#include <openvswitch/vlog.h>

#include <vswitch-idl.h>
#include <openswitch-idl.h>

#include "ofproto-bcm-provider.h"
#include "ops-pbmp.h"
#include "ops-vlan.h"
#include "ops-lag.h"
#include "ops-routing.h"
#include "ops-knet.h"
#include "ops-sflow.h"
#include "netdev-bcmsdk.h"
#include "platform-defines.h"
#include "plugin-extensions.h"
#include "ofproto-bcm-provider.h"
#include "ops-stg.h"
#include "ops-qos.h"
#include "qos-asic-provider.h"
#include "ops-mac-learning.h"
#include "ops-mirrors.h"
#include "eventlog.h"
#include "ops-classifier.h"
#include "ofproto-ofdpa.h"

#define DEFAULT_VID  (1)

VLOG_DEFINE_THIS_MODULE(ofproto_bcm_provider);

COVERAGE_DEFINE(ofproto_bcm_provider_expired);
COVERAGE_DEFINE(rev_reconfigure_bcm);
COVERAGE_DEFINE(rev_bond_bcm);
COVERAGE_DEFINE(rev_port_toggled_bcm);

static void rule_get_stats(struct rule *, uint64_t *packets,
                           uint64_t *bytes, long long int *used);
static void bundle_remove(struct ofport *);
static struct bcmsdk_provider_ofport_node *get_ofp_port(
                          const struct bcmsdk_provider_node *ofproto,
                          ofp_port_t ofp_port);

static void available_vrf_ids_init(void);
static size_t allocate_vrf_id(void);
static void release_vrf_id(size_t);
static void port_unconfigure_ips(struct ofbundle *bundle);

/* vrf id avalability bitmap */
static unsigned long *available_vrf_ids = NULL;

static void
available_vrf_ids_init() {
    available_vrf_ids = bitmap_allocate(BCM_MAX_VRFS);
}

static size_t
allocate_vrf_id() {
    size_t vrf_id;

    vrf_id = bitmap_scan(available_vrf_ids, 0, 0, BCM_MAX_VRFS);
    if (vrf_id == BCM_MAX_VRFS) {
        VLOG_DBG("Couldn't allocate VRF ID\n");

        return BCM_MAX_VRFS;
    }

    bitmap_set1(available_vrf_ids, vrf_id);
    return vrf_id;
}

static void
release_vrf_id(size_t vrf_id) {
    bitmap_set0(available_vrf_ids, vrf_id);
}

struct bcmsdk_provider_ofport_node *
bcmsdk_provider_ofport_node_cast(const struct ofport *ofport)
{
    return ofport ?
           CONTAINER_OF(ofport, struct bcmsdk_provider_ofport_node, up) : NULL;
}

/*
 * used externally, do NOT make static
 */
inline struct bcmsdk_provider_node *
bcmsdk_provider_node_cast(const struct ofproto *ofproto)
{
    ovs_assert(ofproto->ofproto_class == &ofproto_bcm_provider_class);
    return CONTAINER_OF(ofproto, struct bcmsdk_provider_node, up);
}

/* All existing ofproto provider instances, indexed by ->up.name. */
static struct hmap all_bcmsdk_provider_nodes =
              HMAP_INITIALIZER(&all_bcmsdk_provider_nodes);

static int ofproto_is_ofdpa(const struct ofproto *ofproto)
{
    int is_ofdpa = 0;

    if (ofproto) {
        if (strcmp(ofproto->type, OFDPA_DATAPATH_TYPE) == 0) {
            is_ofdpa = 1;
        }
    }
    return(is_ofdpa);
}

static enum ofperr ovs_ofdpa_match_fields_masks_get(const struct match *match, ofdpaFlowEntry_t *flow)
{
    enum ofperr err = 0;
    uint16_t vid, vid_mask;

    switch (flow->tableId) {
    case OFDPA_FLOW_TABLE_ID_VLAN:
        flow->flowData.vlanFlowEntry.match_criteria.inPort = match->flow.in_port.ofp_port;
        if (match->wc.masks.in_port.ofp_port == 0xFFFF) {
            flow->flowData.vlanFlowEntry.match_criteria.inPortMask = OFDPA_INPORT_EXACT_MASK;
        } else {
            flow->flowData.vlanFlowEntry.match_criteria.inPortMask = match->wc.masks.in_port.ofp_port;
        }

        /* DEI bit indicating 'present' is included in the VID match field */
        vid = ntohs(match->flow.vlan_tci) & (VLAN_VID_MASK | VLAN_CFI);
        vid_mask = ntohs(match->wc.masks.vlan_tci) & (VLAN_VID_MASK | VLAN_CFI);

        flow->flowData.vlanFlowEntry.match_criteria.vlanId = vid;

        if (vid_mask != 0) {
            if (vid == OFDPA_VID_PRESENT) {   /* All */
                flow->flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_PRESENT;
            } else if ((vid & OFDPA_VID_EXACT_MASK) == OFDPA_VID_NONE) {      /* untagged */
                flow->flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_EXACT_MASK;
            } else {     /* tagged */
                flow->flowData.vlanFlowEntry.match_criteria.vlanIdMask = (OFDPA_VID_PRESENT | OFDPA_VID_EXACT_MASK);
            }
        } else {         /* ALL */
            flow->flowData.vlanFlowEntry.match_criteria.vlanId = OFDPA_VID_NONE;
            flow->flowData.vlanFlowEntry.match_criteria.vlanIdMask = OFDPA_VID_FIELD_MASK;
        }

        break;

    case OFDPA_FLOW_TABLE_ID_BRIDGING:
        flow->flowData.bridgingFlowEntry.match_criteria.tunnelId     = ntohll(match->flow.tunnel.tun_id);
        flow->flowData.bridgingFlowEntry.match_criteria.tunnelIdMask = ntohll(match->wc.masks.tunnel.tun_id);

        memcpy(&flow->flowData.bridgingFlowEntry.match_criteria.destMac, &match->flow.dl_dst, OFDPA_MAC_ADDR_LEN);
        memcpy(&flow->flowData.bridgingFlowEntry.match_criteria.destMacMask, &match->wc.masks.dl_dst, OFDPA_MAC_ADDR_LEN);

        break;

    default:
        VLOG_ERR("Invalid table id %d", flow->tableId);
        err = OFPERR_OFPBRC_BAD_TYPE;
        break;
    }

    return err;
}

static enum ofperr ovs_ofdpa_translate_openflow_actions(const struct ofpact *act, ofdpaFlowEntry_t *flow)
{
    ofp_port_t port_no;

    switch (act->type) {
    case OFPACT_OUTPUT:
    {
        port_no = ofpact_get_OUTPUT(act)->port;

        switch (port_no) {
        case OFPP_CONTROLLER:
        {
            switch (flow->tableId) {
            case OFDPA_FLOW_TABLE_ID_BRIDGING:
                flow->flowData.bridgingFlowEntry.outputPort = OFDPA_PORT_CONTROLLER;
                break;
            default:
                VLOG_ERR("Unsupported output port action (OFPP_CONTROLLER) for Table: %d", flow->tableId);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            }

            break;
        }

        case OFPP_LOCAL:
        case OFPP_FLOOD:
        case OFPP_ALL:
        case OFPP_TABLE:
        case OFPP_IN_PORT:
        case OFPP_NORMAL:
            VLOG_ERR("Unsupported port type for output port action 0x%x", port_no);
            return OFPERR_OFPBIC_UNSUP_INST;

        default:
            /* Physical or logical port as output port */

            /* NOTE: currently no flow entry types support output to physical or logical port (only OFPP_CONTROLLER) */
            VLOG_ERR("Unsupported port type for output port action 0x%x", port_no);
            return OFPERR_OFPBIC_UNSUP_INST;
            break;
        }
        break;
    }

    case OFPACT_SET_FIELD:
    {
        enum mf_field_id id = ofpact_get_SET_FIELD(act)->field->id;
        switch (id) {
        case MFF_TUN_ID:
        {
            ovs_be64 tunnel_id;

            tunnel_id = ntohll(ofpact_get_SET_FIELD(act)->value.be64);

            if (flow->tableId == OFDPA_FLOW_TABLE_ID_VLAN) {
                flow->flowData.vlanFlowEntry.tunnelIdAction = 1;
                flow->flowData.vlanFlowEntry.tunnelId = tunnel_id;
            } else {
                VLOG_ERR("Unsupported set-field %d for table %d", id, flow->tableId);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            }

            break;
        }

        default:
            VLOG_ERR("Unsupported set-field %d for table %d", id, flow->tableId);
            return OFPERR_OFPHFC_INCOMPATIBLE;
        }
        break;
    }

    case OFPACT_GROUP:
    {
        uint32_t group_id;

        group_id = ofpact_get_GROUP(act)->group_id;

        switch (flow->tableId) {
        case OFDPA_FLOW_TABLE_ID_BRIDGING:
            flow->flowData.bridgingFlowEntry.groupID = group_id;
            break;

        default:
            VLOG_ERR("Unsupported action: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
            return OFPERR_OFPHFC_INCOMPATIBLE;
        }
        break;
    }

    default:
        VLOG_ERR("Unsupported action: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
        return OFPERR_OFPHFC_INCOMPATIBLE;
    }

    return 0;
}

static enum ofperr
ovs_ofdpa_instructions_get(const struct rule_actions *rule_action, ofdpaFlowEntry_t *flow)
{
    enum ofperr err;
    uint8_t next_table_id;
    const struct ofpact *act;
    const struct ofpact *write_act;

    OFPACT_FOR_EACH(act, rule_action->ofpacts, rule_action->ofpacts_len) {
        switch (ovs_instruction_type_from_ofpact_type(act->type)) {
        case OVSINST_OFPIT11_WRITE_ACTIONS:
            switch (flow->tableId) {
            case OFDPA_FLOW_TABLE_ID_VLAN:
            case OFDPA_FLOW_TABLE_ID_BRIDGING:
                break;
            default:
                VLOG_ERR("Unsupported instruction: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            }
            write_act = ofpact_get_WRITE_ACTIONS(act)->actions;
            err = ovs_ofdpa_translate_openflow_actions(write_act, flow);
            if (err != 0) {
                return err;
            }
            break;

        case OVSINST_OFPIT11_GOTO_TABLE:
            next_table_id = ofpact_get_GOTO_TABLE(act)->table_id;
            switch (flow->tableId) {
            case OFDPA_FLOW_TABLE_ID_VLAN:
                flow->flowData.vlanFlowEntry.gotoTableId = next_table_id;
                break;
            case OFDPA_FLOW_TABLE_ID_BRIDGING:
                flow->flowData.bridgingFlowEntry.gotoTableId = next_table_id;
                break;
            default:
                VLOG_ERR("Unsupported instruction: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            }
            break;

        case OVSINST_OFPIT11_APPLY_ACTIONS:
            switch (flow->tableId) {
            case OFDPA_FLOW_TABLE_ID_INGRESS_PORT:
            case OFDPA_FLOW_TABLE_ID_VLAN:
            case OFDPA_FLOW_TABLE_ID_TERMINATION_MAC:
            case OFDPA_FLOW_TABLE_ID_BRIDGING:
            case OFDPA_FLOW_TABLE_ID_ACL_POLICY:
                break;
            default:
                VLOG_ERR("Unsupported instruction: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            }

            err = ovs_ofdpa_translate_openflow_actions(act, flow);
            if (err != 0) {
                return err;
            }
            break;
        default:
            VLOG_ERR("Unsupported instruction: '%s' (%d), table %d", ofpact_name(act->type), act->type, flow->tableId);
            return OFPERR_OFPHFC_INCOMPATIBLE;
        }
    }
    return 0;
}

static enum ofperr
ofdpa_flow_add(struct rule *rule_)
{
    enum ofperr err;
    struct match megamatch;
    ofdpaFlowEntry_t flow;
    OFDPA_ERROR_t ofdpa_rv = OFDPA_E_NONE;

    memset(&flow, 0, sizeof(flow));
    minimatch_expand(&rule_->cr.match, &megamatch);
    flow.tableId   = rule_->table_id;
    flow.hard_time = rule_->hard_timeout;
    flow.idle_time = rule_->idle_timeout;
    flow.priority  = rule_->cr.priority;
    flow.cookie    = rule_->flow_cookie;

    /* Get the match fields and masks from LOCI match structure */
    err = ovs_ofdpa_match_fields_masks_get(&megamatch, &flow);
    if (err) {
        VLOG_ERR("Error getting match fields and masks. (err = %d)", err);
        return err;
    }

    /* Get the instructions set from the flow add object */
    err = ovs_ofdpa_instructions_get(rule_->actions, &flow);
    if (err) {
        VLOG_ERR("Failed to get flow instructions. (err = 0x%8x)", err);
        return err;
    }

    /* Submit the changes to ofdpa */
    ofdpa_rv = ofdpaFlowAdd(&flow);
    if (ofdpa_rv != OFDPA_E_NONE) {
        VLOG_ERR("Failed to add flow. (ofdpa_rv = %d)", ofdpa_rv);
    } else {
        VLOG_DBG("Flow added successfully. (ofdpa_rv = %d)", ofdpa_rv);
    }

    if (ofdpa_rv == OFDPA_E_NONE)
        return 0;
    else
        return OFPERR_OFPHFC_INCOMPATIBLE;
}

static enum
ofperr ofdpa_flow_modify(struct rule *rule_)
{
    enum ofperr err;
    struct match megamatch;
    ofdpaFlowEntry_t flow;
    OFDPA_ERROR_t ofdpa_rv = OFDPA_E_NONE;

    memset(&flow, 0, sizeof(flow));

    minimatch_expand(&rule_->cr.match, &megamatch);
    flow.tableId   = rule_->table_id;
    flow.hard_time = rule_->hard_timeout;
    flow.idle_time = rule_->idle_timeout;
    flow.priority  = rule_->cr.priority;
    flow.cookie    = rule_->flow_cookie;

    /* Get the match fields and masks from LOCI match structure */
    err = ovs_ofdpa_match_fields_masks_get(&megamatch, &flow);
    if (err) {
        VLOG_ERR("Error getting match fields and masks. (err = %d)", err);
        return err;
    }

    /* Get the instructions set from the flow add object */
    err = ovs_ofdpa_instructions_get(rule_->actions, &flow);
    if (err) {
        VLOG_ERR("Failed to get flow instructions. (err = 0x%8x)", err);
        return err;
    }

    /* Submit the changes to ofdpa */
    ofdpa_rv = ofdpaFlowModify(&flow);
    if (ofdpa_rv != OFDPA_E_NONE) {
        VLOG_ERR("Failed to modify flow. (ofdpa_rv = %d)", ofdpa_rv);
    } else {
        VLOG_DBG("Flow modified successfully. (ofdpa_rv = %d)", ofdpa_rv);
    }

    if (ofdpa_rv == OFDPA_E_NONE)
        return 0;
    else
        return OFPERR_OFPHFC_INCOMPATIBLE;
}

enum ofperr ofdpa_flow_delete(struct rule *rule_)
{
    enum ofperr err;
    struct match megamatch;
    ofdpaFlowEntry_t flow;
    OFDPA_ERROR_t ofdpa_rv = OFDPA_E_NONE;

    memset(&flow, 0, sizeof(flow));

    minimatch_expand(&rule_->cr.match, &megamatch);
    flow.tableId   = rule_->table_id;
    flow.hard_time = rule_->hard_timeout;
    flow.idle_time = rule_->idle_timeout;
    flow.priority  = rule_->cr.priority;
    flow.cookie    = rule_->flow_cookie;


    /* Get the match fields and masks from LOCI match structure */
    err = ovs_ofdpa_match_fields_masks_get(&megamatch, &flow);
    if (err) {
        VLOG_ERR("Error getting match fields and masks. (err = %d)", err);
        return err;
    }

    /* Get the instructions set from the LOCI flow add object */
    err = ovs_ofdpa_instructions_get(rule_->actions, &flow);
    if (err) {
        VLOG_ERR("Failed to get flow instructions. (err = 0x%8x)", err);
        return err;
    }

    /* Delete the flow entry */
    ofdpa_rv = ofdpaFlowDelete(&flow);
    if (ofdpa_rv != OFDPA_E_NONE) {
        VLOG_ERR("Failed to delete flow. (ofdpa_rv = %d)", ofdpa_rv);
    } else {
        VLOG_DBG("Flow deleted successfully. (ofdpa_rv = %d)", ofdpa_rv);
    }

    if (ofdpa_rv == OFDPA_E_NONE)
        return 0;
    else
        return OFPERR_OFPHFC_INCOMPATIBLE;
}

static enum ofperr
ovs_ofdpa_translate_group_actions(struct ofputil_bucket *bucket,
                                  ovs_ofdpa_group_bucket_t *group_bucket,
                                  uint64_t *group_action_bitmap,
                                  uint64_t *group_action_sf_bitmap)
{
    ofp_port_t port_no;
    const struct ofpact *act;

    OFPACT_FOR_EACH(act, bucket->ofpacts, bucket->ofpacts_len) {
        switch (act->type) {
        case OFPACT_OUTPUT:
        {
            port_no = ofpact_get_OUTPUT(act)->port;
            switch (port_no) {
            case OFPP_CONTROLLER:
            case OFPP_FLOOD:
            case OFPP_ALL:
            case OFPP_TABLE:
            case OFPP_LOCAL:
            case OFPP_IN_PORT:
            case OFPP_NORMAL:
                VLOG_ERR("Unsupported port for group output port action 0x%x", port_no);
                return OFPERR_OFPHFC_INCOMPATIBLE;
            default:
                group_bucket->outputPort = port_no;
                break;
            }
            break;
        }

        case OFPACT_STRIP_VLAN: /*OF_ACTION_POP_VLAN*/
            group_bucket->popVlanTag = 1;
            break;

        default:
            VLOG_ERR("unsupported action type %d", act->type);
            return OFPERR_OFPHFC_INCOMPATIBLE;
        }
    }

    VLOG_DBG("group_action_bitmap is 0x%" PRIx64 ", group_action_sf_bitmap is 0x%" PRIx64 "",
             *group_action_bitmap, *group_action_sf_bitmap);

    return 0;
}

static enum ofperr
ovs_ofdpa_translate_group_buckets(uint32_t group_id,
                                  struct ovs_list *of_buckets,
                                  int is_add_cmd)
{
    enum ofperr err;
    uint16_t bucket_index = 0;
    ovs_ofdpa_group_bucket_t group_bucket;
    uint32_t group_type;
    ofdpaGroupEntry_t group_entry;
    ofdpaGroupBucketEntry_t group_bucket_entry;
    OFDPA_ERROR_t ofdpa_rv = OFDPA_E_FAIL;
    int group_added = 0;

    uint64_t group_action_bitmap = 0;
    uint64_t group_action_sf_bitmap = 0;
    struct ofputil_bucket *bucket;

    LIST_FOR_EACH(bucket, list_node, of_buckets) {
        VLOG_DBG("bucket weight %d watch_port %d watch_group %d act_length %zd",
                 bucket->weight, bucket->watch_port,
                 bucket->watch_group, bucket->ofpacts_len);

        memset(&group_bucket, 0, sizeof(group_bucket));

        err = ovs_ofdpa_translate_group_actions(bucket, &group_bucket, &group_action_bitmap, &group_action_sf_bitmap);

        if (err < 0) {
            VLOG_ERR("Error in translating group actions error %d", err);
            if (group_added == 1) {
                /* Delete the added group */
                (void)ofdpaGroupDelete(group_id);
            }
            return err;
        }

        ofdpaGroupTypeGet(group_id, &group_type);

        memset(&group_bucket_entry, 0, sizeof(group_bucket_entry));
        group_bucket_entry.groupId = group_id;
        group_bucket_entry.bucketIndex = bucket_index;

        switch (group_type) {

        case OFDPA_GROUP_ENTRY_TYPE_L2_INTERFACE:
            group_bucket_entry.bucketData.l2Interface.outputPort = group_bucket.outputPort;
            group_bucket_entry.bucketData.l2Interface.popVlanTag = group_bucket.popVlanTag;

            break;

        default:
            err = OFPERR_OFPHFC_EPERM;
            VLOG_ERR("Invalid GROUP_TYPE %d", group_type);
            break;
        }

        if (err) {
            if (err == OFPERR_OFPHFC_INCOMPATIBLE) {
                VLOG_ERR("Incompatible fields for Group Type err %d", err);
            }
            if (group_added == 1) {
                /* Delete the added group */
                (void)ofdpaGroupDelete(group_id);
            }
            return err;
        }

        if (is_add_cmd == true) {
            if (bucket_index == 0) {
                /* First Bucket; Add Group first*/
                group_entry.groupId = group_id;
                ofdpa_rv = ofdpaGroupAdd(&group_entry);
                if (ofdpa_rv != OFDPA_E_NONE) {
                    VLOG_ERR("Error in adding Group, rv = %d", ofdpa_rv);
                    break;
                }
                group_added = 1;
            }
        } else {      /* OF_GROUP_MODIFY */
            if (bucket_index == 0) {
                /* First Bucket; Delete all buckets first */
                ofdpa_rv = ofdpaGroupBucketsDeleteAll(group_id);
                if (ofdpa_rv != OFDPA_E_NONE) {
                    VLOG_ERR("Error in deleting Group buckets, rv = %d", ofdpa_rv);
                    break;
                }
            }
        }

        ofdpa_rv = ofdpaGroupBucketEntryAdd(&group_bucket_entry);
        if (ofdpa_rv != OFDPA_E_NONE) {
            VLOG_ERR("Error in adding Group bucket, rv = %d",ofdpa_rv);
            if (group_added == 1) {
                /* Delete the added group */
                (void)ofdpaGroupDelete(group_id);
            } else {    /* OF_GROUP_MODIFY */
                /* Need to clean up and delete Group here as well.
                Will be done by the caller as the Group also needs to
                be deleted from the Indigo database. */
            }
            break;
        }

        bucket_index++;
    }

    if (ofdpa_rv == OFDPA_E_NONE)
        return 0;
    else
        return OFPERR_OFPHFC_INCOMPATIBLE;
}

enum ofperr ofdpa_group_add(struct bcmsdk_provider_group *group)
{
    if (group->up.type != OFPGT11_ALL) {
        return OFPERR_OFPBIC_UNSUP_INST;
    }

    return ovs_ofdpa_translate_group_buckets(group->up.group_id, &group->up.buckets, 1 /*add*/);
}

enum ofperr ofdpa_group_modify(struct bcmsdk_provider_group *group)
{
    return ovs_ofdpa_translate_group_buckets(group->up.group_id, &group->up.buckets, 0 /*modify*/);
}

void ofdpa_group_delete(uint32_t id)
{
    OFDPA_ERROR_t ofdpa_rv;

    ofdpa_rv = ofdpaGroupDelete(id);
    if (ofdpa_rv != OFDPA_E_NONE) {
        VLOG_ERR("Group Delete failed, rv = %d",ofdpa_rv);
    }

    return;
}

/* Factory functions. */

static void
init(const struct shash *iface_hints)
{
    VLOG_DBG("%s::%d init %p", __FUNCTION__, __LINE__, iface_hints);
    available_vrf_ids_init();
    return;
}

static void
enumerate_types(struct sset *types)
{
    sset_add(types, "system");
    sset_add(types, "vrf");
    sset_add(types, "ofdpa");
}

static int
enumerate_names(const char *type, struct sset *names)
{
    struct bcmsdk_provider_node *ofproto;

    sset_clear(names);
    HMAP_FOR_EACH (ofproto,
                   all_bcmsdk_provider_node, &all_bcmsdk_provider_nodes) {
        if (strcmp(type, ofproto->up.type)) {
            continue;
        }

        sset_add(names, ofproto->up.name);
        VLOG_DBG("Enumerating bridge %s for type %s", ofproto->up.name, type);
    }

    return 0;
}

static int
del(const char *type OVS_UNUSED, const char *name OVS_UNUSED)
{
    return 0;
}

static const char *
port_open_type(const char *datapath_type OVS_UNUSED, const char *port_type)
{
    if ((strcmp(port_type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0) ||
        (strcmp(port_type, OVSREC_INTERFACE_TYPE_LOOPBACK) == 0) ||
        (strcmp(port_type, OVSREC_INTERFACE_TYPE_VLANSUBINT) == 0)) {
        return port_type;
    } else {
        return "system";
    }
}

/* Basic life-cycle. */

static struct ofproto *
alloc(void)
{
    struct bcmsdk_provider_node *ofproto = xmalloc(sizeof *ofproto);
    return &ofproto->up;
}

static void
dealloc(struct ofproto *ofproto_)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    free(ofproto);
}

static int
construct(struct ofproto *ofproto_)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    int error=0;

    VLOG_DBG("constructing ofproto - %s", ofproto->up.name);

    if (!strcmp(ofproto_->type, "vrf")) {
        ofproto->vrf = true;
        ofproto->vrf_id = allocate_vrf_id();
        if (ofproto->vrf_id == BCM_MAX_VRFS) {
            error = 1;
            goto out;
        }

        VLOG_DBG("Allocated VRF ID %zu for VRF %s\n",
                 ofproto->vrf_id, ofproto_->name);
    } else {
        ofproto->vrf = false;
    }

    ofproto->netflow = NULL;
    ofproto->stp = NULL;
    ofproto->rstp = NULL;
    ofproto->dump_seq = 0;
    hmap_init(&ofproto->bundles);
    ofproto->ms = NULL;
    ofproto->has_bonded_bundles = false;
    ofproto->lacp_enabled = false;
    ofproto_tunnel_init();
    ovs_mutex_init_adaptive(&ofproto->stats_mutex);
    ovs_mutex_init(&ofproto->vsp_mutex);

    guarded_list_init(&ofproto->pins);

    sset_init(&ofproto->ports);
    sset_init(&ofproto->ghost_ports);
    sset_init(&ofproto->port_poll_set);
    ofproto->port_poll_errno = 0;
    ofproto->change_seq = 0;
    ofproto->pins_seq = seq_create();
    ofproto->pins_seqno = seq_read(ofproto->pins_seq);

    hmap_insert(&all_bcmsdk_provider_nodes, &ofproto->all_bcmsdk_provider_node,
                hash_string(ofproto->up.name, 0));
    memset(&ofproto->stats, 0, sizeof ofproto->stats);

    ofproto_init_tables(ofproto_, N_TABLES);

    ofproto->up.tables[TBL_INTERNAL].flags = OFTABLE_HIDDEN | OFTABLE_READONLY;

out:
    return error;
}

static void
destruct(struct ofproto *ofproto_)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);

    hmap_remove(&all_bcmsdk_provider_nodes, &ofproto->all_bcmsdk_provider_node);

    hmap_destroy(&ofproto->bundles);

    sset_destroy(&ofproto->ports);
    sset_destroy(&ofproto->ghost_ports);
    sset_destroy(&ofproto->port_poll_set);

    if (ofproto->vrf) {
        release_vrf_id(ofproto->vrf_id);
    }

    ovs_mutex_destroy(&ofproto->stats_mutex);
    ovs_mutex_destroy(&ofproto->vsp_mutex);

    return;
}

static int
run(struct ofproto *ofproto_)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);

    ops_sflow_run(ofproto);

    return 0;
}

static void
wait(struct ofproto *ofproto_ OVS_UNUSED)
{
    return;
}

static void
query_tables(struct ofproto *ofproto,
             struct ofputil_table_features *features,
             struct ofputil_table_stats *stats)
{
    VLOG_DBG("query_tables %p %p %p", ofproto,features,stats);
    return;
}

static void
set_tables_version(struct ofproto *ofproto, cls_version_t version)
{
    return;
}

static struct ofport *
port_alloc(void)
{
    struct bcmsdk_provider_ofport_node *port = xmalloc(sizeof *port);
    return &port->up;
}

static void
port_dealloc(struct ofport *port_)
{
    struct bcmsdk_provider_ofport_node *port =
           bcmsdk_provider_ofport_node_cast(port_);
    free(port);
}

static int
port_construct(struct ofport *port_)
{
    struct bcmsdk_provider_ofport_node *port =
           bcmsdk_provider_ofport_node_cast(port_);
    VLOG_DBG("construct port %s", netdev_get_name(port->up.netdev));

    port->bundle = NULL;

    if (ofproto_is_ofdpa(port->up.ofproto)) {
        OFDPA_ERROR_t rc;
        int hw_unit, hw_port;
        ofdpaPortInfo_t port_info;

        netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, NULL);

        if (hw_port < 0) {
            VLOG_DBG("Skip create port in OF-DPA. Invalid hw_port value. (ofp_port = %d, hw_unit = %d, hw_port = %d)\n",
                     port->up.ofp_port, hw_unit, hw_port);
        } else {
            port_info.type = OFDPA_PORT_TYPE_PHYSICAL;
            port_info.portInfo.physical.hw_unit = hw_unit;
            port_info.portInfo.physical.hw_port = hw_port;

            rc = ofdpaPortCreate(port->up.ofp_port, &port_info);
            if (rc != OFDPA_E_NONE) {
                VLOG_ERR("Error creating port in OFDPA. (rc = %d, ofp_port = %d, hw_unit = %d, hw_port = %d)\n",
                         rc, port->up.ofp_port, hw_unit, hw_port);
            }
        }
    }

    return 0;
}

static void
port_destruct(struct ofport *port_)
{
    struct bcmsdk_provider_ofport_node *port =
           bcmsdk_provider_ofport_node_cast(port_);
    VLOG_DBG("destruct port %s", netdev_get_name(port->up.netdev));

    if (ofproto_is_ofdpa(port->up.ofproto)) {
        OFDPA_ERROR_t rc;

        rc = ofdpaPortDelete(port->up.ofp_port);
        if (rc != OFDPA_E_NONE) {
            VLOG_ERR("Error deleting port from OFDPA. (rc = %d, ofp_port = %d)\n",
                     rc, port->up.ofp_port);
        }
    }

    return;
}

static void
port_reconfigured(struct ofport *port_, enum ofputil_port_config old_config)
{
    VLOG_DBG("port_reconfigured %p %d", port_, old_config);
    return;
}

static bool
cfm_status_changed(struct ofport *ofport_)
{
    VLOG_DBG("cfm_status_changed %p", ofport_);
    return false;
}

static bool
bfd_status_changed(struct ofport *ofport_ OVS_UNUSED)
{
    return false;
}

/* Bundles. */

static void
add_trunked_vlans(unsigned long *vlan_list, opennsl_pbmp_t *pbm)
{
    int vid;

    if (vlan_list) {
        BITMAP_FOR_EACH_1(vid, VLAN_BITMAP_SIZE, vlan_list) {
            bcmsdk_add_trunk_ports(vid, pbm);
        }
    }
}

static void
del_trunked_vlans(unsigned long *vlan_list, opennsl_pbmp_t *pbm)
{
    int vid;

    if (vlan_list) {
        BITMAP_FOR_EACH_1(vid, VLAN_BITMAP_SIZE, vlan_list) {
            bcmsdk_del_trunk_ports(vid, pbm);
        }
    }
}

static void
config_all_vlans(enum port_vlan_mode vlan_mode, int vlan,
                 unsigned long *trunks, opennsl_pbmp_t *pbm)
{
    VLOG_DBG("%s: entry, vlan_mode=%d, tag=%d, trunks=%p, pbm=%p",
             __FUNCTION__, (int)vlan_mode, vlan, trunks, pbm);

    if ((pbm == NULL) || bcmsdk_pbmp_is_empty(pbm)) {
        return;
    }

    switch (vlan_mode) {
    case PORT_VLAN_ACCESS:
        if (vlan != -1) {
            bcmsdk_add_access_ports(vlan, pbm);
        }
        break;

    case PORT_VLAN_TRUNK:
        add_trunked_vlans(trunks, pbm);
        break;

    case PORT_VLAN_NATIVE_TAGGED:
        if (vlan != -1) {
            bcmsdk_add_native_tagged_ports(vlan, pbm);
        }

        add_trunked_vlans(trunks, pbm);
        break;

    case PORT_VLAN_NATIVE_UNTAGGED:
        if (vlan != -1) {
            bcmsdk_add_native_untagged_ports(vlan, pbm, false);
        }

        add_trunked_vlans(trunks, pbm);
        break;

    default:
        /* Should not happen. */
        VLOG_ERR("Invalid VLAN mode (%d).", vlan_mode);
        break;
    }

}

static void
unconfig_all_vlans(enum port_vlan_mode vlan_mode, int vlan,
                   unsigned long *trunks, opennsl_pbmp_t *pbm)
{
    VLOG_DBG("%s: entry, vlan_mode=%d, tag=%d, trunks=%p, pbm=%p",
             __FUNCTION__, (int)vlan_mode, vlan, trunks, pbm);

    if ((pbm == NULL) || bcmsdk_pbmp_is_empty(pbm)) {
        return;
    }

    switch (vlan_mode) {
    case PORT_VLAN_ACCESS:
        if (vlan != -1) {
            bcmsdk_del_access_ports(vlan, pbm);
        }
        break;

    case PORT_VLAN_TRUNK:
        del_trunked_vlans(trunks, pbm);
        break;

    case PORT_VLAN_NATIVE_TAGGED:
        if (vlan != -1) {
            bcmsdk_del_native_tagged_ports(vlan, pbm);
        }

        del_trunked_vlans(trunks, pbm);
        break;

    case PORT_VLAN_NATIVE_UNTAGGED:
        if (vlan != -1) {
            bcmsdk_del_native_untagged_ports(vlan, pbm, false);
        }

        del_trunked_vlans(trunks, pbm);
        break;

    default:
        /* Should not happen. */
        VLOG_ERR("Invalid VLAN mode (%d).", vlan_mode);
        break;
    }

}

static void
handle_trunk_vlan_changes(unsigned long *old_trunks, unsigned long *new_trunks,
                          opennsl_pbmp_t *pbm, enum port_vlan_mode old_mode,
                          enum port_vlan_mode new_mode)
{
    int vid;
    bool removed_vlans_found = false;
    bool added_vlans_found = false;
    unsigned long *removed_vlans = bitmap_allocate(VLAN_BITMAP_SIZE);
    unsigned long *added_vlans = bitmap_allocate(VLAN_BITMAP_SIZE);

    if (old_trunks) {
        BITMAP_FOR_EACH_1(vid, VLAN_BITMAP_SIZE, old_trunks) {
            if (!new_trunks || !bitmap_is_set(new_trunks, vid)) {
                VLOG_DBG("Found a deleted VLAN %d", vid);
                bitmap_set1(removed_vlans, vid);
                removed_vlans_found = true;
            }
        }
        /* Remove VLANs based on old mode. */
        if (removed_vlans_found) {
            unconfig_all_vlans(old_mode, -1, removed_vlans, pbm);
        }
    }

    if (new_trunks) {
        BITMAP_FOR_EACH_1(vid, VLAN_BITMAP_SIZE, new_trunks) {
            if (!old_trunks || !bitmap_is_set(old_trunks, vid)) {
                VLOG_DBG("Found an added VLAN %d", vid);
                bitmap_set1(added_vlans, vid);
                added_vlans_found = true;
            }
        }
        /* Configure new VLANs based on new mode. */
        if (added_vlans_found) {
            config_all_vlans(new_mode, -1, added_vlans, pbm);
        }
    }

    /* Done with these bitmaps. */
    bitmap_free(removed_vlans);
    bitmap_free(added_vlans);
}

/*
 * used externally, do NOT make static
 */
struct ofbundle *
bundle_lookup(const struct bcmsdk_provider_node *ofproto, void *aux)
{
    struct ofbundle *bundle;

    HMAP_FOR_EACH_IN_BUCKET (bundle, hmap_node, hash_pointer(aux, 0),
                             &ofproto->bundles) {
        if (bundle->aux == aux) {
            return bundle;
        }
    }
    return NULL;
}

static void
bundle_del_port(struct bcmsdk_provider_ofport_node *port)
{
    list_remove(&port->bundle_node);
    port->bundle = NULL;
}

static bool
bundle_add_port(struct ofbundle *bundle, ofp_port_t ofp_port,
                struct lacp_slave_settings *lacp OVS_UNUSED)
{
    struct bcmsdk_provider_ofport_node *port;

    port = get_ofp_port(bundle->ofproto, ofp_port);
    if (!port) {
        return false;
    }

    if (port->bundle != bundle) {
        if (port->bundle) {
            bundle_remove(&port->up);
        }
        port->bundle = bundle;
        list_push_back(&bundle->ports, &port->bundle_node);
    }

    return true;
}

static void
bundle_destroy(struct ofbundle *bundle)
{
    struct bcmsdk_provider_node *ofproto;
    struct bcmsdk_provider_ofport_node *port = NULL, *next_port;
    const char *type;
    int hw_unit, hw_port;
    uint8_t mac[ETH_ADDR_LEN];

    if (!bundle) {
        return;
    }
    ofproto = bundle->ofproto;
    VLOG_DBG("%s, destroying bundle = %s", __FUNCTION__, bundle->name);

    /* Unconfigure all IP's for this port */
    port_unconfigure_ips(bundle);

    LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {

        type = netdev_get_type(port->up.netdev);

        if (strcmp(type, OVSREC_INTERFACE_TYPE_SYSTEM) == 0) {

            if (bundle->l3_intf) {
                /* Clear up LAG member before deleting the bundle completely
                 */
                if (list_size(&bundle->ports) > 1) {

                    opennsl_pbmp_t lag_pbmp;

                    OPENNSL_PBMP_CLEAR(lag_pbmp);
                    netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, mac);
                    VLOG_DBG("%s port %s removed",
                              __FUNCTION__,netdev_get_name(port->up.netdev));
                    OPENNSL_PBMP_PORT_ADD(lag_pbmp, hw_port);

                    /* Delete bitmap */
                    bcmsdk_del_native_untagged_ports(bundle->l3_intf->l3a_vid,
                            &lag_pbmp,
                            true);

                    /* Delete knet */
                    handle_bcmsdk_knet_l3_port_filters(port->up.netdev,
                            bundle->l3_intf->l3a_vid,
                            false);
                } else {
                    port_unconfigure_ips(bundle);
                    netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, mac);
                    VLOG_DBG("%s destroy the l3 interface hw_port = %d",
                             __FUNCTION__, hw_port);
                    ops_routing_disable_l3_interface(hw_unit,
                            hw_port,
                            bundle->l3_intf,
                            port->up.netdev);
                    /* Destroy L3 stats */
                    netdev_bcmsdk_l3intf_fp_stats_destroy(bundle->hw_port, bundle->hw_unit);
                    bundle->l3_intf = NULL;
                }
            }

            /* Unconfigure any existing VLAN in h/w. */
            unconfig_all_vlans(bundle->vlan_mode, bundle->vlan,
                    bundle->trunks, bundle->pbm);
        } else if (strcmp(type, OVSREC_INTERFACE_TYPE_VLANSUBINT) == 0) {
            VLOG_DBG("%s destroy the subinterface %s", __FUNCTION__, bundle->name);
            if (bundle->l3_intf) {
                port_unconfigure_ips(bundle);
                ops_routing_disable_l3_subinterface(bundle->hw_unit,
                        bundle->hw_port,
                        bundle->l3_intf,
                        port->up.netdev);
                bundle->l3_intf = NULL;
            }

        } else if (strcmp(type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0) {
            VLOG_DBG("%s destroy the internal interface",__FUNCTION__);
            if (bundle->l3_intf) {
                port_unconfigure_ips(bundle);
                opennsl_l3_intf_delete(bundle->hw_unit, bundle->l3_intf);
                bundle->l3_intf = NULL;
            }
        }
        netdev_bcmsdk_l3_ingress_stats_destroy(port->up.netdev);
        netdev_bcmsdk_l3_egress_stats_destroy(port->up.netdev);
        bundle_del_port(port);
    }

    VLOG_DBG("%s: Deallocate bond_hw_handle# %d for port %s",
             __FUNCTION__, bundle->bond_hw_handle, bundle->name);

    if (bundle->bond_hw_handle != -1) {
        VLOG_DBG("%s destroy the lag %s", __FUNCTION__, bundle->name);
        bcmsdk_destroy_lag(bundle->bond_hw_handle);
        ops_sflow_remove_polling_on_lag_interface(bundle);
        bundle->lag_sflow_polling_interval = 0;
    }

    /* in case a mirror destination, unconfigure it */
    mirror_object_destroy_with_mtp(bundle);

    ofproto = bundle->ofproto;

    hmap_remove(&ofproto->bundles, &bundle->hmap_node);
    bitmap_free(bundle->trunks);
    free(bundle->name);
    free(bundle);
}

static void
port_list_to_hw_pbm(struct ofproto *ofproto_, opennsl_pbmp_t *pbm,
                    ofp_port_t *port_list, size_t n_ports)
{
    const struct ofport *ofport;
    int i, ofp_port, hw_unit, hw_id;

    for (i = 0; i < n_ports; i++) {
        ofport = ofproto_get_port(ofproto_, port_list[i]);
        ofp_port = ofport ? ofport->ofp_port : OFPP_NONE;
        if (ofp_port == OFPP_NONE) {
            VLOG_WARN("Null ofport for port list member# %d", i);
            continue;
        }
        netdev_bcmsdk_get_hw_info(ofport->netdev, &hw_unit, &hw_id, NULL);
        ovs_assert(hw_unit <= MAX_SWITCH_UNIT_ID);
        bcmsdk_pbmp_add_hw_port(pbm, hw_unit, hw_id);

        VLOG_DBG("member# %d port %s internal port# %d, hw_unit# %d, hw_id# %d",
                 i, netdev_get_name(ofport->netdev), ofp_port, hw_unit, hw_id);
    }
}

/* Host Functions */
/* Function to configure local host ip */
static int
port_l3_host_add(struct ofproto *ofproto_, bool is_ipv6, char *ip_addr)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofproto_l3_host host_info;

    VLOG_DBG("ofproto_host_add called for ip %s", ip_addr);

    /* Update the host info for ofproto action */
    host_info.family = is_ipv6 ? OFPROTO_ROUTE_IPV6 : OFPROTO_ROUTE_IPV4;
    host_info.ip_address = ip_addr;

    /* Call Provider */
    if (!ops_routing_host_entry_action(0, ofproto->vrf_id, OFPROTO_HOST_ADD,
                                       &host_info) ) {
        VLOG_DBG("Added host entry for %s", ip_addr);
        return 0;
    } else {
        VLOG_ERR("!ops_routing_host_entry_action for add failed");
        return 1;
    }
} /* port_l3_host_add */

/* Function to unconfigure local host ip */
static int
port_l3_host_delete(struct ofproto *ofproto_, bool is_ipv6, char *ip_addr)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofproto_l3_host host_info;

    VLOG_DBG("ofproto_host_delete called for ip %s", ip_addr);

    /* Update the host info for ofproto action */
    host_info.family = is_ipv6 ? OFPROTO_ROUTE_IPV6 : OFPROTO_ROUTE_IPV4;
    host_info.ip_address = ip_addr;

    /* Call Provider */
    if (!ops_routing_host_entry_action(0, ofproto->vrf_id, OFPROTO_HOST_DELETE,
                                       &host_info) ) {
        VLOG_DBG("Deleted host entry for %s", ip_addr);
        return 0;
    } else {
        VLOG_ERR("!ops_routing_host_entry_action for delete failed");
        return 1;
    }
} /* port_l3_host_delete */

/* Function to unconfigure and free all port ip's */
static void
port_unconfigure_ips(struct ofbundle *bundle)
{
    struct ofproto *ofproto;
    bool is_ipv6 = false;
    struct net_address *addr, *next;

    ofproto = &bundle->ofproto->up;

    /* Unconfigure primary ipv4 address and free */
    if (bundle->ip4_address) {
        port_l3_host_delete(ofproto, is_ipv6, bundle->ip4_address);
        free(bundle->ip4_address);
    }

    /* Unconfigure primary ipv6 address and free */
    if (bundle->ip6_address) {
        port_l3_host_delete(ofproto, is_ipv6, bundle->ip6_address);
        free(bundle->ip6_address);
    }

    /* Unconfigure secondary ipv4 address and free the hash */
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &bundle->secondary_ip4addr) {
        port_l3_host_delete(ofproto, is_ipv6, addr->address);
        hmap_remove(&bundle->secondary_ip4addr, &addr->addr_node);
        free(addr->address);
        free(addr);
    }
    hmap_destroy( &bundle->secondary_ip4addr);

    /* Unconfigure secondary ipv6 address and free the hash */
    is_ipv6 = true;
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &bundle->secondary_ip6addr) {
        port_l3_host_delete(ofproto, is_ipv6, addr->address);
        hmap_remove(&bundle->secondary_ip6addr, &addr->addr_node);
        free(addr->address);
        free(addr);
    }
    hmap_destroy( &bundle->secondary_ip6addr);

} /* port_unconfigure_ips */

/*
** Function to find if the ipv4 secondary address already exist in the hash.
*/
static struct net_address *
port_ip4_addr_find(struct ofbundle *bundle, const char *address)
{
    struct net_address *addr;

    HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                             &bundle->secondary_ip4addr) {
        if (!strcmp(addr->address, address)) {
            return addr;
        }
    }

    return NULL;
} /* port_ip4_addr_find */

/*
** Function to find if the ipv6 secondary address already exist in the hash.
*/
static struct net_address *
port_ip6_addr_find(struct ofbundle *bundle, const char *address)
{
    struct net_address *addr;

    HMAP_FOR_EACH_WITH_HASH (addr, addr_node, hash_string(address, 0),
                             &bundle->secondary_ip6addr) {
        if (!strcmp(addr->address, address)) {
            return addr;
        }
    }

    return NULL;
} /* port_ip6_addr_find */

/*
** Function to check for changes in secondary ipv4 configuration of a
** given port
*/
static void
port_config_secondary_ipv4_addr(struct ofproto *ofproto,
                                struct ofbundle *bundle,
                                const struct ofproto_bundle_settings *s)
{
    struct shash new_ip_list;
    struct net_address *addr, *next;
    struct shash_node *addr_node;
    int i;
    bool is_ipv6 = false;

    shash_init(&new_ip_list);

    /* Create hash of the current secondary ip's */
    for (i = 0; i < s->n_ip4_address_secondary; i++) {
       if(!shash_add_once(&new_ip_list, s->ip4_address_secondary[i],
                           s->ip4_address_secondary[i])) {
            VLOG_WARN("Duplicate address in secondary list %s\n",
                      s->ip4_address_secondary[i]);
        }
    }

    /* Compare current and old to delete any obselete one's */
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &bundle->secondary_ip4addr) {
        if (!shash_find_data(&new_ip_list, addr->address)) {
            hmap_remove(&bundle->secondary_ip4addr, &addr->addr_node);
            port_l3_host_delete(ofproto, is_ipv6, addr->address);
            free(addr->address);
            free(addr);
        }
    }

    /* Add the newly added addresses to the list */
    SHASH_FOR_EACH (addr_node, &new_ip_list) {
        struct net_address *addr;
        const char *address = addr_node->data;
        if (!port_ip4_addr_find(bundle, address)) {
            /*
             * Add the new address to the list
             */
            addr = xzalloc(sizeof *addr);
            addr->address = xstrdup(address);
            hmap_insert(&bundle->secondary_ip4addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            port_l3_host_add(ofproto, is_ipv6, addr->address);
        }
    }
} /* port_config_secondary_ipv4_addr */

/*
** Function to check for changes in secondary ipv6 configuration of a
** given port
*/
static void
port_config_secondary_ipv6_addr(struct ofproto *ofproto,
                                struct ofbundle *bundle,
                                const struct ofproto_bundle_settings *s)
{
    struct shash new_ip6_list;
    struct net_address *addr, *next;
    struct shash_node *addr_node;
    int i;
    bool is_ipv6 = true;

    shash_init(&new_ip6_list);

    /* Create hash of the current secondary ip's */
    for (i = 0; i < s->n_ip6_address_secondary; i++) {
        if(!shash_add_once(&new_ip6_list, s->ip6_address_secondary[i],
                           s->ip6_address_secondary[i])) {
            VLOG_WARN("Duplicate address in secondary list %s\n",
                      s->ip6_address_secondary[i]);
        }
    }

    /* Compare current and old to delete any obselete one's */
    HMAP_FOR_EACH_SAFE (addr, next, addr_node, &bundle->secondary_ip6addr) {
        if (!shash_find_data(&new_ip6_list, addr->address)) {
            hmap_remove(&bundle->secondary_ip6addr, &addr->addr_node);
            port_l3_host_delete(ofproto, is_ipv6, addr->address);
            free(addr->address);
            free(addr);
        }
    }

    /* Add the newly added addresses to the list */
    SHASH_FOR_EACH (addr_node, &new_ip6_list) {
        struct net_address *addr;
        const char *address = addr_node->data;
        if (!port_ip6_addr_find(bundle, address)) {
            /*
             * Add the new address to the list
             */
            addr = xzalloc(sizeof *addr);
            addr->address = xstrdup(address);
            hmap_insert(&bundle->secondary_ip6addr, &addr->addr_node,
                        hash_string(addr->address, 0));
            port_l3_host_add(ofproto, is_ipv6, addr->address);
        }
    }
}

/* Function to check for changes in ip configuration of a given port */
static int
port_ip_reconfigure(struct ofproto *ofproto, struct ofbundle *bundle,
                    const struct ofproto_bundle_settings *s)
{
    bool is_ipv6 = false;

    VLOG_DBG("In port_ip_reconfigure with ip_change val=0x%x", s->ip_change);
    /* If primary ipv4 got added/deleted/modified */
    if (s->ip_change & PORT_PRIMARY_IPv4_CHANGED) {
        if (s->ip4_address) {
            if (bundle->ip4_address) {
                if (strcmp(bundle->ip4_address, s->ip4_address) != 0) {
                    /* If current and earlier are different, delete old */
                    port_l3_host_delete(ofproto, is_ipv6,
                                        bundle->ip4_address);
                    free(bundle->ip4_address);

                    /* Add new */
                    bundle->ip4_address = xstrdup(s->ip4_address);
                    port_l3_host_add(ofproto, is_ipv6,
                                     bundle->ip4_address);
                }
                /* else no change */
            } else {
                /* Earlier primary was not there, just add new */
                bundle->ip4_address = xstrdup(s->ip4_address);
                port_l3_host_add(ofproto, is_ipv6, bundle->ip4_address);
            }
        } else {
            /* Primary got removed, earlier if it was there then remove it */
            if (bundle->ip4_address != NULL) {
                port_l3_host_delete(ofproto, is_ipv6, bundle->ip4_address);
                free(bundle->ip4_address);
                bundle->ip4_address = NULL;
            }
        }
    }

    /* If primary ipv6 got added/deleted/modified */
    if (s->ip_change & PORT_PRIMARY_IPv6_CHANGED) {
        is_ipv6 = true;
        if (s->ip6_address) {
            if (bundle->ip6_address) {
                if (strcmp(bundle->ip6_address, s->ip6_address) !=0) {
                    /* If current and earlier are different, delete old */
                    port_l3_host_delete(ofproto, is_ipv6, bundle->ip6_address);
                    free(bundle->ip6_address);

                    /* Add new */
                    bundle->ip6_address = xstrdup(s->ip6_address);
                    port_l3_host_add(ofproto, is_ipv6, bundle->ip6_address);

                }
                /* else no change */
            } else {
                /* Earlier primary was not there, just add new */
                bundle->ip6_address = xstrdup(s->ip6_address);
                port_l3_host_add(ofproto, is_ipv6, bundle->ip6_address);
            }
        } else {
            /* Primary got removed, earlier if it was there then remove it */
            if (bundle->ip6_address != NULL) {
                port_l3_host_delete(ofproto, is_ipv6, bundle->ip6_address);
                free(bundle->ip6_address);
                bundle->ip6_address = NULL;
            }
        }
    }

    /* If any secondary ipv4 addr added/deleted/modified */
    if (s->ip_change & PORT_SECONDARY_IPv4_CHANGED) {
        VLOG_DBG("ip4_address_secondary modified");
        port_config_secondary_ipv4_addr(ofproto, bundle, s);
    }

    if (s->ip_change & PORT_SECONDARY_IPv6_CHANGED) {
        VLOG_DBG("ip6_address_secondary modified");
        port_config_secondary_ipv6_addr(ofproto, bundle, s);
    }

    return 0;
}

static int
bundle_set(struct ofproto *ofproto_, void *aux,
           const struct ofproto_bundle_settings *s)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    int i;
    const char *opt_arg;
    opennsl_pbmp_t *all_pbm;
    opennsl_pbmp_t *temp_pbm;
    unsigned long *new_trunks = NULL;
    bool trunk_all_vlans = false;
    struct bcmsdk_provider_ofport_node *port;
    struct ofbundle *bundle;
    const char *type = NULL;
    struct bcmsdk_provider_ofport_node *next_port;
    bool ok;

    VLOG_DBG("%s: entry, ofproto_=%p, aux=%p, s=%p",
             __FUNCTION__, ofproto_, aux, s);

    bundle = bundle_lookup(ofproto, aux);

    if (s == NULL) {
        if (bundle != NULL) {
            VLOG_DBG("%s deleting bundle %s", __FUNCTION__,bundle->name);
            bundle_destroy(bundle);
        }
        return 0;
    }

    if (!bundle) {
        VLOG_DBG("%s creating NEW bundle %s", __FUNCTION__, s->name);

        bundle = xmalloc(sizeof *bundle);

        bundle->enable = true;
        bundle->ofproto = ofproto;
        hmap_insert(&ofproto->bundles, &bundle->hmap_node,
                    hash_pointer(aux, 0));
        bundle->aux = aux;
        bundle->name = NULL;
        bundle->l3_intf = NULL;
        bundle->hw_unit = 0;
        bundle->hw_port = -1;
        list_init(&bundle->ports);
        bundle->vlan_mode = PORT_VLAN_ACCESS;
        bundle->vlan = -1;
        bundle->trunks = NULL;
        bundle->trunk_all_vlans = false;
        bundle->pbm = NULL;
        bundle->bond_hw_handle = -1;
        bundle->mirror_data = NULL;
        bundle->lacp = NULL;
        bundle->bond = NULL;
        bundle->lag_sflow_polling_interval = 0;

        bundle->ip4_address = NULL;
        bundle->ip6_address = NULL;
        hmap_init(&bundle->secondary_ip4addr);
        hmap_init(&bundle->secondary_ip6addr);
    }

    if (!bundle->name || strcmp(s->name, bundle->name)) {
        free(bundle->name);
        bundle->name = xstrdup(s->name);
    }

    VLOG_DBG("%s: bundle->name=%s, bundle->bond_hw_handle=%d, "
             "n_slaves=%d, s->bond=%p, "
             "s->hw_bond_should_exist=%d, "
             "s->bond_handle_alloc_only=%d, "
             "bundle->hw_port=%d",
             __FUNCTION__, bundle->name, bundle->bond_hw_handle,
             (int) s->n_slaves, s->bond,
             s->hw_bond_should_exist,
             s->bond_handle_alloc_only,
             bundle->hw_port);

    /* Allocate Broadcom hw port bitmap. */
    all_pbm = bcmsdk_alloc_pbmp();
    if (all_pbm == NULL) {
        return ENOMEM;
    }

    if ((-1 == bundle->bond_hw_handle) &&
        (s->hw_bond_should_exist || (s->bond_handle_alloc_only))) {
        /* Create a h/w LAG if there is more than one member present
           in the bundle or if requested by upper layer. */
        bcmsdk_create_lag(&bundle->bond_hw_handle, s->name);
        log_event("LAG_CREATE",
                   EV_KV("lag_id", "%s", bundle->name));
        VLOG_DBG("%s: Allocated bond_hw_handle# %d for port %s",
                 __FUNCTION__, bundle->bond_hw_handle, s->name);
        if (s->bond_handle_alloc_only) {
            bcmsdk_destroy_pbmp(all_pbm);
            return 0;
        }
    } else if ((-1 != bundle->bond_hw_handle) &&
               (false == s->hw_bond_should_exist)) {
        /* LAG should not exist in h/w any more. */
        VLOG_DBG("%s destroy LAG", __FUNCTION__);
        bcmsdk_destroy_lag(bundle->bond_hw_handle);
        bundle->bond_hw_handle = -1;
        ops_sflow_remove_polling_on_lag_interface(bundle);
        bundle->lag_sflow_polling_interval = 0;
        log_event("LAG_DELETE",
                   EV_KV("lag_id", "%s", bundle->name));
    }

    if (ofproto->vrf &&
        (ofproto->vrf_id != BCM_MAX_VRFS)
        && s->n_slaves > 0) {
        struct bcmsdk_provider_ofport_node *port;
        int hw_unit, hw_port;
        opennsl_vlan_t vlan_id;
        uint8_t mac[ETH_ADDR_LEN];

        port = get_ofp_port(bundle->ofproto, s->slaves[0]);
        if (!port) {
            VLOG_ERR("slave is not in the ports\n");
            return ENODEV;
        }

        type = netdev_get_type(port->up.netdev);
        VLOG_DBG("%s type = %s", __FUNCTION__,type);
        netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, mac);

        /* For internal vlan interfaces, we get vlanid from tag column
         * For regular l3 interfaces we will get from internal vlan id from
         * hw_config column
         */
        if (strcmp(type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0) {
            vlan_id = s->vlan;
        } else if (strcmp(type, OVSREC_INTERFACE_TYPE_VLANSUBINT) == 0) {
            VLOG_DBG("%s get subinterface vlan", __FUNCTION__);
            netdev_bcmsdk_get_subintf_vlan(port->up.netdev, &vlan_id);
            VLOG_DBG("%s subinterface vlan = %d\n", __FUNCTION__,vlan_id);
        } else if ((strcmp(type, OVSREC_INTERFACE_TYPE_LOOPBACK) == 0)) {
            /* For l3-loopback interfaces, just configure ips */
            port_ip_reconfigure(ofproto_, bundle, s);
            VLOG_DBG("%s Done with l3 loopback configurations", __FUNCTION__);
            log_event("LOOPBACK_CREATE", EV_KV("interface", "%s", bundle->name));
            goto done;
        } else {
            vlan_id = smap_get_int(s->port_options[PORT_HW_CONFIG],
                                   "internal_vlan_id", 0);
        }
        VLOG_DBG("%s s-enable = %d vlan_id(%d)\n",__FUNCTION__,s->enable, vlan_id);

        if (bundle->l3_intf) {
            VLOG_DBG("%s bundle %s exists", __FUNCTION__, bundle->name);
            /* if reserved vlan changed/removed or if port status is disabled */
            if ((bundle->l3_intf->l3a_vid != vlan_id || !s->enable) &&
                 false == s->hw_bond_should_exist) {
                VLOG_DBG("%s call disable s-enable = %d or vid(%d) != vlan_id(%d)",
                           __FUNCTION__, s->enable,
                           bundle->l3_intf->l3a_vid, vlan_id);
                if (strcmp(type, OVSREC_INTERFACE_TYPE_VLANSUBINT) == 0) {
                    VLOG_DBG("%s disable l3 subinterface", __FUNCTION__);
                     if (bundle->l3_intf->l3a_vid != vlan_id) {
                         log_event("SUBINTERFACE_ENC_UPDATE",
                                   EV_KV("interface", "%s", bundle->name),
                                   EV_KV("value", "%d", vlan_id));
                     }
                     if (!s->enable) {
                         log_event("SUBINTERFACE_ADMIN_STATE",
                                    EV_KV("interface", "%s", bundle->name),
                                    EV_KV("state", "%s", s->enable?"up":"down"));
                     }
                    ops_routing_disable_l3_subinterface(hw_unit, hw_port,
                                                        bundle->l3_intf,
                                                        port->up.netdev);
                    log_event("SUBINTERFACE_DELETE",
                               EV_KV("interface", "%s", bundle->name));
                } else if (strcmp(type, OVSREC_INTERFACE_TYPE_SYSTEM) == 0) {
                    VLOG_DBG("%s disable l3 interface %s",
                              __FUNCTION__, bundle->name);
                         log_event("L3INTERFACE_ADMIN_STATE",
                                    EV_KV("interface", "%s", bundle->name),
                                    EV_KV("state", "%s", s->enable?"up":"down"));
                    ops_routing_disable_l3_interface(hw_unit, hw_port,
                                                     bundle->l3_intf,
                                                     port->up.netdev);
                    /* Destroy L3 stats */
                    netdev_bcmsdk_l3intf_fp_stats_destroy(hw_port, hw_unit);

                    log_event("L3INTERFACE_DELETE",
                               EV_KV("interface", "%s", bundle->name));
                } else if (strcmp(type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0) {
                    opennsl_l3_intf_delete(hw_unit, bundle->l3_intf);
                }
                netdev_bcmsdk_l3_ingress_stats_remove(port->up.netdev);
                bundle->l3_intf = NULL;
                bundle->hw_unit = 0;
                bundle->hw_port = -1;
            }
        }

        if (vlan_id && !bundle->l3_intf && s->enable) {

            /* If interface type is not internal create l3 interface, else
             * create an l3 vlan interface on every hw_unit. */
            if (strcmp(type, OVSREC_INTERFACE_TYPE_SYSTEM) == 0) {
                VLOG_DBG("%s Create interface %s", __FUNCTION__, bundle->name);
                log_event("L3INTERFACE_ADMIN_STATE",
                           EV_KV("interface", "%s", bundle->name),
                           EV_KV("state", "%s", s->enable?"up":"down"));
                bundle->l3_intf = ops_routing_enable_l3_interface(
                            hw_unit, hw_port, ofproto->vrf_id, vlan_id,
                            mac, port->up.netdev);

                if (bundle->l3_intf) {
                    bundle->hw_unit = hw_unit;
                    bundle->hw_port = hw_port;
                    /* Create FP group/entries for l3 interface stats */
                    netdev_bcmsdk_l3intf_fp_stats_create(port->up.netdev, vlan_id);
                    log_event("L3INTERFACE_CREATE",
                               EV_KV("interface", "%s", bundle->name));
                }

            } else if (strcmp(type, OVSREC_INTERFACE_TYPE_VLANSUBINT) == 0) {
                VLOG_DBG("%s enable subinterface l3", __FUNCTION__);
                int unit = 0;
                bundle->l3_intf = ops_routing_enable_l3_subinterface(
                        unit, hw_port, ofproto->vrf_id, vlan_id,
                        mac, port->up.netdev);
                if (bundle->l3_intf) {
                    bundle->hw_unit = hw_unit;
                    bundle->hw_port = hw_port;
                    log_event("SUBINTERFACE_CREATE",
                               EV_KV("interface", "%s", bundle->name));
                }

            } else if (strcmp(type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0) {
                int unit = 0;
                for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {
                    bundle->l3_intf = ops_routing_enable_l3_vlan_interface(
                            unit, ofproto->vrf_id, vlan_id,
                            mac);
                    if (bundle->l3_intf) {
                        log_event("VLANINTERFACE_CREATE",
                                EV_KV("interface", "%s", bundle->name));
                    }
                }
            }
            /* Initialize L3 ingress flex counters. The L3 egress flex counters are
             * installed dynamically per egress object at creation time. */
            netdev_bcmsdk_create_l3_ingress_stats(port->up.netdev, vlan_id);
        }
    }

    /* Check for ip changes. IP config can come in any order, no l3_int check */
    port_ip_reconfigure(ofproto_, bundle, s);

    /* Look for port configuration options
     * FIXME: - fill up stubs with actual actions */
    opt_arg = smap_get(s->port_options[PORT_OPT_VLAN], "vlan_options_p0");
    if (opt_arg != NULL) {
       VLOG_DBG("%s VLAN config options option_arg= %s", __FUNCTION__,opt_arg);
    }

    opt_arg = smap_get(s->port_options[PORT_OPT_BOND], "bond_options_p0");
    if (opt_arg != NULL) {
       VLOG_DBG("%s BOND config options option_arg= %s", __FUNCTION__,opt_arg);
    }

    /* sFlow config on port. true - enable sFlow; false - disable sFlow */
    bool sflow_enabled = smap_get_bool(s->port_options[PORT_OTHER_CONFIG],
                             PORT_OTHER_CONFIG_SFLOW_PER_INTERFACE_KEY_STR,
                             true);
    if (s->name &&
        strncmp(s->name, DEFAULT_BRIDGE_NAME, strlen(DEFAULT_BRIDGE_NAME)) == 0) {
        VLOG_DBG("sFlow will not be configured on bridge_normal");
    }
    /* Check if sFlow configuration on port has changed and
       download it to ASIC. */
    else if (ops_sflow_port_config_changed(bundle->name, sflow_enabled)) {
        int hw_unit, hw_port;
        /* Loop through all the 'ports' under a 'bundle' and
           enable/disable sFlow sampling on all of them. */
        LIST_FOR_EACH_SAFE(port, next_port, bundle_node, &bundle->ports) {
            type = netdev_get_type(port->up.netdev);
            /* sFlow is only supported on physical(system) interfaces */
            if (strcmp(type, OVSREC_INTERFACE_TYPE_SYSTEM) == 0) {
                VLOG_DBG("sflow on port %s is %s", bundle->name,
                         (sflow_enabled)?"ENABLED":"DISABLED");
                netdev_bcmsdk_get_hw_info(port->up.netdev,
                                          &hw_unit, &hw_port, NULL);
                ops_sflow_set_per_interface(hw_unit, hw_port, sflow_enabled);
                sflow_options_update_ports_list(bundle->name, sflow_enabled);
            } else {
                VLOG_DBG("sFlow is not applicable for port %s of type %s",
                         bundle->name, type);
                break;
            }
        }
    }
    else {
        VLOG_DBG("No sFlow configuration change on port %s : %s", bundle->name,
                 (sflow_enabled)?"ENABLED":"DISABLED");
    }

    /* Go through the list of physical interfaces (slaves) that
     * belong to this logical port, and construct a corresponding
     * hw port bitmap so we can configure the VLAN(s) all at once. */
    port_list_to_hw_pbm(ofproto_, all_pbm, s->slaves, s->n_slaves);

    /* Apply LAG configuration if the bundle is a LAG. */
    if (bundle->bond_hw_handle != -1) {
        opennsl_pbmp_t *tx_en_pbm = NULL;
        int lag_mode = 0;

        if (s->bond) {
            /* update LAG balance mode. */
            switch (s->bond->balance) {
            case BM_L2_SRC_DST_HASH:
                lag_mode = OPENNSL_TRUNK_PSC_SRCDSTMAC;
                log_event("LAG_LOAD_BAL_MODE",
                           EV_KV("interface", "%s", bundle->name),
                           EV_KV("mode", "%s", "SRCDSTMAC"));
                break;
            case BM_L3_SRC_DST_HASH:
                lag_mode = OPENNSL_TRUNK_PSC_SRCDSTIP;
                log_event("LAG_LOAD_BAL_MODE",
                           EV_KV("interface", "%s", bundle->name),
                           EV_KV("mode", "%s", "SRCDSTIP"));
                break;
            case BM_L4_SRC_DST_HASH:
                bcmsdk_trunk_hash_setup(OPS_L4_SRC_DST);
                lag_mode = OPENNSL_TRUNK_PSC_PORTFLOW;
                log_event("LAG_LOAD_BAL_MODE",
                           EV_KV("interface", "%s", bundle->name),
                           EV_KV("mode", "%s", "PORTFLOW"));
                break;
            default:
                break;
            }
            if (lag_mode != 0) {
                bcmsdk_set_lag_balance_mode(bundle->bond_hw_handle, lag_mode);
            }
        }

        /* Attach ports to the LAG. */
        bcmsdk_attach_ports_to_lag(bundle->bond_hw_handle, all_pbm);

        /* Allocate another port bitmap for LAG's tx_enabled members. */
        tx_en_pbm = bcmsdk_alloc_pbmp();
        if (NULL == tx_en_pbm) {
            bcmsdk_destroy_pbmp(all_pbm);
            return ENOMEM;
        }

        port_list_to_hw_pbm(ofproto_, tx_en_pbm, s->slaves_tx_enable,
                            s->n_slaves_tx_enable);

        bcmsdk_egress_enable_lag_ports(bundle->bond_hw_handle,
                                       tx_en_pbm);
        bcmsdk_destroy_pbmp(tx_en_pbm);

        /* Update the l3 interface with new LAG membership list */
        if (ofproto->vrf &&
           (ofproto->vrf_id != BCM_MAX_VRFS) &&
           (bundle->l3_intf && s->hw_bond_should_exist)) {
            int hw_unit, hw_port;
            uint8_t mac[ETH_ADDR_LEN];
            opennsl_pbmp_t lag_pbmp;
            OPENNSL_PBMP_CLEAR(lag_pbmp);
            opennsl_vlan_t vlan_id = bundle->l3_intf->l3a_vid;

            /* Add/delete vlan bit map from bundle->l3_intf->l3a_vid
               Also add/delete knet */
            /* if new port is added the bundle previous count is less */
            for (i = 0; i < s->n_slaves; i++) {
                LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {
                    if (s->slaves[i] == port->up.ofp_port) {
                        goto found2;
                    }
                }

                OPENNSL_PBMP_CLEAR(lag_pbmp);
                port = get_ofp_port(bundle->ofproto, s->slaves[i]);
                if (!port) {
                    VLOG_ERR("slave is not in the ports\n");
                    return ENODEV;
                }
                VLOG_DBG("%s Adding port %s from VLAN",
                         __FUNCTION__,netdev_get_name(port->up.netdev));
                log_event("L3_LAG_ADD_VLAN_PORT_BITMAP",
                           EV_KV("interface", "%s", bundle->name),
                           EV_KV("port", "%s", netdev_get_name(port->up.netdev)),
                           EV_KV("vlan", "%d", vlan_id));
                netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, mac);
                OPENNSL_PBMP_PORT_ADD(lag_pbmp, hw_port);
                /* add bitmap */
                bcmsdk_add_native_untagged_ports(vlan_id,
                        &lag_pbmp,
                        true);
                /* Add knet */
                handle_bcmsdk_knet_l3_port_filters(port->up.netdev,
                        vlan_id,
                        true);
                found2: ;
            }
            /* Remove lag member */
            LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {
                for (i = 0; i < s->n_slaves; i++) {
                    if (s->slaves[i] == port->up.ofp_port) {
                        goto found1;
                    }
                }

                OPENNSL_PBMP_CLEAR(lag_pbmp);
                netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port, mac);
                OPENNSL_PBMP_PORT_ADD(lag_pbmp, hw_port);
                if (s->n_slaves == 0 && bundle->l3_intf) {
                    VLOG_DBG("%s destroy l3 interface %s", __FUNCTION__, bundle->name);
                    ops_routing_disable_l3_interface(hw_unit, hw_port,
                            bundle->l3_intf,
                            port->up.netdev);
                    log_event("L3_LAG_DELETE",
                               EV_KV("interface", "%s", bundle->name));
                    bundle->l3_intf = NULL;
                } else {

                    VLOG_DBG("%s Removing port %s from VLAN",
                             __FUNCTION__,netdev_get_name(port->up.netdev));
                    if (bundle->l3_intf) {
                        /* Delete bitmap */
                        bcmsdk_del_native_untagged_ports(vlan_id, &lag_pbmp,
                                true);
                        log_event("L3_LAG_REM_VLAN_PORT_BITMAP",
                                EV_KV("interface", "%s", bundle->name),
                                EV_KV("port", "%s", netdev_get_name(port->up.netdev)),
                                EV_KV("vlan", "%d", vlan_id));
                    }

                    /* Delete knet */
                    handle_bcmsdk_knet_l3_port_filters(port->up.netdev,
                            vlan_id,
                            false);

                    found1: ;
                }
            }
        }
    }

    /* Update set of ports only for L2 and L3 interfaces and L2 LAG,
       but in case of L3 LAG, we need to update ports to bundle only
       if bundle->l3_intf is created. This is required because L3 LAG
       needs to update the VLAN bitmap before updating bundle->ports */
    if ((!ofproto->vrf) || (ofproto->vrf && !s->hw_bond_should_exist) ||
        (ofproto->vrf && s->hw_bond_should_exist && bundle->l3_intf)) {
        ok = true;
        for (i = 0; i < s->n_slaves; i++) {
            if (!bundle_add_port(bundle, s->slaves[i], NULL)) {
                ok = false;
            }
        }

        if (!ok || list_size(&bundle->ports) != s->n_slaves) {
            struct bcmsdk_provider_ofport_node *next_port;

            LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {
                for (i = 0; i < s->n_slaves; i++) {
                    if (s->slaves[i] == port->up.ofp_port) {
                        goto found;
                    }
                }

                VLOG_DBG("%s Bundle delete port %s",
                        __FUNCTION__,netdev_get_name(port->up.netdev));
                bundle_del_port(port);
                log_event("LAG_REM_PORT_BITMAP",
                        EV_KV("interface", "%s", bundle->name),
                        EV_KV("port", "%s", netdev_get_name(port->up.netdev)));
                found: ;
            }
        }

        ovs_assert(list_size(&bundle->ports) <= s->n_slaves);

        if (list_is_empty(&bundle->ports)) {
            if (s->hw_bond_should_exist == false) {
                VLOG_DBG("%s calling bundle destroy",__FUNCTION__);
                bundle_destroy(bundle);
                return 0;
            }
        }
    }

    /* NOTE: "bundle" holds previous VLAN configuration (if any).
     * "s" holds current desired VLAN configuration. */

    /* Figure out the new set of VLANs to configure.  If bundle's
     * vlan_mode is one of trunks (trunk, native_tagged, native_untagged),
     * and s->trunks is NULL, that means the bundle is implicitly trunking
     * all VLANs.  Use the global bitmap of all VLANs.
     */
    VLOG_DBG("%s vlan mode = %d", __FUNCTION__,s->vlan_mode);
    if (s->vlan_mode != PORT_VLAN_ACCESS) {
        if (s->trunks != NULL) {
            new_trunks = s->trunks;
        } else {
            new_trunks = ofproto_->vlans_bmp;
            trunk_all_vlans = true;
        }
    }

    /* If no interface was configured before, just
     * configure everything on the new ports. */
    if (bundle->pbm == NULL || bcmsdk_pbmp_is_empty(bundle->pbm)) {
        config_all_vlans(s->vlan_mode, s->vlan, new_trunks, all_pbm);
        goto done;
    }

    /* Allocate temporary port bitmap to figure out
     * what interfaces have been added/deleted from port. */
    temp_pbm = bcmsdk_alloc_pbmp();
    if (temp_pbm == NULL) {
        return ENOMEM;
    }

    /* First, unconfigure any physical interface that has
     * been removed from this logical port. */
    bcmsdk_pbmp_remove(temp_pbm, bundle->pbm, all_pbm);
    if (!bcmsdk_pbmp_is_empty(temp_pbm)) {
        unconfig_all_vlans(bundle->vlan_mode, bundle->vlan,
                           bundle->trunks, temp_pbm);
        bcmsdk_clear_pbmp(temp_pbm);
    }

    /* Next, configure all VLANs on any new interface. */
    bcmsdk_pbmp_remove(temp_pbm, all_pbm, bundle->pbm);
    if (!bcmsdk_pbmp_is_empty(temp_pbm)) {
        config_all_vlans(s->vlan_mode, s->vlan, new_trunks, temp_pbm);
        bcmsdk_clear_pbmp(temp_pbm);
    }

    /* For existing interfaces, configure only changed VLANs. */
    bcmsdk_pbmp_and(temp_pbm, all_pbm, bundle->pbm);
    if (!bcmsdk_pbmp_is_empty(temp_pbm)) {
        int mode_changed = (bundle->vlan_mode != s->vlan_mode);
        int tag_changed = (bundle->vlan != s->vlan);

        /* Check for mode changes first. */
        if (mode_changed) {
           if (bundle->vlan_mode == PORT_VLAN_ACCESS && s->vlan_mode == PORT_VLAN_TRUNK) {
                /* Was ACCESS type, becoming one of the TRUNK types. */
                if (bundle->vlan != -1) {
                    bcmsdk_del_access_ports(bundle->vlan, temp_pbm);
                }

                /* Add all new trunk VLANs. */
                config_all_vlans(s->vlan_mode, s->vlan,
                                 new_trunks, temp_pbm);
                bcmsdk_add_native_untagged_ports(DEFAULT_VID, temp_pbm, false);

                /* Should have nothing else to do... */
                goto label_bcmsdk_destroy_pbmp;

            } else if (s->vlan_mode == PORT_VLAN_ACCESS) {
                /* Was one of the TRUNK types (trunk, native-tagged,
                 * or native-untagged), becoming ACCESS type. */
                unconfig_all_vlans(bundle->vlan_mode, bundle->vlan,
                                   bundle->trunks, temp_pbm);
                bcmsdk_del_native_untagged_ports(DEFAULT_VID, temp_pbm, false);
                /* Add new access VLAN. */
                if (s->vlan != -1) {
                    bcmsdk_add_access_ports(s->vlan, temp_pbm);
                }

                /* Should have nothing else to do... */
                goto label_bcmsdk_destroy_pbmp;

            } else {
                /* Changing modes among one of the TRUNK types (trunk,
                 * native-tagged, or native-untagged). */

                /* Unconfigure old native tag settings first. */
                switch (bundle->vlan_mode) {
                case PORT_VLAN_NATIVE_TAGGED:
                    if (bundle->vlan != -1) {
                        bcmsdk_del_native_tagged_ports(bundle->vlan, temp_pbm);

                        /* If the native VLAN we just unconfigured is also listed
                         * explicitly as part of the trunks, need to add it back. */
                        if (bitmap_is_set(bundle->trunks, bundle->vlan)) {
                            bcmsdk_add_trunk_ports(bundle->vlan, temp_pbm);
                        }
                    }
                    break;
                case PORT_VLAN_NATIVE_UNTAGGED:
                    if (bundle->vlan != -1) {
                        bcmsdk_del_native_untagged_ports(bundle->vlan, temp_pbm, false);

                        /* If the native VLAN we just unconfigured is also listed
                         * explicitly as part of the trunks, need to add it back. */
                        if (bitmap_is_set(bundle->trunks, bundle->vlan)) {
                            bcmsdk_add_trunk_ports(bundle->vlan, temp_pbm);
                        }
                    }
                    break;
                case PORT_VLAN_ACCESS:
                     if (bundle->vlan != -1) {
                        bcmsdk_del_access_ports(bundle->vlan, temp_pbm);
                     }

                    /* Add all new trunk VLANs. */
                     config_all_vlans(s->vlan_mode, s->vlan,
                                 new_trunks, temp_pbm);
                    break;
                case PORT_VLAN_TRUNK:
                default:
                    break;
                }

                /* Configure new native tag settings. */
                switch (s->vlan_mode) {
                case PORT_VLAN_NATIVE_TAGGED:
                    if (s->vlan != -1) {
                        bcmsdk_del_native_untagged_ports(s->vlan, temp_pbm, false);
                        bcmsdk_add_native_tagged_ports(s->vlan, temp_pbm);
                    }
                    break;
                case PORT_VLAN_NATIVE_UNTAGGED:
                    if (bundle->vlan == 1) {
                        bcmsdk_del_native_untagged_ports(DEFAULT_VID, temp_pbm, false);
                    }

                    if (s->vlan != -1) {
                        bcmsdk_add_native_untagged_ports(s->vlan, temp_pbm, false);
                    }
                    break;
                case PORT_VLAN_TRUNK:
                    bcmsdk_add_native_untagged_ports(DEFAULT_VID, temp_pbm, false);
                    break;
                case PORT_VLAN_ACCESS:
                default:
                    break;
                }
            } /* Changing modes among one of the TRUNK types */

        } else if (tag_changed) {
            /* VLAN mode didn't change, but VLAN tag changed. */
            switch (bundle->vlan_mode) {
            case PORT_VLAN_ACCESS:
                if (bundle->vlan != -1) {
                    bcmsdk_del_access_ports(bundle->vlan, temp_pbm);
                }
                if (s->vlan != -1) {
                    bcmsdk_add_access_ports(s->vlan, temp_pbm);
                }
                /* For ACCESS ports, nothing more to do. */
                goto label_bcmsdk_destroy_pbmp;
                break;
            case PORT_VLAN_NATIVE_TAGGED:
                if (bundle->vlan != -1) {
                    bcmsdk_del_native_tagged_ports(bundle->vlan, temp_pbm);
                    /* If the native VLAN we just unconfigured is also listed
                     * explicitly as part of the trunks, need to add it back. */
                    if (bitmap_is_set(bundle->trunks, bundle->vlan)) {
                        bcmsdk_add_trunk_ports(bundle->vlan, temp_pbm);
                    }
                }
                if (s->vlan != -1) {
                    bcmsdk_add_native_tagged_ports(s->vlan, temp_pbm);
                }
                break;
            case PORT_VLAN_NATIVE_UNTAGGED:
                if (bundle->vlan != -1) {
                    bcmsdk_del_native_untagged_ports(bundle->vlan, temp_pbm, false);
                    /* If the native VLAN we just unconfigured is also listed
                     * explicitly as part of the trunks, need to add it back. */
                    if (bitmap_is_set(bundle->trunks, bundle->vlan)) {
                        bcmsdk_add_trunk_ports(bundle->vlan, temp_pbm);
                    }
                }
                if (s->vlan != -1) {
                    bcmsdk_add_native_untagged_ports(s->vlan, temp_pbm, false);
                }
                break;
            case PORT_VLAN_TRUNK:
            default:
                break;
            }
        }

        /* Now that we've handled VLAN mode or VLAN tag changes, check &
           update the rest of TRUNK VLANs (tagged VLANs only). */
        if (s->vlan_mode != PORT_VLAN_ACCESS) {
            handle_trunk_vlan_changes(bundle->trunks, new_trunks, temp_pbm,
                                      bundle->vlan_mode, s->vlan_mode);
        }
    }

label_bcmsdk_destroy_pbmp:

    /* Done with temp_pbm. */
    bcmsdk_destroy_pbmp(temp_pbm);

done:
    /* Save enable/disable on bundle */
    bundle->enable = s->enable;
    /* Done with VLAN configuration.  Save the new information. */
    bundle->vlan_mode = s->vlan_mode;
    bundle->vlan = s->vlan;

    if (bundle->pbm != NULL) {
        bcmsdk_destroy_pbmp(bundle->pbm);
    }
    bundle->pbm = all_pbm;
    if (bundle->trunks != NULL) {
        bitmap_free(bundle->trunks);
    }
    bundle->trunks = vlan_bitmap_clone(new_trunks);
    bundle->trunk_all_vlans = trunk_all_vlans;

    return 0;
}

static void
bundle_remove(struct ofport *port_)
{
    struct bcmsdk_provider_ofport_node *port =
        bcmsdk_provider_ofport_node_cast(port_);
    struct ofbundle *bundle = port->bundle;

    if (bundle) {
        if (bundle->bond_hw_handle == -1) {
        VLOG_DBG("bundle remove, removing bundle %s\n", bundle->name);
            bundle_del_port(port);
            if (list_is_empty(&bundle->ports)) {
                bundle_destroy(bundle);
            }
        }
    }
}

static int
bundle_get(struct ofproto *ofproto_, void *aux, int *bundle_handle)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofbundle *bundle;

    bundle = bundle_lookup(ofproto, aux);
    if (bundle) {
        *bundle_handle = bundle->bond_hw_handle;
    } else {
        *bundle_handle = -1;
    }

    return 0;
}

/* VLANs. */

static int
set_vlan(struct ofproto *ofproto, int vid, bool add)
{
    struct ofbundle *bundle;
    struct bcmsdk_provider_node *bcm_ofproto = bcmsdk_provider_node_cast(ofproto);

    VLOG_DBG("%s: entry, vid=%d, oper=%s", __FUNCTION__, vid, (add ? "add":"del"));

    if (add) {
        /* Create VLAN first. */
        bcmsdk_create_vlan(vid, false);
        set_created_by_user(vid, 1);

        /* Add this VLAN to any port that's implicitly trunking all VLANs. */
        HMAP_FOR_EACH (bundle, hmap_node, &bcm_ofproto->bundles) {
            if (bundle->trunk_all_vlans) {
                bcmsdk_add_trunk_ports(vid, bundle->pbm);
                bitmap_set1(bundle->trunks, vid);
            }
        }

    } else {
        /* Delete this VLAN from any port that's implicitly trunking all VLANs. */
        HMAP_FOR_EACH (bundle, hmap_node, &bcm_ofproto->bundles) {
            if (bundle->trunk_all_vlans) {
                bitmap_set0(bundle->trunks, vid);
                bcmsdk_del_trunk_ports(vid, bundle->pbm);
            }
        }
        set_created_by_user(vid, 0);

        /* Delete VLAN. */
        bcmsdk_destroy_vlan(vid, false);
    }
    return 0;
}

static void
forward_bpdu_changed(struct ofproto *ofproto_ OVS_UNUSED)
{
    return;
}

/* Ports. */

static struct bcmsdk_provider_ofport_node *
get_ofp_port(const struct bcmsdk_provider_node *ofproto, ofp_port_t ofp_port)
{
    struct ofport *ofport = ofproto_get_port(&ofproto->up, ofp_port);
    return ofport ? bcmsdk_provider_ofport_node_cast(ofport) : NULL;
}

static int
port_query_by_name(const struct ofproto *ofproto_, const char *devname,
                   struct ofproto_port *ofproto_port)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    const char *type = netdev_get_type_from_name(devname);

    VLOG_DBG("port_query_by_name - %s", devname);

    /* We must get the name and type from the netdev layer directly. */
    if (type) {
        const struct ofport *ofport;

        ofport = shash_find_data(&ofproto->up.port_by_name, devname);
        ofproto_port->ofp_port = ofport ? ofport->ofp_port : OFPP_NONE;
        ofproto_port->name = xstrdup(devname);
        ofproto_port->type = xstrdup(type);
        VLOG_DBG("get_ofp_port name= %s type= %s flow# %d",
                   ofproto_port->name, ofproto_port->type, ofproto_port->ofp_port);
        return 0;
    }
    return ENODEV;

}

static int
port_add(struct ofproto *ofproto_, struct netdev *netdev)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    const char *devname = netdev_get_name(netdev);

    sset_add(&ofproto->ports, devname);

    if (!ofproto_is_ofdpa(ofproto_)) {
        ops_sflow_add_port(netdev);
    }
    return 0;
}

static int
port_del(struct ofproto *ofproto_, ofp_port_t ofp_port)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct bcmsdk_provider_ofport_node *ofport OVS_UNUSED =
           get_ofp_port(ofproto, ofp_port);
    int error = 0;

    return error;
}

static int
port_get_stats(const struct ofport *ofport_, struct netdev_stats *stats)
{
    struct bcmsdk_provider_ofport_node *ofport =
           bcmsdk_provider_ofport_node_cast(ofport_);
    int error;

    error = netdev_get_stats(ofport->up.netdev, stats);

    if (!error && ofport_->ofp_port == OFPP_LOCAL) {
        struct bcmsdk_provider_node *ofproto =
               bcmsdk_provider_node_cast(ofport->up.ofproto);

        ovs_mutex_lock(&ofproto->stats_mutex);
        /* ofproto->stats.tx_packets represents packets that we created
         * internally and sent to some port
         * Account for them as if they had come from OFPP_LOCAL and
         * got forwarded.
         */

        if (stats->rx_packets != UINT64_MAX) {
            stats->rx_packets += ofproto->stats.tx_packets;
        }

        if (stats->rx_bytes != UINT64_MAX) {
            stats->rx_bytes += ofproto->stats.tx_bytes;
        }

        /* ofproto->stats.rx_packets represents packets that were received on
         * some port and we processed internally and dropped (e.g. STP).
         * Account for them as if they had been forwarded to OFPP_LOCAL.
         */

        if (stats->tx_packets != UINT64_MAX) {
            stats->tx_packets += ofproto->stats.rx_packets;
        }

        if (stats->tx_bytes != UINT64_MAX) {
            stats->tx_bytes += ofproto->stats.rx_bytes;
        }
        ovs_mutex_unlock(&ofproto->stats_mutex);
    }

    return error;
}

static int
port_dump_start(const struct ofproto *ofproto_ OVS_UNUSED, void **statep)
{
    VLOG_DBG("%s", __FUNCTION__);
    *statep = xzalloc(sizeof(struct bcmsdk_provider_port_dump_state));
    return 0;
}

static int
port_dump_next(const struct ofproto *ofproto_, void *state_,
               struct ofproto_port *port)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct bcmsdk_provider_port_dump_state *state = state_;
    const struct sset *sset;
    struct sset_node *node;

    if (state->has_port) {
        ofproto_port_destroy(&state->port);
        state->has_port = false;
    }
    sset = state->ghost ? &ofproto->ghost_ports : &ofproto->ports;
    while ((node = sset_at_position(sset, &state->bucket, &state->offset))) {
        int error;

        VLOG_DBG("port dump loop detecting port %s", node->name);

        error = port_query_by_name(ofproto_, node->name, &state->port);
        if (!error) {
            VLOG_DBG("port dump loop reporting port struct %s",
                       state->port.name);
            *port = state->port;
            state->has_port = true;
            return 0;
        } else if (error != ENODEV) {
            return error;
        }
    }

    if (!state->ghost) {
        state->ghost = true;
        state->bucket = 0;
        state->offset = 0;
        return port_dump_next(ofproto_, state_, port);
    }

    return EOF;
}

static int
port_dump_done(const struct ofproto *ofproto_ OVS_UNUSED, void *state_)
{
    struct bcmsdk_provider_port_dump_state *state = state_;
    VLOG_DBG("%s", __FUNCTION__);

    if (state->has_port) {
        ofproto_port_destroy(&state->port);
    }
    free(state);
    return 0;
}

static struct bcmsdk_provider_rule
              *bcmsdk_provider_rule_cast(const struct rule *rule)
{
    return rule ? CONTAINER_OF(rule, struct bcmsdk_provider_rule, up) : NULL;
}

static inline void bcmsdk_provider_rule_ref(struct bcmsdk_provider_rule *rule)
{
    if (rule) {
        ofproto_rule_ref(&(rule->up));
    }
}

static inline void bcmsdk_provider_rule_unref(struct bcmsdk_provider_rule *rule)
{
    if (rule) {
        ofproto_rule_unref(&(rule->up));
    }
}

static struct rule *
rule_alloc(void)
{
    struct bcmsdk_provider_rule *rule = xmalloc(sizeof *rule);
    return &rule->up;
}

static void
rule_dealloc(struct rule *rule_)
{
    struct bcmsdk_provider_rule *rule = bcmsdk_provider_rule_cast(rule_);
    free(rule);
}

static enum ofperr
rule_construct(struct rule *rule_)
    OVS_NO_THREAD_SAFETY_ANALYSIS
{
    struct bcmsdk_provider_rule *rule = bcmsdk_provider_rule_cast(rule_);

    VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);
    ovs_mutex_init(&rule->stats_mutex);
    rule->new_rule = NULL;

    return 0;
}

static void rule_insert(struct rule *rule_, struct rule *old_rule_,
                        bool forward_stats)
OVS_REQUIRES(ofproto_mutex)
{
    struct bcmsdk_provider_rule *rule = bcmsdk_provider_rule_cast(rule_);
    enum ofperr error;

    if (ofproto_is_ofdpa(rule->up.ofproto)) {
        VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);

        if (old_rule_ == NULL) {
            VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);

            ovs_mutex_lock(&rule->stats_mutex);

            error = ofdpa_flow_add(rule_);

            ovs_mutex_unlock(&rule->stats_mutex);

            if (error) {
                VLOG_ERR("Error adding flow. [error = %d]", error);
            }
        } else {
            struct bcmsdk_provider_rule *old_rule = bcmsdk_provider_rule_cast(old_rule_);

            VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);

           /* Take a reference to the new rule, and refer all stats updates from
            * the old rule to the new rule. */
            bcmsdk_provider_rule_ref(rule);

            ovs_mutex_lock(&old_rule->stats_mutex);
            ovs_mutex_lock(&rule->stats_mutex);

            error = ofdpa_flow_modify(rule_);
            old_rule->new_rule = rule;

            ovs_mutex_unlock(&rule->stats_mutex);
            ovs_mutex_unlock(&old_rule->stats_mutex);

            if (error) {
                VLOG_ERR("Error modifying flow. [error = %d]", error);
            }
        }
    }

    return;
}

static void
rule_delete(struct rule *rule_)
    OVS_REQUIRES(ofproto_mutex)
{
    struct bcmsdk_provider_rule *rule = bcmsdk_provider_rule_cast(rule_);
    enum ofperr error;

    if (ofproto_is_ofdpa(rule->up.ofproto)) {
        VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);

        ovs_mutex_lock(&rule->stats_mutex);

       /*
        * Do not delete flows that were the "old" rule in a call to
        * rule_insert() for a modify operation. This is because OF-DPA
        * modifies the rule in place, rather than adding the new rule
        * along with the old rule. When OVS invokes rule_delete() for
        * the "old" rule, there is no "old" rule in OF-DPA to delete.
        */
        if (!rule->new_rule) {
            error = ofdpa_flow_delete(rule_);
            if (error) {
                VLOG_ERR("Error deleting flow. [error = %d]", error);
            }
        }
        ovs_mutex_unlock(&rule->stats_mutex);
    }
    return;
}

static void
rule_destruct(struct rule *rule_)
{
    struct bcmsdk_provider_rule *rule = bcmsdk_provider_rule_cast(rule_);

    ovs_mutex_destroy(&rule->stats_mutex);

    /* Release reference to the new rule, if any. */
    if (rule->new_rule) {
        bcmsdk_provider_rule_unref(rule->new_rule);
    }

    return;
}

static void
rule_get_stats(struct rule *rule_ OVS_UNUSED, uint64_t *packets OVS_UNUSED,
               uint64_t *bytes OVS_UNUSED, long long int *used OVS_UNUSED)
{
    return;
}

static enum ofperr
rule_execute(struct rule *rule OVS_UNUSED, const struct flow *flow OVS_UNUSED,
             struct dp_packet *packet OVS_UNUSED)
{
    return 0;
}

static struct bcmsdk_provider_group
              *bcmsdk_provider_group_cast(const struct ofgroup *group)
{
    return group ? CONTAINER_OF(group, struct bcmsdk_provider_group, up) : NULL;
}

static struct ofgroup *
group_alloc(void)
{
    struct bcmsdk_provider_group *group = xzalloc(sizeof *group);
    return &group->up;
}

static void
group_dealloc(struct ofgroup *group_)
{
    struct bcmsdk_provider_group *group = bcmsdk_provider_group_cast(group_);
    free(group);
}

static enum ofperr
group_construct(struct ofgroup *group_)
{
    struct bcmsdk_provider_group *group = bcmsdk_provider_group_cast(group_);
    enum ofperr error;

    VLOG_DBG("==========in ofdpa %s, %d", __FUNCTION__,__LINE__);
    ovs_mutex_init(&group->stats_mutex);
    ovs_mutex_lock(&group->stats_mutex);
    error = ofdpa_group_add(group);
    if (error) {
        VLOG_ERR("Error adding group entry. [rc = %d]", error);
    }
    ovs_mutex_unlock(&group->stats_mutex);

    return error;
}

static void
group_destruct(struct ofgroup *group_)
{
    struct bcmsdk_provider_group *group = bcmsdk_provider_group_cast(group_);

    if (ofproto_is_ofdpa(group->up.ofproto)) {
        ovs_mutex_lock(&group->stats_mutex);
        ofdpa_group_delete(group->up.group_id);
        ovs_mutex_unlock(&group->stats_mutex);
        ovs_mutex_destroy(&group->stats_mutex);
    }

    return;
}

static enum ofperr
group_modify(struct ofgroup *group_)
{
    struct bcmsdk_provider_group *group = bcmsdk_provider_group_cast(group_);
    enum ofperr error;

    error = 0;

    if (ofproto_is_ofdpa(group->up.ofproto)) {
        ovs_mutex_lock(&group->stats_mutex);
        error = ofdpa_group_modify(group);
        if(error) {
            VLOG_ERR("Error modifying group entry. [rc = %d]", error);
        }
        ovs_mutex_unlock(&group->stats_mutex);
    }

    return error;
}

static enum ofperr
group_get_stats(const struct ofgroup *group_ OVS_UNUSED,
                struct ofputil_group_stats *ogs OVS_UNUSED)
{
    return 0;
}

static const char *
get_datapath_version(const struct ofproto *ofproto_)
{
    if (ofproto_is_ofdpa(ofproto_)) {
#if 0 /* use call to OF-DPA API when available */
        return ofdpaVersionStringGet();
#else
        return "0.0.0.1";
#endif
    }
    else {
        return bcmsdk_datapath_version();
    }
}

static bool
set_frag_handling(struct ofproto *ofproto_ OVS_UNUSED,
                  enum ofp_config_flags frag_handling OVS_UNUSED)
{
    return false;
}

static enum ofperr
packet_out(struct ofproto *ofproto_ OVS_UNUSED, struct dp_packet *packet OVS_UNUSED,
           const struct flow *flow OVS_UNUSED,
           const struct ofpact *ofpacts OVS_UNUSED, size_t ofpacts_len OVS_UNUSED)
{
    return 0;
}

static void
get_netflow_ids(const struct ofproto *ofproto_ OVS_UNUSED,
                uint8_t *engine_type OVS_UNUSED, uint8_t *engine_id OVS_UNUSED)
{
    return;
}

/* Ft to add l3 host entry */
static int
add_l3_host_entry(const struct ofproto *ofproto_, void *aux,
                  bool is_ipv6_addr, char *ip_addr,
                  char *next_hop_mac_addr, int *l3_egress_id)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofbundle *port_bundle;
    struct bcmsdk_provider_ofport_node *port = NULL, *next_port;
    int rc = 0;

    port_bundle = bundle_lookup(ofproto, aux);
    if ( (port_bundle == NULL) ||
         (port_bundle->l3_intf == NULL) ) {
        VLOG_ERR("Failed to get port bundle/l3_intf not configured");
        return 1; /* Return error */
    }

    rc = ops_routing_add_host_entry(port_bundle->hw_unit, port_bundle->hw_port,
                                   ofproto->vrf_id, is_ipv6_addr,
                                   ip_addr, next_hop_mac_addr,
                                   port_bundle->l3_intf->l3a_intf_id,
                                   l3_egress_id,
                                   port_bundle->l3_intf->l3a_vid,
                                   port_bundle->bond_hw_handle);
    if (rc) {
        VLOG_ERR("Failed to add L3 host entry for ip %s", ip_addr);
        return rc;
    }

    LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &port_bundle->ports) {
        /* Break because we are looking for the first slave */
        VLOG_DBG("port_t->up.ofp_port = %d\n", port->up.ofp_port);
        VLOG_DBG(" add numslaves = %zu", list_size(&port_bundle->ports));
        break;
    }

    if (!port) {
        VLOG_ERR("Failed to get port while adding l3 host entry");
        return 1; /* Return error */
        log_event("L3INTERFACE_ADD_HOST_ERR",
                  EV_KV("ipaddr", "%s", ip_addr),
                  EV_KV("egressid", "%d", *l3_egress_id),
                  EV_KV("err", "%s", opennsl_errmsg(rc)));
    }
    log_event("L3INTERFACE_ADD_HOST",
            EV_KV("ipaddr", "%s", ip_addr),
            EV_KV("egressid", "%d", *l3_egress_id));

    if (list_size(&port_bundle->ports) == 1) {
        rc = netdev_bcmsdk_set_l3_egress_id(port->up.netdev, *l3_egress_id);
        if (rc) {
            VLOG_ERR("Failed to set l3 egress data in netdev bcmsdk for egress"
                     " object, %d", *l3_egress_id);
        }
    }

    return rc;
} /* add_l3_host_entry */

/* Ft to delete l3 host entry */
static int
delete_l3_host_entry(const struct ofproto *ofproto_, void *aux,
                     bool is_ipv6_addr, char *ip_addr, int *l3_egress_id)

{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofbundle *port_bundle;
    struct bcmsdk_provider_ofport_node *port = NULL, *next_port;
    int rc = 0;

    port_bundle = bundle_lookup(ofproto, aux);
    if (port_bundle == NULL) {
        VLOG_ERR("Failed to get port bundle");
        return 1; /* Return error */
    }

    LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &port_bundle->ports) {
        /* Break because we are looking for the first slave */
        VLOG_DBG("port_t->up.ofp_port = %d\n", port->up.ofp_port);
        VLOG_DBG(" delete numslaves = %zu", list_size(&port_bundle->ports));
        break;
    }

    if (!port) {
        VLOG_ERR("Failed to get port while deleting l3 host entry");
        return 1; /* Return error */
    }

    if ((list_size(&port_bundle->ports) == 1)) {
        rc = netdev_bcmsdk_remove_l3_egress_id(port->up.netdev, *l3_egress_id);
        if (rc) {
            VLOG_ERR("Failed to remove l3 egress data in netdev bcmsdk for "
                     " egress object, %d", *l3_egress_id);
        }
    }

    rc = ops_routing_delete_host_entry(port_bundle->hw_unit,
                                      port_bundle->hw_port,
                                      ofproto->vrf_id, is_ipv6_addr, ip_addr,
                                      l3_egress_id);
    if (rc) {
        VLOG_ERR("Failed to delete L3 host entry for ip %s", ip_addr);
        log_event("L3INTERFACE_DEL_HOST_ERR",
                   EV_KV("ipaddr", "%s", ip_addr),
                   EV_KV("egressid", "%d", *l3_egress_id),
                   EV_KV("err", "%s", opennsl_errmsg(rc)));
    } else {
        log_event("L3INTERFACE_DEL_HOST",
                   EV_KV("ipaddr", "%s", ip_addr),
                   EV_KV("egressid", "%d", *l3_egress_id));
    }

    return rc;
} /* delete_l3_host_entry */

static int
set_sflow(struct ofproto *ofproto_,
          const struct ofproto_sflow_options *oso)
{
    int ret;
    uint32_t rate;
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    uint32_t header;
    uint32_t datagram;

    if (oso == NULL) { /* disable sflow */
        VLOG_DBG("%s : Input sflow options are NULL (maybe sflow (or collectors) is "
                "not configured or disabled)", ofproto->up.name);

        ops_sflow_agent_disable(ofproto);

        if (sflow_options) {
            free(sflow_options);
            sflow_options = NULL;
        }
        return 0;
    }

    /* No targets/collectors, sFlow Agent can't exist. */
    if (sset_is_empty(&oso->targets)) {
        VLOG_DBG("sflow targets not set. Disable sFlow Agent.");

        ops_sflow_agent_disable(ofproto);

        /* if 'sflow_options' have any targets, clear them. */
        if (sflow_options) {
            sset_clear(&sflow_options->targets);
        }
        return 0;
    }

    /* Sampling rate not configured in CLI. Can't create sFlow Agent. This
     * scenario is only possible at initiation. Once sFlow Agent is created,
     * it will always have a sampling rate (a default rate, at least). */
    if (oso->sampling_rate == 0) {
        VLOG_DBG("sflow sampling_rate not set. Cannot create sFlow Agent.");
        return 0;
    }

    if (!oso->agent_ip || (strlen(oso->agent_ip) == 0)) {
        VLOG_DBG("sflow agent IP not found. Disable sFlow Agent.");
        ops_sflow_agent_disable(ofproto);
        if (sflow_options) {
            memset(sflow_options->agent_ip, 0, sizeof(sflow_options->agent_ip));
        }
        return 0;
    }

    /* sFlow Agent create, for first time. */
    if (sflow_options == NULL) {
        VLOG_DBG("sflow: Initialize sFlow agent with input options.");
        ops_sflow_agent_enable(ofproto, oso);
        return 0;
    } else if (ops_sflow_options_equal(oso, sflow_options)) {
        /* polling interval change checked later for each port. */
        ops_sflow_set_polling_interval(ofproto, sflow_options->polling_interval);
        VLOG_DBG("sflow: options are unchanged.");
        return 0;
    }

    /* sFlow Agent exists. It's options has changed, update Agent. */

    VLOG_DBG("sflow config: sampling: %d, num_targets: %zd, hdr len: %d,"
             "agent dev: %s, agent ip: %s, polling : %d",
            oso->sampling_rate, sset_count(&oso->targets), oso->header_len,
            oso->agent_device ? oso->agent_device : "NULL",
            oso->agent_ip ? oso->agent_ip : "NULL", oso->polling_interval);

    /* Sampling rate has changed. */
    rate = oso->sampling_rate;
    if (sflow_options->sampling_rate != rate) {
        sflow_options->sampling_rate = rate;
        ops_sflow_set_sampling_rate(0, 0, rate, rate);
        VLOG_DBG("sflow: sampling rate %d applied on sFlow Agent", rate);
    }


    /* Max datagram size has changed. */
    datagram = oso->max_datagram;
    if (sflow_options->max_datagram != datagram) {
        sflow_options->max_datagram = datagram;
        ops_sflow_set_max_datagram_size(datagram);
        VLOG_DBG("sflow: max datagram set to %d", datagram);
    }

    /* Header size has changed. */
    header = oso->header_len;
    if (sflow_options->header_len != header) {
        sflow_options->header_len = header;
        ops_sflow_set_header_size(header);
        VLOG_DBG("sflow: header size set to %d", header);
    }

    /* source IP for sFlow agent */
    if (strncmp(sflow_options->agent_ip, oso->agent_ip, sizeof(oso->agent_ip))) {
        memset(sflow_options->agent_ip, 0, sizeof(sflow_options->agent_ip));
        strncpy(sflow_options->agent_ip, oso->agent_ip,
                sizeof(sflow_options->agent_ip) - 1);
        ops_sflow_agent_ip(oso->agent_ip);
        VLOG_DBG("sflow: agent_ip changed to '%s'", oso->agent_ip);
    }

    if (!sset_equals(&oso->targets, &sflow_options->targets)) {
        /* collectors has changed. destroy and create again. */
        sset_destroy(&sflow_options->targets);
        sset_clone(&sflow_options->targets, &oso->targets);
        if ((ret = ops_sflow_set_collectors(&sflow_options->targets)) != 0) {
            /* couldn't configure collectors. retry next time */
            VLOG_DBG("sflow: couldn't configure collectors. ret %d", ret);
            sset_clear(&sflow_options->targets);
        }
    }

    /* polling interval change checked later for each port. */
    sflow_options->polling_interval = oso->polling_interval;
    ops_sflow_set_polling_interval(ofproto, sflow_options->polling_interval);

    return 0;
}

/* Ft to get BCM host data-path hit-bit */
static int
get_l3_host_hit_bit(const struct ofproto *ofproto_, void *aux,
                    bool is_ipv6_addr, char *ip_addr, bool *hit_bit)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct ofbundle *port_bundle;
    int rc = 0;

    port_bundle = bundle_lookup(ofproto, aux);
    if (port_bundle == NULL) {
        VLOG_ERR("Failed to get port bundle");
        return 1; /* Return error */
    }

    rc = ops_routing_get_host_hit(port_bundle->hw_unit, ofproto->vrf_id,
                                 is_ipv6_addr, ip_addr, hit_bit);
    if (rc) {
        VLOG_ERR("Failed to get L3 host hit for ip %s", ip_addr);
        log_event("L3INTERFACE_HITBIT_FAILURE",
                   EV_KV("ipaddr", "%s", ip_addr));
    }

    return rc;
} /* netdev_bcmsdk_get_host_hit */

/* Function to add, update, delete l3 route */
static int
l3_route_action(const struct ofproto *ofprotop,
                enum ofproto_route_action action,
                struct ofproto_route *routep)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofprotop);

    return ops_routing_route_entry_action(0, ofproto->vrf_id, action, routep);
}

/* Function to enable/disable ECMP */
int
l3_ecmp_set(const struct ofproto *ofprotop, bool enable)
{
    return ops_routing_ecmp_set(0, enable);
}

/* Function to enable/disable ECMP hash configs */
int
l3_ecmp_hash_set(const struct ofproto *ofprotop, unsigned int hash, bool enable)
{
    return ops_routing_ecmp_hash_set(0, hash, enable);
}

/* QoS */
int
set_port_qos_cfg(struct ofproto *ofproto_,
                 void *aux,  /* struct port *port */
                 const struct  qos_port_settings *cfg)
{
    struct bcmsdk_provider_node *ofproto = bcmsdk_provider_node_cast(ofproto_);
    struct bcmsdk_provider_ofport_node *port, *next_port;
    int hw_unit, hw_port;
    uint8_t mac[ETH_ADDR_LEN];
    int rc = OPS_QOS_SUCCESS_CODE;

    struct ofbundle *bundle = bundle_lookup(ofproto, aux);
    if (bundle) {
        VLOG_DBG("set port qos cfg: name %s ofproto@ %p port %s, "
                 "settings->qos_trust %d, cfg@ %p",
                  ofproto_->name, ofproto, bundle->name,
                  cfg->qos_trust, cfg->other_config);

        VLOG_DBG("bundle hw_unit %d, hw_port %d, bond_hw_handle %d bond %p",
                  bundle->hw_unit, bundle->hw_port, bundle->bond_hw_handle,
                  bundle->bond);

        /*
         * Set port qos cfg could be for individual port or LAGs. The following
         * code should take care of both cases.
         */
        LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {
            netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port,
                                      mac);

            VLOG_DBG("port hw_unit %d, hw_port %d, bond_hw_handle %d bond %p",
                       hw_unit, hw_port, bundle->bond_hw_handle, bundle->bond);

            if (VALID_HW_UNIT(hw_unit) && VALID_HW_UNIT_PORT(hw_unit,
                hw_port)) {
                rc = ops_qos_set_port_cfg(bundle, hw_unit, hw_port,
                                          cfg);
            } else {
                /* Log an ERR msg and return error code */
                VLOG_ERR("Invalid hw unit %d or port %d in "
                         "set port qos cfg for %s",
                          hw_unit, hw_port, bundle->name);

                return OPS_QOS_FAILURE_CODE;
            }

        }
    } else {
        VLOG_ERR("set port qos trust: NO BUNDLE aux@%p, "
                  "settings->qos_trust %d, cfg@ %p",
                   aux, cfg->qos_trust, cfg->other_config);
        return OPS_QOS_FAILURE_CODE;
    }

    return rc;
}

int
set_cos_map(struct ofproto *ofproto_,
            void *aux,
            const struct cos_map_settings *settings)
{
    int rc;

    VLOG_DBG("QoS: set COS map");
    rc = ops_qos_set_cos_map(settings);

    return rc;
}

int
set_dscp_map(struct ofproto *ofproto_,
             void *aux,
             const struct dscp_map_settings *settings)
{
    int rc;

    VLOG_DBG("QoS: set DSCP map");
    rc = ops_qos_set_dscp_map(settings);

    return rc;
}

int
apply_qos_profile(struct ofproto *ofproto_,
                  void *aux,
                  const struct schedule_profile_settings *s_settings,
                  const struct queue_profile_settings *q_settings)
{
    struct bcmsdk_provider_node *ofproto;

    int index;
    struct queue_profile_entry *qp_entry;
    struct schedule_profile_entry *sp_entry;
    uint8_t mac[ETH_ADDR_LEN];
    struct bcmsdk_provider_ofport_node *port, *next_port;
    int hw_unit, hw_port;
    int rc = OPS_QOS_SUCCESS_CODE;
    struct ofbundle *bundle;

    if (ofproto_) {
        ofproto = bcmsdk_provider_node_cast(ofproto_);

        bundle = bundle_lookup(ofproto, aux);

        VLOG_DBG("apply_qos_profile: %s ofproto@ %p port %s aux=%p "
                 "q_settings=%p s_settings=%p",
                  ofproto_->name, ofproto, bundle->name, aux,
                  s_settings, q_settings);
    } else {
        /* This is global defaults case */
        ofproto = NULL;
        bundle = NULL;

        VLOG_DBG("apply_qos_profile: global defaults config "
                 "q_settings=%p s_settings=%p",
                  s_settings, q_settings);
    }

    for (index = 0; index < q_settings->n_entries; index++) {
        qp_entry = q_settings->entries[index];
        VLOG_DBG("... %d q=%d #lp=%d", index,
                  qp_entry->queue, qp_entry->n_local_priorities);

    }

    for (index = 0; index < s_settings->n_entries; index++) {
        sp_entry = s_settings->entries[index];
        VLOG_DBG("... %d alg=%d wt=%d", index,
                  sp_entry->algorithm, sp_entry->weight);

    }

    if (aux == NULL) {
        /* If aux is NULL, apply queue profile and return */
        VLOG_DBG("apply qos profile: applying queue profile...");

        rc = ops_qos_apply_queue_profile(s_settings, q_settings);

        return rc;
    }

    /* Apply the schedule profile now */
    if (bundle) {
        VLOG_DBG("apply qos profile: name %s ofproto@ %p port %s",
                  ofproto_->name, ofproto, bundle->name);

        VLOG_DBG("bundle hw_unit %d, hw_port %d, bond_hw_handle %d bond %p",
                  bundle->hw_unit, bundle->hw_port, bundle->bond_hw_handle,
                  bundle->bond);

        /*
         * Apply qos profile could be for individual port or LAGs.
         * The following code should take care of both cases.
         */
        LIST_FOR_EACH_SAFE (port, next_port, bundle_node, &bundle->ports) {
            netdev_bcmsdk_get_hw_info(port->up.netdev, &hw_unit, &hw_port,
                                      mac);

            VLOG_DBG("port hw_unit %d, hw_port %d, bond_hw_handle %d bond %p",
                       hw_unit, hw_port, bundle->bond_hw_handle, bundle->bond);

            if (VALID_HW_UNIT(hw_unit) && VALID_HW_UNIT_PORT(hw_unit,
                hw_port)) {
                rc = ops_qos_apply_schedule_profile(bundle,
                                        hw_unit, hw_port,
                                        s_settings, q_settings);;
            } else {
                /* Log an ERR msg and return error code */
                VLOG_ERR("Invalid hw unit %d or port %d in "
                         "apply qos profile for %s",
                          hw_unit, hw_port, bundle->name);

                return OPS_QOS_FAILURE_CODE;
            }

        }
    } else {
        VLOG_ERR("apply qos profile: NO BUNDLE aux@ %p, "
                 "ofproto_ name %s, ofproto@ %p ",
                  aux, ofproto_->name, ofproto);
        return OPS_QOS_FAILURE_CODE;
    }

    return rc;
}

static struct qos_asic_plugin_interface qos_asic_plugin = {
    set_port_qos_cfg,
    set_cos_map,
    set_dscp_map,
    apply_qos_profile
};

static struct plugin_extension_interface qos_extension = {
    QOS_ASIC_PLUGIN_INTERFACE_NAME,
    QOS_ASIC_PLUGIN_INTERFACE_MAJOR,
    QOS_ASIC_PLUGIN_INTERFACE_MINOR,
    (void *)&qos_asic_plugin
};

int
register_qos_extension(void)
{
    return(register_plugin_extension(&qos_extension));
}

int
register_classifier_plugins(void)
{
    /* Register classifier plugin extension */
     return(register_ops_cls_plugin());
}

const struct ofproto_class ofproto_bcm_provider_class = {
    init,
    enumerate_types,
    enumerate_names,
    del,
    port_open_type,
    NULL,                       /* may implement type_run */
    NULL,                       /* may implement type_wait */
    alloc,
    construct,
    destruct,
    dealloc,
    run,
    wait,
    NULL,                       /* get_memory_usage */
    NULL,                       /* may implement type_get_memory_usage */
    NULL,                       /* may implement flush */
    query_tables,
    set_tables_version,
    port_alloc,
    port_construct,
    port_destruct,
    port_dealloc,
    NULL,                       /* may implement port_modified */
    port_reconfigured,
    port_query_by_name,
    port_add,
    port_del,
    port_get_stats,
    port_dump_start,
    port_dump_next,
    port_dump_done,
    NULL,                       /* may implement port_poll */
    NULL,                       /* may implement port_poll_wait */
    NULL,                       /* may implement port_is_lacp_current */
    NULL,                       /* may implement port_get_lacp_stats */
    NULL,                       /* rule_choose_table */
    rule_alloc,
    rule_construct,
    rule_insert,
    rule_delete,
    rule_destruct,
    rule_dealloc,
    rule_get_stats,
    rule_execute,
    set_frag_handling,
    packet_out,
    NULL,                       /* may implement set_netflow */
    get_netflow_ids,
    set_sflow,                  /* may implement set_sflow */
    NULL,                       /* may implement set_ipfix */
    NULL,                       /* may implement set_cfm */
    cfm_status_changed,
    NULL,                       /* may implement get_cfm_status */
    NULL,                       /* may implement set_lldp */
    NULL,                       /* may implement get_lldp_status */
    NULL,                       /* may implement set_aa */
    NULL,                       /* may implement aa_mapping_set */
    NULL,                       /* may implement aa_mapping_unset */
    NULL,                       /* may implement aa_vlan_get_queued */
    NULL,                       /* may implement aa_vlan_get_queue_size */
    NULL,                       /* may implement set_bfd */
    bfd_status_changed,
    NULL,                       /* may implement get_bfd_status */
    NULL,                       /* may implement set_stp */
    NULL,                       /* may implement get_stp_status */
    NULL,                       /* may implement set_stp_port */
    NULL,                       /* may implement get_stp_port_status */
    NULL,                       /* may implement get_stp_port_stats */
    NULL,                       /* may implement set_rstp */
    NULL,                       /* may implement get_rstp_status */
    NULL,                       /* may implement set_rstp_port */
    NULL,                       /* may implement get_rstp_port_status */
    NULL,                       /* may implement set_queues */
    bundle_set,
    bundle_remove,
    bundle_get,
    set_vlan,

    /* mirror processing functions */
    .mirror_set = mirror_set__,
    .mirror_get_stats = mirror_get_stats__,

    NULL,                       /* may implement set_flood_vlans */

    .is_mirror_output_bundle = is_mirror_output_bundle,
    forward_bpdu_changed,
    NULL,                       /* may implement set_mac_table_config */
    NULL,                       /* may implement set_mcast_snooping */
    NULL,                       /* may implement set_mcast_snooping_port */
    NULL,                       /* set_realdev, is unused */
    NULL,                       /* meter_get_features */
    NULL,                       /* meter_set */
    NULL,                       /* meter_get */
    NULL,                       /* meter_del */
    group_alloc,                /* group_alloc */
    group_construct,            /* group_construct */
    group_destruct,             /* group_destruct */
    group_dealloc,              /* group_dealloc */
    group_modify,               /* group_modify */
    group_get_stats,            /* group_get_stats */
    get_datapath_version,       /* get_datapath_version */
    add_l3_host_entry,          /* Add l3 host entry */
    delete_l3_host_entry,       /* Delete l3 host entry */
    get_l3_host_hit_bit,        /* Get l3 host entry hit bits */
    l3_route_action,            /* l3 route action - install, update, delete */
    l3_ecmp_set,                /* enable/disable ECMP globally */
    l3_ecmp_hash_set,           /* enable/disable ECMP hash configs */
};
