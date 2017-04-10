/*
 * Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 *
 * File: ops-lag.c
 *
 * Purpose: This file contains OpenSwitch LAG related application code in the Broadcom SDK.
 */

#include <stdlib.h>
#include <sys/queue.h>

#include <openvswitch/vlog.h>
#include <opennsl/error.h>
#include <opennsl/types.h>
#include <opennsl/trunk.h>
#include <opennsl/switch.h>

#include "platform-defines.h"
#include "ops-debug.h"
#include "ops-lag.h"
#include "eventlog.h"
#include "string.h"
#include "hmap.h"
#include "hash.h"
#include "mac-learning-plugin.h"

VLOG_DEFINE_THIS_MODULE(ops_lag);

typedef struct ops_lag_data {
    TAILQ_ENTRY(ops_lag_data)  lag_data_list;

    opennsl_trunk_t lag_id;
    int lag_mode;                                // OpenNSL LAG hash mode.
    int hw_created;                              // Boolean indicating if this
                                                 // LAG has been created in in h/w.
    opennsl_pbmp_t ports_pbm[MAX_SWITCH_UNITS];  // Attached ports
    opennsl_pbmp_t egr_en_pbm[MAX_SWITCH_UNITS]; // Ports with egress enabled.

} ops_lag_data_t;

struct lag_port {
    struct hmap_node hmap_node;
    int hw_unit;
    opennsl_trunk_t lag_id;
    char port_name[PORT_NAME_SIZE];
};

static int lags_data_initialized = 0;
TAILQ_HEAD(ops_lag_data_head, ops_lag_data) ops_lags;

/* Broadcom switch chip module ID.
   OPS_TODO: Support multiple switch chips. */
#define MODID_0         0

static struct hmap all_lag_ports = HMAP_INITIALIZER(&all_lag_ports);

static ops_lag_data_t *find_lag_data(opennsl_trunk_t lag_id);
static void hw_lag_detach_port(int unit, opennsl_trunk_t lag_id, opennsl_port_t hw_port);

////////////////////////////////// DEBUG ///////////////////////////////////
static inline char *
lag_mode_to_str(int lag_mode)
{
    switch (lag_mode) {
    case OPENNSL_TRUNK_PSC_SRCDSTMAC:
        return "src_dst_MAC";
    case OPENNSL_TRUNK_PSC_SRCDSTIP:
        return "src_dst_IP";
    case OPENNSL_TRUNK_PSC_PORTFLOW:
    /* NOTE: we have to change this if we support two or more
             enhanced hashing modes later on.*/
        return "l4-src-dst";
    default:
        return "unknown";
    }

} // lag_mode_to_str

static void
show_lag_data(struct ds *ds, ops_lag_data_t *lagp)
{
    int unit;
    char pfmt[_SHR_PBMP_FMT_LEN];

    ds_put_format(ds, "LAG ID %d:\n", lagp->lag_id);
    ds_put_format(ds, "  lag_mode=%d (%s)\n", lagp->lag_mode,
                  lag_mode_to_str(lagp->lag_mode));
    ds_put_format(ds, "  hw_created=%d\n", lagp->hw_created);
    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {
        ds_put_format(ds, "  Attached ports=%s\n",
                      _SHR_PBMP_FMT(lagp->ports_pbm[unit], pfmt));
        ds_put_format(ds, "  Egress enabled ports=%s\n",
                      _SHR_PBMP_FMT(lagp->egr_en_pbm[unit], pfmt));
    }
    ds_put_format(ds, "\n");

} // show_lag_data

void
ops_lag_dump(struct ds *ds, opennsl_trunk_t lagid)
{
    ops_lag_data_t *lagp = NULL;

    if (lagid != -1) {
        lagp = find_lag_data(lagid);
        if (lagp != NULL) {
            show_lag_data(ds, lagp);
        } else {
            ds_put_format(ds, "LAG ID %d does not exist.\n", lagid);
        }
    } else {
        if (!lags_data_initialized) {
            ds_put_format(ds, "LAG data not yet initialized.\n");
        } else {
            ds_put_format(ds, "Dumping all LAGs...\n");
            for (lagp = ops_lags.tqh_first;
                 lagp != NULL;
                 lagp = lagp->lag_data_list.tqe_next ) {
                show_lag_data(ds, lagp);
            }
        }
    }

} // ops_vlan_dump

