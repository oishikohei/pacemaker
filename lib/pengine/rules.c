/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <glib.h>

#include <crm/crm.h>
#include <crm/common/xml.h>
#include <crm/pengine/rules.h>

#include <crm/common/iso8601_internal.h>
#include <crm/common/nvpair_internal.h>
#include <crm/common/rules_internal.h>
#include <crm/common/xml_internal.h>
#include <crm/pengine/internal.h>
#include <crm/pengine/rules_internal.h>

#include <sys/types.h>
#include <regex.h>

CRM_TRACE_INIT_DATA(pe_rules);

/*!
 * \internal
 * \brief Map pe_rule_eval_data_t to pcmk_rule_input_t
 *
 * \param[out] new  New data struct
 * \param[in]  old  Old data struct
 */
static void
map_rule_input(pcmk_rule_input_t *new, const pe_rule_eval_data_t *old)
{
    if (old == NULL) {
        return;
    }
    new->now = old->now;
    new->node_attrs = old->node_hash;
    if (old->rsc_data != NULL) {
        new->rsc_standard = old->rsc_data->standard;
        new->rsc_provider = old->rsc_data->provider;
        new->rsc_agent = old->rsc_data->agent;
    }
    if (old->match_data != NULL) {
        new->rsc_params = old->match_data->params;
        new->rsc_meta = old->match_data->meta;
        if (old->match_data->re != NULL) {
            new->rsc_id = old->match_data->re->string;
            new->rsc_id_submatches = old->match_data->re->pmatch;
            new->rsc_id_nmatches = old->match_data->re->nregs;
        }
    }
    if (old->op_data != NULL) {
        new->op_name = old->op_data->op_name;
        new->op_interval_ms = old->op_data->interval;
    }
}

/*!
 * \brief Extract nvpair blocks contained by an XML element into a hash table
 *
 * \param[in,out] top           Ignored
 * \param[in]     xml_obj       XML element containing blocks of nvpair elements
 * \param[in]     set_name      If not NULL, only use blocks of this element
 * \param[in]     rule_data     Matching parameters to use when unpacking
 * \param[out]    hash          Where to store extracted name/value pairs
 * \param[in]     always_first  If not NULL, process block with this ID first
 * \param[in]     overwrite     Whether to replace existing values with same
 *                              name (all internal callers pass \c FALSE)
 * \param[out]    next_change   If not NULL, set to when evaluation will change
 */
void
pe_eval_nvpairs(xmlNode *top, const xmlNode *xml_obj, const char *set_name,
                const pe_rule_eval_data_t *rule_data, GHashTable *hash,
                const char *always_first, gboolean overwrite,
                crm_time_t *next_change)
{
    GList *pairs = pcmk__xe_dereference_children(xml_obj, set_name);

    if (pairs) {
        pcmk__nvpair_unpack_t data = {
            .values = hash,
            .first_id = always_first,
            .overwrite = overwrite,
            .next_change = next_change,
        };

        map_rule_input(&(data.rule_input), rule_data);

        pairs = g_list_sort_with_data(pairs, pcmk__cmp_nvpair_blocks, &data);
        g_list_foreach(pairs, pcmk__unpack_nvpair_block, &data);
        g_list_free(pairs);
    }
}

/*!
 * \brief Extract nvpair blocks contained by an XML element into a hash table
 *
 * \param[in,out] top           Ignored
 * \param[in]     xml_obj       XML element containing blocks of nvpair elements
 * \param[in]     set_name      Element name to identify nvpair blocks
 * \param[in]     node_hash     Node attributes to use when evaluating rules
 * \param[out]    hash          Where to store extracted name/value pairs
 * \param[in]     always_first  If not NULL, process block with this ID first
 * \param[in]     overwrite     Whether to replace existing values with same
 *                              name (all internal callers pass \c FALSE)
 * \param[in]     now           Time to use when evaluating rules
 * \param[out]    next_change   If not NULL, set to when evaluation will change
 */
void
pe_unpack_nvpairs(xmlNode *top, const xmlNode *xml_obj, const char *set_name,
                  GHashTable *node_hash, GHashTable *hash,
                  const char *always_first, gboolean overwrite,
                  crm_time_t *now, crm_time_t *next_change)
{
    pe_rule_eval_data_t rule_data = {
        .node_hash = node_hash,
        .now = now,
        .match_data = NULL,
        .rsc_data = NULL,
        .op_data = NULL
    };

    pe_eval_nvpairs(NULL, xml_obj, set_name, &rule_data, hash,
                    always_first, overwrite, next_change);
}

// Deprecated functions kept only for backward API compatibility
// LCOV_EXCL_START

#include <crm/pengine/rules_compat.h>

gboolean
test_rule(xmlNode * rule, GHashTable * node_hash, enum rsc_role_e role, crm_time_t * now)
{
    pcmk_rule_input_t rule_input = {
        .node_attrs = node_hash,
        .now = now,
    };

    return pcmk_evaluate_rule(rule, &rule_input, NULL) == pcmk_rc_ok;
}

// LCOV_EXCL_STOP
// End deprecated API