////////////////////////////////// HW API //////////////////////////////////

static void
hw_create_lag(int unit, opennsl_trunk_t *lag_id)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_trunk_info_t group_info;

    SW_LAG_DBG("entry: unit=%d, lag_id=%d", unit, *lag_id);

    rc = opennsl_trunk_create(unit, 0, lag_id);

    if (OPENNSL_SUCCESS(rc)) {

        opennsl_trunk_info_t_init(&group_info);

        // Configure the trunk group.
        group_info.dlf_index  = OPENNSL_TRUNK_UNSPEC_INDEX;
        group_info.mc_index   = OPENNSL_TRUNK_UNSPEC_INDEX;
        group_info.ipmc_index = OPENNSL_TRUNK_UNSPEC_INDEX;

        // Use SRC/DST IP as selection criteria for regular traffic.
        group_info.psc = OPENNSL_TRUNK_PSC_SRCDSTIP;

        rc = opennsl_trunk_set(unit, *lag_id, &group_info, 0, NULL);

        if (OPENNSL_SUCCESS(rc)) {
            SW_LAG_DBG("trunk set succeeds unit %d, lag_id %d\n",
                       unit, *lag_id);
            log_event("TRUNK_SET_SUCCEEDS",
                EV_KV("unit", "%d", unit),
                EV_KV("lag_id", "%d", *lag_id));
            VLOG_INFO("trunk set succeeds");
        }
    }

    if (OPENNSL_FAILURE(rc) && (rc != OPENNSL_E_EXISTS)) {
        // Ignore duplicated create requests.
        VLOG_ERR("Unit %d LAG %d create error, rc=%d (%s)",
                 unit, *lag_id, rc, opennsl_errmsg(rc));
        log_event("LAG_CREATION_FAILED",
            EV_KV("unit", "%d", unit),
            EV_KV("lag_id", "%d", *lag_id),
            EV_KV("rc=", "%d", rc),
            EV_KV("error", "%s", opennsl_errmsg(rc)));
    }

    SW_LAG_DBG("done: rc=%s", opennsl_errmsg(rc));

} // hw_create_lag

static void
hw_destroy_lag(int unit, opennsl_trunk_t lag_id)
{
    opennsl_error_t rc = OPENNSL_E_NONE;

    SW_LAG_DBG("entry: unit=%d, lag_id=%d", unit, lag_id);

    rc = opennsl_trunk_destroy(unit, lag_id);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Unit %d, LAGID %d destroy error, rc=%d (%s)",
                 unit, lag_id, rc, opennsl_errmsg(rc));
        log_event("DESTROY_LAG_FAILED",
            EV_KV("unit", "%d", unit),
            EV_KV("lag_id", "%d", lag_id),
            EV_KV("rc=", "%d", rc),
            EV_KV("error", "%s", opennsl_errmsg(rc)));
    }

    SW_LAG_DBG("done: rc=%s", opennsl_errmsg(rc));

} // hw_destroy_lag

static int
is_port_attached_to_lag(int unit, opennsl_port_t hw_port, opennsl_trunk_t *trunk_id)
{
    int rc = OPENNSL_E_NONE;

    rc = opennsl_trunk_find(unit, MODID_0, hw_port, trunk_id);
    if (OPENNSL_SUCCESS(rc)) {
        return 1;
    }

    return 0;

} // is_port_attached_to_lag

static void
hw_lag_attach_port(int unit, opennsl_trunk_t lag_id, opennsl_port_t hw_port)
{
    opennsl_trunk_member_t member_element;
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_trunk_t exist_lag_id;

    SW_LAG_DBG("Trunk Attach: unit=%d, hw_port=%d, tid=%d",
               unit, hw_port, lag_id);

    // If port is already attached to trunk, take appropriate action.
    // BCM API doesn't check for duplicate ports in trunk.
    if (is_port_attached_to_lag(unit, hw_port, &exist_lag_id)) {
        ops_lag_data_t *lagp;

        if (lag_id == exist_lag_id) {
            // Don't attempt to attach again.
            // BCM API doesn't check for duplicate ports in trunk.
            SW_LAG_DBG("hw_port %d is already attached to tid %d.",
                       hw_port, lag_id);
            goto done;
        }

        // Detach port from existing LAG so that it can be added to LAG
        // identified by 'lag_id'.
        hw_lag_detach_port(unit, exist_lag_id, hw_port);
        lagp = find_lag_data(exist_lag_id);
        if (!lagp) {
            VLOG_ERR("Failed to get LAG data for LAGID %d",
                     exist_lag_id);
        } else {
            SW_LAG_DBG("Moving hw_port %d from tid %d to tid %d.",
                       hw_port, exist_lag_id, lag_id);
            OPENNSL_PBMP_PORT_REMOVE(lagp->ports_pbm[unit], hw_port);
        }
    }

    opennsl_trunk_member_t_init(&member_element);
    OPENNSL_GPORT_MODPORT_SET(member_element.gport, MODID_0, hw_port);

    // Always disable egress while attaching the port to trunk.
    // Later LACPd will unset this flag, when port is ready to transmit.
    member_element.flags = OPENNSL_TRUNK_MEMBER_EGRESS_DISABLE;

    rc = opennsl_trunk_member_add(unit, lag_id, &member_element);
    if (OPENNSL_SUCCESS(rc)) {
        SW_LAG_DBG("trunk member add succeeds unit %d, hw_port=%d, "
                   "tid %d", unit, hw_port, lag_id);
        log_event("TRUNK_MEMBER_ADD_SUCCEEDS",
            EV_KV("unit", "%d", unit),
            EV_KV("hw_port", "%d", hw_port),
            EV_KV("tid", "%d", lag_id));
    } else {
        VLOG_ERR("Trunk port attach error, hw_port %d, tid %d, "
                 "rc=%d (%s)", hw_port, lag_id, rc, opennsl_errmsg(rc));
        log_event("TRUNK_PORT_ATTACH_ERROR",
            EV_KV("hw_port", "%d", hw_port),
            EV_KV("tid", "%d", lag_id),
            EV_KV("rc", "%d", rc),
            EV_KV("error", "%s", opennsl_errmsg(rc)));
        goto done;
    }

done:
    SW_LAG_DBG("Done.");

} // hw_lag_attach_port

void
hw_lag_egress_enable_port(int unit, opennsl_trunk_t trunk_id, opennsl_port_t hw_port, int enable)
{
    int i = 0;
    int member_count;
    opennsl_port_t local_port = 0;
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_trunk_info_t trunk_info;
    opennsl_trunk_member_t member_array[OPENNSL_TRUNK_MAX_PORTCNT];

    SW_LAG_DBG("Trunk port egress enable: unit=%d, hw_port=%d, "
               "tid=%d enable=%d", unit, hw_port, trunk_id, enable);

    rc = opennsl_trunk_get(unit, trunk_id, &trunk_info,
                           OPENNSL_TRUNK_MAX_PORTCNT,
                           member_array, &member_count);
    if (OPENNSL_SUCCESS(rc)) {
        for (i = 0; i < member_count; i++) {

            local_port = OPENNSL_GPORT_MODPORT_PORT_GET(member_array[i].gport);

            if (local_port == hw_port) {
                if (enable) {
                    member_array[i].flags &= ~OPENNSL_TRUNK_MEMBER_EGRESS_DISABLE;
                } else {
                    member_array[i].flags |= OPENNSL_TRUNK_MEMBER_EGRESS_DISABLE;
                }
                rc = opennsl_trunk_set(unit, trunk_id, &trunk_info,
                                       member_count, member_array);
                if (OPENNSL_FAILURE(rc)) {
                    VLOG_ERR("Failed to set egress enable on "
                             "hw_port=%d, tid=%d, rc=%d (%s)",
                             hw_port, trunk_id, rc, opennsl_errmsg(rc));
                    log_event("FAILED_TO_SET_EGRESS_ENABLE",
                        EV_KV("hw_port=", "%d", hw_port),
                        EV_KV("tid=", "%d", trunk_id),
                        EV_KV("rc=", "%d", rc),
                        EV_KV("error", "%s", opennsl_errmsg(rc)));
                }
                break;
            }
        }
    } else {
        VLOG_ERR("Failed to get trunk member info. tid=%d, rc=%d (%s)",
                 trunk_id, rc, opennsl_errmsg(rc));
    }

    SW_LAG_DBG("Done.");

} // hw_lag_egress_enable_port

static void
hw_lag_detach_port(int unit, opennsl_trunk_t lag_id, opennsl_port_t hw_port)
{
    opennsl_trunk_member_t member_element;
    opennsl_error_t rc = OPENNSL_E_NONE;

    SW_LAG_DBG("Trunk Detach: lagid=%d, unit=%d, hw_port=%d",
               lag_id, unit, hw_port);

    opennsl_trunk_member_t_init(&member_element);
    OPENNSL_GPORT_MODPORT_SET(member_element.gport, MODID_0, hw_port);

    rc = opennsl_trunk_member_delete(unit, lag_id, &member_element);

    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to delete hw_port %d from tid %d, "
                 "rc=%d (%s)", hw_port, lag_id, rc, opennsl_errmsg(rc));
        log_event("FAILED_TO_DELETE_PORT",
            EV_KV("hw_port", "%d", hw_port),
            EV_KV("tid", "%d", lag_id),
            EV_KV("rc", "%d", rc),
            EV_KV("error", "%s", opennsl_errmsg(rc)));
    }

    SW_LAG_DBG("Done.");

} // hw_lag_detach_port

static void
hw_trunk_hash_setup(int unit, int hash_mode)
{
    int field_list;
    opennsl_error_t rc = OPENNSL_E_NONE;

    SW_LAG_DBG("entry: unit=%d, hash_mode=%d", unit, hash_mode);

    rc = opennsl_switch_control_set(unit, opennslSwitchHashSeed1, 0x22222222);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashSeed1: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return;
    }

    rc = opennsl_switch_control_set(unit, opennslSwitchHashField1Config,
                                        OPENNSL_HASH_FIELD_CONFIG_CRC32HI);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashField1Config: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return;
    }

    field_list = OPENNSL_HASH_FIELD_DSTL4 | OPENNSL_HASH_FIELD_SRCL4;

    rc = opennsl_switch_control_set(unit, opennslSwitchHashIP4TcpUdpField1,
                                        field_list);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashIP4TcpUdpField1: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return;
    }

    SW_LAG_DBG("done: rc=%s", opennsl_errmsg(rc));

} //hw_trunk_hash_setup

static
void
hw_set_lag_balance_mode(int unit, opennsl_trunk_t lag_id, int lag_mode)
{
    opennsl_error_t rc = OPENNSL_E_NONE;

    SW_LAG_DBG("entry: unit=%d, lag_id=%d lag_mode=%d",
               unit, lag_id, lag_mode);

    // Verify lag_mode is one of the supported Broadcom's PSC mode.
    switch (lag_mode) {
    case OPENNSL_TRUNK_PSC_SRCDSTMAC:
        break;
    case OPENNSL_TRUNK_PSC_SRCDSTIP:
        break;
    case OPENNSL_TRUNK_PSC_PORTFLOW:
        break;
    default:
        VLOG_ERR("Invalid LAG mode value %d", lag_mode);
        return;
    }

    rc = opennsl_trunk_psc_set(unit, lag_id, lag_mode);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("trunk psc set failed. unit %d, lag_id %d, "
                 "psc=%d, rc=%d (%s)", unit, lag_id,
                 lag_mode, rc, opennsl_errmsg(rc));
        log_event("TRUNK_PSC_SET_FAILED",
            EV_KV("unit", "%d", unit),
            EV_KV("lag_id", "%d", lag_id),
            EV_KV("psc", "%d", lag_mode),
            EV_KV("rc", "%d", rc),
            EV_KV("error", "%s", opennsl_errmsg(rc)));
    }

    SW_LAG_DBG("done: rc=%s", opennsl_errmsg(rc));

} // hw_set_lag_balance_mode

////////////////////////////// INTERNAL API ///////////////////////////////

static uint32_t ops_lag_port_calc_hash(int hw_unit, opennsl_trunk_t lag_id)
{
    return (hash_2words(hw_unit, lag_id));
}

static ops_lag_data_t *
find_lag_data(opennsl_trunk_t lag_id)
{
    ops_lag_data_t *lagp = NULL;

    if (!lags_data_initialized) {
        TAILQ_INIT(&ops_lags);
        lags_data_initialized = 1;
    }

    for (lagp = ops_lags.tqh_first;
         lagp != NULL;
         lagp = lagp->lag_data_list.tqe_next ) {
        if (lagp->lag_id == lag_id) {
            break;
        }
    }

    return lagp;

} // find_lag_data

static ops_lag_data_t *
get_lag_data(opennsl_trunk_t lag_id)
{
    int unit;
    ops_lag_data_t *lagp = NULL;

    lagp = find_lag_data(lag_id);
    if (lagp == NULL) {
        // LAG data hasn't been created yet.
        lagp = malloc(sizeof(ops_lag_data_t));
        if (!lagp) {
            VLOG_ERR("Failed to allocate memory for LAG id=%d", lag_id);
            return NULL;
        }

        // lag_id is allocated by bcmsdk later.
        lagp->lag_id = -1;
        lagp->hw_created = 0;

        for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {
            OPENNSL_PBMP_CLEAR(lagp->ports_pbm[unit]);
            OPENNSL_PBMP_CLEAR(lagp->egr_en_pbm[unit]);
        }
        TAILQ_INSERT_TAIL(&ops_lags, lagp, lag_data_list);
    }

    return lagp;

} // get_lag_data

static struct lag_port *
ops_lag_port_hmap_find (int hw_unit, opennsl_trunk_t lag_id)
{
    struct lag_port *lag_port = NULL;
    uint32_t hash = ops_lag_port_calc_hash(hw_unit, lag_id);

    VLOG_DBG("%s: hw_unit: %d, lag_id: %d", __FUNCTION__, hw_unit, lag_id);
    HMAP_FOR_EACH_WITH_HASH (lag_port, hmap_node, hash, &all_lag_ports) {
        if ((lag_port->lag_id == lag_id) && (lag_port->hw_unit == hw_unit)) {
            break;
        }
    }

    return (lag_port);
}

static void ops_lag_port_hmap_remove (int hw_unit, opennsl_trunk_t lag_id)
{
    struct lag_port *lag_port;

    VLOG_DBG("%s: hw_unit: %d, lag_id: %d", __FUNCTION__, hw_unit, lag_id);
    lag_port = ops_lag_port_hmap_find(hw_unit, lag_id);

    if (lag_port) {
        hmap_remove(&all_lag_ports, &lag_port->hmap_node);
        free(lag_port);
    } else {
        VLOG_DBG("%s: hw_unit: %d, lag_id: %d entry not found in hmap",
                 __FUNCTION__, hw_unit, lag_id);
    }
}

static void
ops_lag_port_hmap_insert (int hw_unit, opennsl_trunk_t lag_id, char *port_name)
{
    struct lag_port *lag_port;

    if (!port_name) {
        VLOG_ERR("%s cannot insert in hmap, port_name is NULL, hw_unit: %d, lag_id: %d",
                 __FUNCTION__, hw_unit, lag_id);
        return;
    }

    VLOG_DBG("%s: hw_unit: %d, lag_id: %d, port_name:%s", __FUNCTION__, hw_unit, lag_id, port_name);
    lag_port = xmalloc(sizeof(struct lag_port));
    lag_port->hw_unit = hw_unit;
    lag_port->lag_id = lag_id;
    strncpy(lag_port->port_name, port_name, PORT_NAME_SIZE);
    hmap_insert(&all_lag_ports, &lag_port->hmap_node, ops_lag_port_calc_hash(hw_unit, lag_id));
}

//////////////////////////////// Public API //////////////////////////////

void
bcmsdk_create_lag(opennsl_trunk_t *lag_idp, char *port_name)
{
    int unit = 0;
    ops_lag_data_t *lagp;

    if (!lag_idp) {
        VLOG_ERR("%s: lag_idp is NULL", __FUNCTION__);
        return;
    }

    SW_LAG_DBG("entry: lag_id=%d", *lag_idp);

    lagp = get_lag_data(*lag_idp);
    if (!lagp) {
        VLOG_ERR("Failed to get LAG data for LAGID %d", *lag_idp);
        return;
    }

    if (lagp->hw_created) {
        VLOG_WARN("Duplicated LAG creation request, LAGID=%d",
                  *lag_idp);
        return;
    }

    hw_create_lag(unit, lag_idp);
    lagp->lag_id = *lag_idp;
    lagp->hw_created = 1;

    if (port_name) {
        ops_lag_port_hmap_insert(unit, *lag_idp, port_name);
    }
    SW_LAG_DBG("done");

} // bcmsdk_create_lag

void
bcmsdk_destroy_lag(opennsl_trunk_t lag_id)
{
    int unit = 0;
    ops_lag_data_t *lagp;

    SW_LAG_DBG("entry: lag_id=%d", lag_id);

    lagp = find_lag_data(lag_id);
    if (lagp) {
        hw_destroy_lag(unit, lagp->lag_id);

        TAILQ_REMOVE(&ops_lags, lagp, lag_data_list);
        free(lagp);
    } else {
        VLOG_WARN("Deleting non-existing LAG, LAG_ID=%d", lag_id);
    }

    ops_lag_port_hmap_remove(unit, lag_id);
    SW_LAG_DBG("done");

} // bcmsdk_destroy_lag


void
bcmsdk_attach_ports_to_lag(opennsl_trunk_t lag_id, opennsl_pbmp_t *pbm)
{
    int unit = 0;
    ops_lag_data_t *lagp;
    opennsl_port_t hw_port;
    opennsl_pbmp_t bcm_pbm;

    SW_LAG_DBG("entry: lag_id=%d", lag_id);

    lagp = get_lag_data(lag_id);
    if (!lagp) {
        VLOG_ERR("Failed to get LAG data for LAGID %d", lag_id);
        return;
    }

    if (!lagp->hw_created) {
        VLOG_WARN("Error LAGID=%d not created in hardware", lag_id);
        return;
    }

    /* Detach removed member ports. */
    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {

        bcm_pbm = pbm[unit];

        OPENNSL_PBMP_ITER(lagp->ports_pbm[unit], hw_port) {
            if (!OPENNSL_PBMP_MEMBER(bcm_pbm, hw_port)) {
                hw_lag_detach_port(unit, lag_id, hw_port);
                OPENNSL_PBMP_PORT_REMOVE(lagp->ports_pbm[unit], hw_port);
            }
        }
    }

    /* Attach current list of member ports. */
    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {

        bcm_pbm = pbm[unit];

        OPENNSL_PBMP_ITER(bcm_pbm, hw_port) {
            if (!OPENNSL_PBMP_MEMBER(lagp->ports_pbm[unit], hw_port)) {
                hw_lag_attach_port(unit, lag_id, hw_port);
                OPENNSL_PBMP_PORT_ADD(lagp->ports_pbm[unit], hw_port);
            }
        }
    }

    SW_LAG_DBG("done");

} // bcmsdk_attach_ports_to_lag

void
bcmsdk_egress_enable_lag_ports(opennsl_trunk_t lag_id, opennsl_pbmp_t *pbm)
{
    int unit = 0;
    ops_lag_data_t *lagp;
    opennsl_port_t hw_port;
    opennsl_pbmp_t bcm_pbm;

    SW_LAG_DBG("entry: lag_id=%d", lag_id);

    lagp = get_lag_data(lag_id);
    if (!lagp) {
        VLOG_ERR("Failed to get LAG data for LAGID %d", lag_id);
        return;
    }

    if (!lagp->hw_created) {
        VLOG_WARN("Error LAGID=%d not created in hardware", lag_id);
        return;
    }

    /* Disable egress for removed member ports. */
    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {

        bcm_pbm = pbm[unit];

        OPENNSL_PBMP_ITER(lagp->egr_en_pbm[unit], hw_port) {
            if (!OPENNSL_PBMP_MEMBER(bcm_pbm, hw_port)) {
                hw_lag_egress_enable_port(unit, lag_id, hw_port, 0);
                OPENNSL_PBMP_PORT_REMOVE(lagp->egr_en_pbm[unit], hw_port);
            }
        }
    }

    /* Enable egress for the current list of member ports. */
    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {

        bcm_pbm = pbm[unit];

        OPENNSL_PBMP_ITER(bcm_pbm, hw_port) {
            if (!OPENNSL_PBMP_MEMBER(lagp->egr_en_pbm[unit], hw_port)) {
                hw_lag_egress_enable_port(unit, lag_id, hw_port, 1);
                OPENNSL_PBMP_PORT_ADD(lagp->egr_en_pbm[unit], hw_port);
            }
        }
    }

    SW_LAG_DBG("done");

} // bcmsdk_egress_enable_lag_ports

void
bcmsdk_trunk_hash_setup(int hash_mode)
{
    int unit = 0;

    SW_LAG_DBG("entry: hash_mode=%d", hash_mode);

    hw_trunk_hash_setup(unit, hash_mode);

    SW_LAG_DBG("done");

} // bcmsdk_trunk_hash_setup

void
bcmsdk_set_lag_balance_mode(opennsl_trunk_t lag_id, int lag_mode)
{
    int unit = 0;
    ops_lag_data_t *lagp;

    SW_LAG_DBG("entry: lag_id=%d", lag_id);

    lagp = find_lag_data(lag_id);
    if (!lagp) {
        VLOG_ERR("Failed to get LAG data for LAGID %d", lag_id);
        return;
    }

    if (!lagp->hw_created) {
        VLOG_WARN("Attempted to set LAG mode in hardware for "
                  "non-existing LAGID=%d", lag_id);
        return;
    }

    if (lagp->lag_mode != lag_mode) {
        hw_set_lag_balance_mode(unit, lag_id, lag_mode);
        lagp->lag_mode = lag_mode;
    }

    SW_LAG_DBG("done");

} // bcmsdk_set_lag_balance_mode

void ops_lag_get_port_name(opennsl_trunk_t lag_id, int hw_unit, char *port_name)
{
    struct lag_port *lag_port;

    if (!port_name || lag_id < 0) {
        return;
    }

    lag_port = ops_lag_port_hmap_find(hw_unit, lag_id);

    if (lag_port) {
        VLOG_DBG("%s: hw_unit: %d, lag_id: %d, port_name:%s", __FUNCTION__,
                 hw_unit, lag_id, lag_port->port_name);
        strncpy(port_name, lag_port->port_name, PORT_NAME_SIZE);
    } else {
        VLOG_ERR("%s: hw_unit: %d, lag_id: %d, port_name not found",
                 __FUNCTION__, hw_unit, lag_id);
    }
}
