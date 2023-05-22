/*
 * Copyright 2004-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdbool.h>
#include <stdint.h>                 // uint8_t, uint32_t

#include <crm/msg_xml.h>
#include <pacemaker-internal.h>

#include "libpacemaker_private.h"

static void stop_resource(pcmk_resource_t *rsc, pcmk_node_t *node,
                          bool optional);
static void start_resource(pcmk_resource_t *rsc, pcmk_node_t *node,
                           bool optional);
static void demote_resource(pcmk_resource_t *rsc, pcmk_node_t *node,
                            bool optional);
static void promote_resource(pcmk_resource_t *rsc, pcmk_node_t *node,
                             bool optional);
static void assert_role_error(pcmk_resource_t *rsc, pcmk_node_t *node,
                              bool optional);

#define RSC_ROLE_MAX    (pcmk_role_promoted + 1)

static enum rsc_role_e rsc_state_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX] = {
    /* This array lists the immediate next role when transitioning from one role
     * to a target role. For example, when going from Stopped to Promoted, the
     * next role is Unpromoted, because the resource must be started before it
     * can be promoted. The current state then becomes Started, which is fed
     * into this array again, giving a next role of Promoted.
     *
     * Current role       Immediate next role   Final target role
     * ------------       -------------------   -----------------
     */
    /* Unknown */       { pcmk_role_unknown,    /* Unknown */
                          pcmk_role_stopped,    /* Stopped */
                          pcmk_role_stopped,    /* Started */
                          pcmk_role_stopped,    /* Unpromoted */
                          pcmk_role_stopped,    /* Promoted */
                        },
    /* Stopped */       { pcmk_role_stopped,    /* Unknown */
                          pcmk_role_stopped,    /* Stopped */
                          pcmk_role_started,    /* Started */
                          pcmk_role_unpromoted, /* Unpromoted */
                          pcmk_role_unpromoted, /* Promoted */
                        },
    /* Started */       { pcmk_role_stopped,    /* Unknown */
                          pcmk_role_stopped,    /* Stopped */
                          pcmk_role_started,    /* Started */
                          pcmk_role_unpromoted, /* Unpromoted */
                          pcmk_role_promoted,   /* Promoted */
                        },
    /* Unpromoted */    { pcmk_role_stopped,    /* Unknown */
                          pcmk_role_stopped,    /* Stopped */
                          pcmk_role_stopped,    /* Started */
                          pcmk_role_unpromoted, /* Unpromoted */
                          pcmk_role_promoted,   /* Promoted */
                        },
    /* Promoted  */     { pcmk_role_stopped,    /* Unknown */
                          pcmk_role_unpromoted, /* Stopped */
                          pcmk_role_unpromoted, /* Started */
                          pcmk_role_unpromoted, /* Unpromoted */
                          pcmk_role_promoted,   /* Promoted */
                        },
};

/*!
 * \internal
 * \brief Function to schedule actions needed for a role change
 *
 * \param[in,out] rsc       Resource whose role is changing
 * \param[in,out] node      Node where resource will be in its next role
 * \param[in]     optional  Whether scheduled actions should be optional
 */
typedef void (*rsc_transition_fn)(pcmk_resource_t *rsc, pcmk_node_t *node,
                                  bool optional);

static rsc_transition_fn rsc_action_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX] = {
    /* This array lists the function needed to transition directly from one role
     * to another. NULL indicates that nothing is needed.
     *
     * Current role         Transition function             Next role
     * ------------         -------------------             ----------
     */
    /* Unknown */       {   assert_role_error,              /* Unknown */
                            stop_resource,                  /* Stopped */
                            assert_role_error,              /* Started */
                            assert_role_error,              /* Unpromoted */
                            assert_role_error,              /* Promoted */
                        },
    /* Stopped */       {   assert_role_error,              /* Unknown */
                            NULL,                           /* Stopped */
                            start_resource,                 /* Started */
                            start_resource,                 /* Unpromoted */
                            assert_role_error,              /* Promoted */
                        },
    /* Started */       {   assert_role_error,              /* Unknown */
                            stop_resource,                  /* Stopped */
                            NULL,                           /* Started */
                            NULL,                           /* Unpromoted */
                            promote_resource,               /* Promoted */
                        },
    /* Unpromoted */    {   assert_role_error,              /* Unknown */
                            stop_resource,                  /* Stopped */
                            stop_resource,                  /* Started */
                            NULL,                           /* Unpromoted */
                            promote_resource,               /* Promoted */
                        },
    /* Promoted  */     {   assert_role_error,              /* Unknown */
                            demote_resource,                /* Stopped */
                            demote_resource,                /* Started */
                            demote_resource,                /* Unpromoted */
                            NULL,                           /* Promoted */
                        },
};

/*!
 * \internal
 * \brief Get a list of a resource's allowed nodes sorted by node score
 *
 * \param[in] rsc  Resource to check
 *
 * \return List of allowed nodes sorted by node score
 */
static GList *
sorted_allowed_nodes(const pcmk_resource_t *rsc)
{
    if (rsc->allowed_nodes != NULL) {
        GList *nodes = g_hash_table_get_values(rsc->allowed_nodes);

        if (nodes != NULL) {
            return pcmk__sort_nodes(nodes, pe__current_node(rsc));
        }
    }
    return NULL;
}

/*!
 * \internal
 * \brief Assign a resource to its best allowed node, if possible
 *
 * \param[in,out] rsc           Resource to choose a node for
 * \param[in]     prefer        If not \c NULL, prefer this node when all else
 *                              equal
 * \param[in]     stop_if_fail  If \c true and \p rsc can't be assigned to a
 *                              node, set next role to stopped and update
 *                              existing actions
 *
 * \return true if \p rsc could be assigned to a node, otherwise false
 *
 * \note If \p stop_if_fail is \c false, then \c pcmk__unassign_resource() can
 *       completely undo the assignment. A successful assignment can be either
 *       undone or left alone as final. A failed assignment has the same effect
 *       as calling pcmk__unassign_resource(); there are no side effects on
 *       roles or actions.
 */
static bool
assign_best_node(pcmk_resource_t *rsc, const pcmk_node_t *prefer,
                 bool stop_if_fail)
{
    GList *nodes = NULL;
    pcmk_node_t *chosen = NULL;
    pcmk_node_t *best = NULL;
    const pcmk_node_t *most_free_node = pcmk__ban_insufficient_capacity(rsc);

    if (prefer == NULL) {
        prefer = most_free_node;
    }

    if (!pcmk_is_set(rsc->flags, pcmk_rsc_unassigned)) {
        // We've already finished assignment of resources to nodes
        return rsc->allocated_to != NULL;
    }

    // Sort allowed nodes by score
    nodes = sorted_allowed_nodes(rsc);
    if (nodes != NULL) {
        best = (pcmk_node_t *) nodes->data; // First node has best score
    }

    if ((prefer != NULL) && (nodes != NULL)) {
        // Get the allowed node version of prefer
        chosen = g_hash_table_lookup(rsc->allowed_nodes, prefer->details->id);

        if (chosen == NULL) {
            pcmk__rsc_trace(rsc, "Preferred node %s for %s was unknown",
                            pe__node_name(prefer), rsc->id);

        /* Favor the preferred node as long as its score is at least as good as
         * the best allowed node's.
         *
         * An alternative would be to favor the preferred node even if the best
         * node is better, when the best node's score is less than INFINITY.
         */
        } else if (chosen->weight < best->weight) {
            pcmk__rsc_trace(rsc, "Preferred node %s for %s was unsuitable",
                            pe__node_name(chosen), rsc->id);
            chosen = NULL;

        } else if (!pcmk__node_available(chosen, true, false)) {
            pcmk__rsc_trace(rsc, "Preferred node %s for %s was unavailable",
                            pe__node_name(chosen), rsc->id);
            chosen = NULL;

        } else {
            pcmk__rsc_trace(rsc,
                            "Chose preferred node %s for %s "
                            "(ignoring %d candidates)",
                            pe__node_name(chosen), rsc->id,
                            g_list_length(nodes));
        }
    }

    if ((chosen == NULL) && (best != NULL)) {
        /* Either there is no preferred node, or the preferred node is not
         * suitable, but another node is allowed to run the resource.
         */

        chosen = best;

        if (!pe_rsc_is_unique_clone(rsc->parent)
            && (chosen->weight > 0) // Zero not acceptable
            && pcmk__node_available(chosen, false, false)) {
            /* If the resource is already running on a node, prefer that node if
             * it is just as good as the chosen node.
             *
             * We don't do this for unique clone instances, because
             * pcmk__assign_instances() has already assigned instances to their
             * running nodes when appropriate, and if we get here, we don't want
             * remaining unassigned instances to prefer a node that's already
             * running another instance.
             */
            pcmk_node_t *running = pe__current_node(rsc);

            if (running == NULL) {
                // Nothing to do

            } else if (!pcmk__node_available(running, true, false)) {
                pcmk__rsc_trace(rsc,
                                "Current node for %s (%s) can't run resources",
                                rsc->id, pe__node_name(running));

            } else {
                int nodes_with_best_score = 1;

                for (GList *iter = nodes->next; iter; iter = iter->next) {
                    pcmk_node_t *allowed = (pcmk_node_t *) iter->data;

                    if (allowed->weight != chosen->weight) {
                        // The nodes are sorted by score, so no more are equal
                        break;
                    }
                    if (pe__same_node(allowed, running)) {
                        // Scores are equal, so prefer the current node
                        chosen = allowed;
                    }
                    nodes_with_best_score++;
                }

                if (nodes_with_best_score > 1) {
                    uint8_t log_level = LOG_INFO;

                    if (chosen->weight >= INFINITY) {
                        log_level = LOG_WARNING;
                    }
                    do_crm_log(log_level,
                               "Chose %s for %s from %d nodes with score %s",
                               pe__node_name(chosen), rsc->id,
                               nodes_with_best_score,
                               pcmk_readable_score(chosen->weight));
                }
            }
        }

        pcmk__rsc_trace(rsc, "Chose %s for %s from %d candidates",
                        pe__node_name(chosen), rsc->id, g_list_length(nodes));
    }

    pcmk__assign_resource(rsc, chosen, false, stop_if_fail);
    g_list_free(nodes);
    return rsc->allocated_to != NULL;
}

/*!
 * \internal
 * \brief Apply a "this with" colocation to a node's allowed node scores
 *
 * \param[in,out] colocation  Colocation to apply
 * \param[in,out] rsc         Resource being assigned
 */
static void
apply_this_with(pcmk__colocation_t *colocation, pcmk_resource_t *rsc)
{
    GHashTable *archive = NULL;
    pcmk_resource_t *other = colocation->primary;

    // In certain cases, we will need to revert the node scores
    if ((colocation->dependent_role >= pcmk_role_promoted)
        || ((colocation->score < 0) && (colocation->score > -INFINITY))) {
        archive = pcmk__copy_node_table(rsc->allowed_nodes);
    }

    if (pcmk_is_set(other->flags, pcmk_rsc_unassigned)) {
        pcmk__rsc_trace(rsc,
                        "%s: Assigning colocation %s primary %s first"
                        "(score=%d role=%s)",
                        rsc->id, colocation->id, other->id,
                        colocation->score, role2text(colocation->dependent_role));
        other->cmds->assign(other, NULL, true);
    }

    // Apply the colocation score to this resource's allowed node scores
    rsc->cmds->apply_coloc_score(rsc, other, colocation, true);
    if ((archive != NULL)
        && !pcmk__any_node_available(rsc->allowed_nodes)) {
        pcmk__rsc_info(rsc,
                       "%s: Reverting scores from colocation with %s "
                       "because no nodes allowed",
                       rsc->id, other->id);
        g_hash_table_destroy(rsc->allowed_nodes);
        rsc->allowed_nodes = archive;
        archive = NULL;
    }
    if (archive != NULL) {
        g_hash_table_destroy(archive);
    }
}

/*!
 * \internal
 * \brief Update a Pacemaker Remote node once its connection has been assigned
 *
 * \param[in] connection  Connection resource that has been assigned
 */
static void
remote_connection_assigned(const pcmk_resource_t *connection)
{
    pcmk_node_t *remote_node = pe_find_node(connection->cluster->nodes,
                                            connection->id);

    CRM_CHECK(remote_node != NULL, return);

    if ((connection->allocated_to != NULL)
        && (connection->next_role != pcmk_role_stopped)) {

        crm_trace("Pacemaker Remote node %s will be online",
                  remote_node->details->id);
        remote_node->details->online = TRUE;
        if (remote_node->details->unseen) {
            // Avoid unnecessary fence, since we will attempt connection
            remote_node->details->unclean = FALSE;
        }

    } else {
        crm_trace("Pacemaker Remote node %s will be shut down "
                  "(%sassigned connection's next role is %s)",
                  remote_node->details->id,
                  ((connection->allocated_to == NULL)? "un" : ""),
                  role2text(connection->next_role));
        remote_node->details->shutdown = TRUE;
    }
}

/*!
 * \internal
 * \brief Assign a primitive resource to a node
 *
 * \param[in,out] rsc           Resource to assign to a node
 * \param[in]     prefer        Node to prefer, if all else is equal
 * \param[in]     stop_if_fail  If \c true and \p rsc can't be assigned to a
 *                              node, set next role to stopped and update
 *                              existing actions
 *
 * \return Node that \p rsc is assigned to, if assigned entirely to one node
 *
 * \note If \p stop_if_fail is \c false, then \c pcmk__unassign_resource() can
 *       completely undo the assignment. A successful assignment can be either
 *       undone or left alone as final. A failed assignment has the same effect
 *       as calling pcmk__unassign_resource(); there are no side effects on
 *       roles or actions.
 */
pcmk_node_t *
pcmk__primitive_assign(pcmk_resource_t *rsc, const pcmk_node_t *prefer,
                       bool stop_if_fail)
{
    GList *this_with_colocations = NULL;
    GList *with_this_colocations = NULL;
    GList *iter = NULL;
    pcmk__colocation_t *colocation = NULL;

    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive));

    // Never assign a child without parent being assigned first
    if ((rsc->parent != NULL)
        && !pcmk_is_set(rsc->parent->flags, pcmk_rsc_assigning)) {
        pcmk__rsc_debug(rsc, "%s: Assigning parent %s first",
                        rsc->id, rsc->parent->id);
        rsc->parent->cmds->assign(rsc->parent, prefer, stop_if_fail);
    }

    if (!pcmk_is_set(rsc->flags, pcmk_rsc_unassigned)) {
        // Assignment has already been done
        const char *node_name = "no node";

        if (rsc->allocated_to != NULL) {
            node_name = pe__node_name(rsc->allocated_to);
        }
        pcmk__rsc_debug(rsc, "%s: pre-assigned to %s", rsc->id, node_name);
        return rsc->allocated_to;
    }

    // Ensure we detect assignment loops
    if (pcmk_is_set(rsc->flags, pcmk_rsc_assigning)) {
        pcmk__rsc_debug(rsc, "Breaking assignment loop involving %s", rsc->id);
        return NULL;
    }
    pe__set_resource_flags(rsc, pcmk_rsc_assigning);

    pe__show_node_scores(true, rsc, "Pre-assignment", rsc->allowed_nodes,
                         rsc->cluster);

    this_with_colocations = pcmk__this_with_colocations(rsc);
    with_this_colocations = pcmk__with_this_colocations(rsc);

    // Apply mandatory colocations first, to satisfy as many as possible
    for (iter = this_with_colocations; iter != NULL; iter = iter->next) {
        colocation = iter->data;

        if ((colocation->score <= -CRM_SCORE_INFINITY)
            || (colocation->score >= CRM_SCORE_INFINITY)) {
            apply_this_with(colocation, rsc);
        }
    }
    for (iter = with_this_colocations; iter != NULL; iter = iter->next) {
        colocation = iter->data;

        if ((colocation->score <= -CRM_SCORE_INFINITY)
            || (colocation->score >= CRM_SCORE_INFINITY)) {
            pcmk__add_dependent_scores(colocation, rsc);
        }
    }

    pe__show_node_scores(true, rsc, "Mandatory-colocations",
                         rsc->allowed_nodes, rsc->cluster);

    // Then apply optional colocations
    for (iter = this_with_colocations; iter != NULL; iter = iter->next) {
        colocation = iter->data;

        if ((colocation->score > -CRM_SCORE_INFINITY)
            && (colocation->score < CRM_SCORE_INFINITY)) {
            apply_this_with(colocation, rsc);
        }
    }
    for (iter = with_this_colocations; iter != NULL; iter = iter->next) {
        colocation = iter->data;

        if ((colocation->score > -CRM_SCORE_INFINITY)
            && (colocation->score < CRM_SCORE_INFINITY)) {
            pcmk__add_dependent_scores(colocation, rsc);
        }
    }

    g_list_free(this_with_colocations);
    g_list_free(with_this_colocations);

    if (rsc->next_role == pcmk_role_stopped) {
        pcmk__rsc_trace(rsc,
                        "Banning %s from all nodes because it will be stopped",
                        rsc->id);
        resource_location(rsc, NULL, -INFINITY, XML_RSC_ATTR_TARGET_ROLE,
                          rsc->cluster);

    } else if ((rsc->next_role > rsc->role)
               && !pcmk_is_set(rsc->cluster->flags, pcmk_sched_quorate)
               && (rsc->cluster->no_quorum_policy == pcmk_no_quorum_freeze)) {
        crm_notice("Resource %s cannot be elevated from %s to %s due to "
                   "no-quorum-policy=freeze",
                   rsc->id, role2text(rsc->role), role2text(rsc->next_role));
        pe__set_next_role(rsc, rsc->role, "no-quorum-policy=freeze");
    }

    pe__show_node_scores(!pcmk_is_set(rsc->cluster->flags,
                                      pcmk_sched_output_scores),
                         rsc, __func__, rsc->allowed_nodes, rsc->cluster);

    // Unmanage resource if fencing is enabled but no device is configured
    if (pcmk_is_set(rsc->cluster->flags, pcmk_sched_fencing_enabled)
        && !pcmk_is_set(rsc->cluster->flags, pcmk_sched_have_fencing)) {
        pe__clear_resource_flags(rsc, pcmk_rsc_managed);
    }

    if (!pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
        // Unmanaged resources stay on their current node
        const char *reason = NULL;
        pcmk_node_t *assign_to = NULL;

        pe__set_next_role(rsc, rsc->role, "unmanaged");
        assign_to = pe__current_node(rsc);
        if (assign_to == NULL) {
            reason = "inactive";
        } else if (rsc->role == pcmk_role_promoted) {
            reason = "promoted";
        } else if (pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {
            reason = "failed";
        } else {
            reason = "active";
        }
        pcmk__rsc_info(rsc, "Unmanaged resource %s assigned to %s: %s", rsc->id,
                       (assign_to? assign_to->details->uname : "no node"),
                       reason);
        pcmk__assign_resource(rsc, assign_to, true, stop_if_fail);

    } else if (pcmk_is_set(rsc->cluster->flags, pcmk_sched_stop_all)) {
        // Must stop at some point, but be consistent with stop_if_fail
        if (stop_if_fail) {
            pcmk__rsc_debug(rsc, "Forcing %s to stop: stop-all-resources",
                            rsc->id);
        }
        pcmk__assign_resource(rsc, NULL, true, stop_if_fail);

    } else if (!assign_best_node(rsc, prefer, stop_if_fail)) {
        // Assignment failed
        if (!pcmk_is_set(rsc->flags, pcmk_rsc_removed)) {
            pcmk__rsc_info(rsc, "Resource %s cannot run anywhere", rsc->id);
        } else if ((rsc->running_on != NULL) && stop_if_fail) {
            pcmk__rsc_info(rsc, "Stopping orphan resource %s", rsc->id);
        }
    }

    pe__clear_resource_flags(rsc, pcmk_rsc_assigning);

    if (rsc->is_remote_node) {
        remote_connection_assigned(rsc);
    }

    return rsc->allocated_to;
}

/*!
 * \internal
 * \brief Schedule actions to bring resource down and back to current role
 *
 * \param[in,out] rsc           Resource to restart
 * \param[in,out] current       Node that resource should be brought down on
 * \param[in]     need_stop     Whether the resource must be stopped
 * \param[in]     need_promote  Whether the resource must be promoted
 *
 * \return Role that resource would have after scheduled actions are taken
 */
static void
schedule_restart_actions(pcmk_resource_t *rsc, pcmk_node_t *current,
                         bool need_stop, bool need_promote)
{
    enum rsc_role_e role = rsc->role;
    enum rsc_role_e next_role;
    rsc_transition_fn fn = NULL;

    pe__set_resource_flags(rsc, pcmk_rsc_restarting);

    // Bring resource down to a stop on its current node
    while (role != pcmk_role_stopped) {
        next_role = rsc_state_matrix[role][pcmk_role_stopped];
        pcmk__rsc_trace(rsc, "Creating %s action to take %s down from %s to %s",
                        (need_stop? "required" : "optional"), rsc->id,
                        role2text(role), role2text(next_role));
        fn = rsc_action_matrix[role][next_role];
        if (fn == NULL) {
            break;
        }
        fn(rsc, current, !need_stop);
        role = next_role;
    }

    // Bring resource up to its next role on its next node
    while ((rsc->role <= rsc->next_role) && (role != rsc->role)
           && !pcmk_is_set(rsc->flags, pcmk_rsc_blocked)) {
        bool required = need_stop;

        next_role = rsc_state_matrix[role][rsc->role];
        if ((next_role == pcmk_role_promoted) && need_promote) {
            required = true;
        }
        pcmk__rsc_trace(rsc, "Creating %s action to take %s up from %s to %s",
                        (required? "required" : "optional"), rsc->id,
                        role2text(role), role2text(next_role));
        fn = rsc_action_matrix[role][next_role];
        if (fn == NULL) {
            break;
        }
        fn(rsc, rsc->allocated_to, !required);
        role = next_role;
    }

    pe__clear_resource_flags(rsc, pcmk_rsc_restarting);
}

/*!
 * \internal
 * \brief If a resource's next role is not explicitly specified, set a default
 *
 * \param[in,out] rsc  Resource to set next role for
 *
 * \return "explicit" if next role was explicitly set, otherwise "implicit"
 */
static const char *
set_default_next_role(pcmk_resource_t *rsc)
{
    if (rsc->next_role != pcmk_role_unknown) {
        return "explicit";
    }

    if (rsc->allocated_to == NULL) {
        pe__set_next_role(rsc, pcmk_role_stopped, "assignment");
    } else {
        pe__set_next_role(rsc, pcmk_role_started, "assignment");
    }
    return "implicit";
}

/*!
 * \internal
 * \brief Create an action to represent an already pending start
 *
 * \param[in,out] rsc  Resource to create start action for
 */
static void
create_pending_start(pcmk_resource_t *rsc)
{
    pcmk_action_t *start = NULL;

    pcmk__rsc_trace(rsc,
                    "Creating action for %s to represent already pending start",
                    rsc->id);
    start = start_action(rsc, rsc->allocated_to, TRUE);
    pe__set_action_flags(start, pcmk_action_always_in_graph);
}

/*!
 * \internal
 * \brief Schedule actions needed to take a resource to its next role
 *
 * \param[in,out] rsc  Resource to schedule actions for
 */
static void
schedule_role_transition_actions(pcmk_resource_t *rsc)
{
    enum rsc_role_e role = rsc->role;

    while (role != rsc->next_role) {
        enum rsc_role_e next_role = rsc_state_matrix[role][rsc->next_role];
        rsc_transition_fn fn = NULL;

        pcmk__rsc_trace(rsc,
                        "Creating action to take %s from %s to %s "
                        "(ending at %s)",
                        rsc->id, role2text(role), role2text(next_role),
                        role2text(rsc->next_role));
        fn = rsc_action_matrix[role][next_role];
        if (fn == NULL) {
            break;
        }
        fn(rsc, rsc->allocated_to, false);
        role = next_role;
    }
}

/*!
 * \internal
 * \brief Create all actions needed for a given primitive resource
 *
 * \param[in,out] rsc  Primitive resource to create actions for
 */
void
pcmk__primitive_create_actions(pcmk_resource_t *rsc)
{
    bool need_stop = false;
    bool need_promote = false;
    bool is_moving = false;
    bool allow_migrate = false;
    bool multiply_active = false;

    pcmk_node_t *current = NULL;
    unsigned int num_all_active = 0;
    unsigned int num_clean_active = 0;
    const char *next_role_source = NULL;

    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive));

    next_role_source = set_default_next_role(rsc);
    pcmk__rsc_trace(rsc,
                    "Creating all actions for %s transition from %s to %s "
                    "(%s) on %s",
                    rsc->id, role2text(rsc->role), role2text(rsc->next_role),
                    next_role_source, pe__node_name(rsc->allocated_to));

    current = rsc->fns->active_node(rsc, &num_all_active, &num_clean_active);

    g_list_foreach(rsc->dangling_migrations, pcmk__abort_dangling_migration,
                   rsc);

    if ((current != NULL) && (rsc->allocated_to != NULL)
        && !pe__same_node(current, rsc->allocated_to)
        && (rsc->next_role >= pcmk_role_started)) {

        pcmk__rsc_trace(rsc, "Moving %s from %s to %s",
                        rsc->id, pe__node_name(current),
                        pe__node_name(rsc->allocated_to));
        is_moving = true;
        allow_migrate = pcmk__rsc_can_migrate(rsc, current);

        // This is needed even if migrating (though I'm not sure why ...)
        need_stop = true;
    }

    // Check whether resource is partially migrated and/or multiply active
    if ((rsc->partial_migration_source != NULL)
        && (rsc->partial_migration_target != NULL)
        && allow_migrate && (num_all_active == 2)
        && pe__same_node(current, rsc->partial_migration_source)
        && pe__same_node(rsc->allocated_to, rsc->partial_migration_target)) {
        /* A partial migration is in progress, and the migration target remains
         * the same as when the migration began.
         */
        pcmk__rsc_trace(rsc,
                        "Partial migration of %s from %s to %s will continue",
                        rsc->id, pe__node_name(rsc->partial_migration_source),
                        pe__node_name(rsc->partial_migration_target));

    } else if ((rsc->partial_migration_source != NULL)
               || (rsc->partial_migration_target != NULL)) {
        // A partial migration is in progress but can't be continued

        if (num_all_active > 2) {
            // The resource is migrating *and* multiply active!
            crm_notice("Forcing recovery of %s because it is migrating "
                       "from %s to %s and possibly active elsewhere",
                       rsc->id, pe__node_name(rsc->partial_migration_source),
                       pe__node_name(rsc->partial_migration_target));
        } else {
            // The migration source or target isn't available
            crm_notice("Forcing recovery of %s because it can no longer "
                       "migrate from %s to %s",
                       rsc->id, pe__node_name(rsc->partial_migration_source),
                       pe__node_name(rsc->partial_migration_target));
        }
        need_stop = true;
        rsc->partial_migration_source = rsc->partial_migration_target = NULL;
        allow_migrate = false;

    } else if (pcmk_is_set(rsc->flags, pcmk_rsc_needs_fencing)) {
        multiply_active = (num_all_active > 1);
    } else {
        /* If a resource has "requires" set to nothing or quorum, don't consider
         * it active on unclean nodes (similar to how all resources behave when
         * stonith-enabled is false). We can start such resources elsewhere
         * before fencing completes, and if we considered the resource active on
         * the failed node, we would attempt recovery for being active on
         * multiple nodes.
         */
        multiply_active = (num_clean_active > 1);
    }

    if (multiply_active) {
        const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

        // Resource was (possibly) incorrectly multiply active
        pcmk__sched_err("%s resource %s might be active on %u nodes (%s)",
                        pcmk__s(class, "Untyped"), rsc->id, num_all_active,
                        recovery2text(rsc->recovery_type));
        crm_notice("See https://wiki.clusterlabs.org/wiki/FAQ"
                   "#Resource_is_Too_Active for more information");

        switch (rsc->recovery_type) {
            case pcmk_multiply_active_restart:
                need_stop = true;
                break;
            case pcmk_multiply_active_unexpected:
                need_stop = true; // stop_resource() will skip expected node
                pe__set_resource_flags(rsc, pcmk_rsc_stop_unexpected);
                break;
            default:
                break;
        }

    } else {
        pe__clear_resource_flags(rsc, pcmk_rsc_stop_unexpected);
    }

    if (pcmk_is_set(rsc->flags, pcmk_rsc_start_pending)) {
        create_pending_start(rsc);
    }

    if (is_moving) {
        // Remaining tests are only for resources staying where they are

    } else if (pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {
        if (pcmk_is_set(rsc->flags, pcmk_rsc_stop_if_failed)) {
            need_stop = true;
            pcmk__rsc_trace(rsc, "Recovering %s", rsc->id);
        } else {
            pcmk__rsc_trace(rsc, "Recovering %s by demotion", rsc->id);
            if (rsc->next_role == pcmk_role_promoted) {
                need_promote = true;
            }
        }

    } else if (pcmk_is_set(rsc->flags, pcmk_rsc_blocked)) {
        pcmk__rsc_trace(rsc, "Blocking further actions on %s", rsc->id);
        need_stop = true;

    } else if ((rsc->role > pcmk_role_started) && (current != NULL)
               && (rsc->allocated_to != NULL)) {
        pcmk_action_t *start = NULL;

        pcmk__rsc_trace(rsc, "Creating start action for promoted resource %s",
                        rsc->id);
        start = start_action(rsc, rsc->allocated_to, TRUE);
        if (!pcmk_is_set(start->flags, pcmk_action_optional)) {
            // Recovery of a promoted resource
            pcmk__rsc_trace(rsc, "%s restart is required for recovery", rsc->id);
            need_stop = true;
        }
    }

    // Create any actions needed to bring resource down and back up to same role
    schedule_restart_actions(rsc, current, need_stop, need_promote);

    // Create any actions needed to take resource from this role to the next
    schedule_role_transition_actions(rsc);

    pcmk__create_recurring_actions(rsc);

    if (allow_migrate) {
        pcmk__create_migration_actions(rsc, current);
    }
}

/*!
 * \internal
 * \brief Ban a resource from any allowed nodes that are Pacemaker Remote nodes
 *
 * \param[in] rsc  Resource to check
 */
static void
rsc_avoids_remote_nodes(const pcmk_resource_t *rsc)
{
    GHashTableIter iter;
    pcmk_node_t *node = NULL;

    g_hash_table_iter_init(&iter, rsc->allowed_nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **) &node)) {
        if (node->details->remote_rsc != NULL) {
            node->weight = -INFINITY;
        }
    }
}

/*!
 * \internal
 * \brief Return allowed nodes as (possibly sorted) list
 *
 * Convert a resource's hash table of allowed nodes to a list. If printing to
 * stdout, sort the list, to keep action ID numbers consistent for regression
 * test output (while avoiding the performance hit on a live cluster).
 *
 * \param[in] rsc       Resource to check for allowed nodes
 *
 * \return List of resource's allowed nodes
 * \note Callers should take care not to rely on the list being sorted.
 */
static GList *
allowed_nodes_as_list(const pcmk_resource_t *rsc)
{
    GList *allowed_nodes = NULL;

    if (rsc->allowed_nodes) {
        allowed_nodes = g_hash_table_get_values(rsc->allowed_nodes);
    }

    if (!pcmk__is_daemon) {
        allowed_nodes = g_list_sort(allowed_nodes, pe__cmp_node_name);
    }

    return allowed_nodes;
}

/*!
 * \internal
 * \brief Create implicit constraints needed for a primitive resource
 *
 * \param[in,out] rsc  Primitive resource to create implicit constraints for
 */
void
pcmk__primitive_internal_constraints(pcmk_resource_t *rsc)
{
    GList *allowed_nodes = NULL;
    bool check_unfencing = false;
    bool check_utilization = false;

    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive));

    if (!pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
        pcmk__rsc_trace(rsc,
                        "Skipping implicit constraints for unmanaged resource "
                        "%s", rsc->id);
        return;
    }

    // Whether resource requires unfencing
    check_unfencing = !pcmk_is_set(rsc->flags, pcmk_rsc_fence_device)
                      && pcmk_is_set(rsc->cluster->flags,
                                     pcmk_sched_enable_unfencing)
                      && pcmk_is_set(rsc->flags, pcmk_rsc_needs_unfencing);

    // Whether a non-default placement strategy is used
    check_utilization = (g_hash_table_size(rsc->utilization) > 0)
                         && !pcmk__str_eq(rsc->cluster->placement_strategy,
                                          "default", pcmk__str_casei);

    // Order stops before starts (i.e. restart)
    pcmk__new_ordering(rsc, pcmk__op_key(rsc->id, PCMK_ACTION_STOP, 0), NULL,
                       rsc, pcmk__op_key(rsc->id, PCMK_ACTION_START, 0), NULL,
                       pcmk__ar_ordered
                       |pcmk__ar_first_implies_then
                       |pcmk__ar_intermediate_stop,
                       rsc->cluster);

    // Promotable ordering: demote before stop, start before promote
    if (pcmk_is_set(pe__const_top_resource(rsc, false)->flags,
                    pcmk_rsc_promotable)
        || (rsc->role > pcmk_role_unpromoted)) {

        pcmk__new_ordering(rsc, pcmk__op_key(rsc->id, PCMK_ACTION_DEMOTE, 0),
                           NULL,
                           rsc, pcmk__op_key(rsc->id, PCMK_ACTION_STOP, 0),
                           NULL,
                           pcmk__ar_promoted_then_implies_first, rsc->cluster);

        pcmk__new_ordering(rsc, pcmk__op_key(rsc->id, PCMK_ACTION_START, 0),
                           NULL,
                           rsc, pcmk__op_key(rsc->id, PCMK_ACTION_PROMOTE, 0),
                           NULL,
                           pcmk__ar_unrunnable_first_blocks, rsc->cluster);
    }

    // Don't clear resource history if probing on same node
    pcmk__new_ordering(rsc, pcmk__op_key(rsc->id, PCMK_ACTION_LRM_DELETE, 0),
                       NULL, rsc,
                       pcmk__op_key(rsc->id, PCMK_ACTION_MONITOR, 0),
                       NULL,
                       pcmk__ar_if_on_same_node|pcmk__ar_then_cancels_first,
                       rsc->cluster);

    // Certain checks need allowed nodes
    if (check_unfencing || check_utilization || (rsc->container != NULL)) {
        allowed_nodes = allowed_nodes_as_list(rsc);
    }

    if (check_unfencing) {
        g_list_foreach(allowed_nodes, pcmk__order_restart_vs_unfence, rsc);
    }

    if (check_utilization) {
        pcmk__create_utilization_constraints(rsc, allowed_nodes);
    }

    if (rsc->container != NULL) {
        pcmk_resource_t *remote_rsc = NULL;

        if (rsc->is_remote_node) {
            // rsc is the implicit remote connection for a guest or bundle node

            /* Guest resources are not allowed to run on Pacemaker Remote nodes,
             * to avoid nesting remotes. However, bundles are allowed.
             */
            if (!pcmk_is_set(rsc->flags, pcmk_rsc_remote_nesting_allowed)) {
                rsc_avoids_remote_nodes(rsc->container);
            }

            /* If someone cleans up a guest or bundle node's container, we will
             * likely schedule a (re-)probe of the container and recovery of the
             * connection. Order the connection stop after the container probe,
             * so that if we detect the container running, we will trigger a new
             * transition and avoid the unnecessary recovery.
             */
            pcmk__order_resource_actions(rsc->container, PCMK_ACTION_MONITOR,
                                         rsc, PCMK_ACTION_STOP,
                                         pcmk__ar_ordered);

        /* A user can specify that a resource must start on a Pacemaker Remote
         * node by explicitly configuring it with the container=NODENAME
         * meta-attribute. This is of questionable merit, since location
         * constraints can accomplish the same thing. But we support it, so here
         * we check whether a resource (that is not itself a remote connection)
         * has container set to a remote node or guest node resource.
         */
        } else if (rsc->container->is_remote_node) {
            remote_rsc = rsc->container;
        } else  {
            remote_rsc = pe__resource_contains_guest_node(rsc->cluster,
                                                          rsc->container);
        }

        if (remote_rsc != NULL) {
            /* Force the resource on the Pacemaker Remote node instead of
             * colocating the resource with the container resource.
             */
            for (GList *item = allowed_nodes; item; item = item->next) {
                pcmk_node_t *node = item->data;

                if (node->details->remote_rsc != remote_rsc) {
                    node->weight = -INFINITY;
                }
            }

        } else {
            /* This resource is either a filler for a container that does NOT
             * represent a Pacemaker Remote node, or a Pacemaker Remote
             * connection resource for a guest node or bundle.
             */
            int score;

            crm_trace("Order and colocate %s relative to its container %s",
                      rsc->id, rsc->container->id);

            pcmk__new_ordering(rsc->container,
                               pcmk__op_key(rsc->container->id,
                                            PCMK_ACTION_START, 0),
                               NULL, rsc,
                               pcmk__op_key(rsc->id, PCMK_ACTION_START, 0),
                               NULL,
                               pcmk__ar_first_implies_then
                               |pcmk__ar_unrunnable_first_blocks,
                               rsc->cluster);

            pcmk__new_ordering(rsc,
                               pcmk__op_key(rsc->id, PCMK_ACTION_STOP, 0),
                               NULL,
                               rsc->container,
                               pcmk__op_key(rsc->container->id,
                                            PCMK_ACTION_STOP, 0),
                               NULL, pcmk__ar_then_implies_first, rsc->cluster);

            if (pcmk_is_set(rsc->flags, pcmk_rsc_remote_nesting_allowed)) {
                score = 10000;    /* Highly preferred but not essential */
            } else {
                score = INFINITY; /* Force them to run on the same host */
            }
            pcmk__new_colocation("#resource-with-container", NULL, score, rsc,
                                 rsc->container, NULL, NULL,
                                 pcmk__coloc_influence);
        }
    }

    if (rsc->is_remote_node
        || pcmk_is_set(rsc->flags, pcmk_rsc_fence_device)) {
        /* Remote connections and fencing devices are not allowed to run on
         * Pacemaker Remote nodes
         */
        rsc_avoids_remote_nodes(rsc);
    }
    g_list_free(allowed_nodes);
}

/*!
 * \internal
 * \brief Apply a colocation's score to node scores or resource priority
 *
 * Given a colocation constraint, apply its score to the dependent's
 * allowed node scores (if we are still placing resources) or priority (if
 * we are choosing promotable clone instance roles).
 *
 * \param[in,out] dependent      Dependent resource in colocation
 * \param[in]     primary        Primary resource in colocation
 * \param[in]     colocation     Colocation constraint to apply
 * \param[in]     for_dependent  true if called on behalf of dependent
 */
void
pcmk__primitive_apply_coloc_score(pcmk_resource_t *dependent,
                                  const pcmk_resource_t *primary,
                                  const pcmk__colocation_t *colocation,
                                  bool for_dependent)
{
    enum pcmk__coloc_affects filter_results;

    CRM_ASSERT((dependent != NULL) && (primary != NULL)
               && (colocation != NULL));

    if (for_dependent) {
        // Always process on behalf of primary resource
        primary->cmds->apply_coloc_score(dependent, primary, colocation, false);
        return;
    }

    filter_results = pcmk__colocation_affects(dependent, primary, colocation,
                                              false);
    pcmk__rsc_trace(dependent, "%s %s with %s (%s, score=%d, filter=%d)",
                    ((colocation->score > 0)? "Colocating" : "Anti-colocating"),
                    dependent->id, primary->id, colocation->id,
                    colocation->score,
                    filter_results);

    switch (filter_results) {
        case pcmk__coloc_affects_role:
            pcmk__apply_coloc_to_priority(dependent, primary, colocation);
            break;
        case pcmk__coloc_affects_location:
            pcmk__apply_coloc_to_scores(dependent, primary, colocation);
            break;
        default: // pcmk__coloc_affects_nothing
            return;
    }
}

/* Primitive implementation of
 * pcmk_assignment_methods_t:with_this_colocations()
 */
void
pcmk__with_primitive_colocations(const pcmk_resource_t *rsc,
                                 const pcmk_resource_t *orig_rsc, GList **list)
{
    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive)
               && (list != NULL));

    if (rsc == orig_rsc) {
        /* For the resource itself, add all of its own colocations and relevant
         * colocations from its parent (if any).
         */
        pcmk__add_with_this_list(list, rsc->rsc_cons_lhs, orig_rsc);
        if (rsc->parent != NULL) {
            rsc->parent->cmds->with_this_colocations(rsc->parent, orig_rsc, list);
        }
    } else {
        // For an ancestor, add only explicitly configured constraints
        for (GList *iter = rsc->rsc_cons_lhs; iter != NULL; iter = iter->next) {
            pcmk__colocation_t *colocation = iter->data;

            if (pcmk_is_set(colocation->flags, pcmk__coloc_explicit)) {
                pcmk__add_with_this(list, colocation, orig_rsc);
            }
        }
    }
}

/* Primitive implementation of
 * pcmk_assignment_methods_t:this_with_colocations()
 */
void
pcmk__primitive_with_colocations(const pcmk_resource_t *rsc,
                                 const pcmk_resource_t *orig_rsc, GList **list)
{
    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive)
               && (list != NULL));

    if (rsc == orig_rsc) {
        /* For the resource itself, add all of its own colocations and relevant
         * colocations from its parent (if any).
         */
        pcmk__add_this_with_list(list, rsc->rsc_cons, orig_rsc);
        if (rsc->parent != NULL) {
            rsc->parent->cmds->this_with_colocations(rsc->parent, orig_rsc, list);
        }
    } else {
        // For an ancestor, add only explicitly configured constraints
        for (GList *iter = rsc->rsc_cons; iter != NULL; iter = iter->next) {
            pcmk__colocation_t *colocation = iter->data;

            if (pcmk_is_set(colocation->flags, pcmk__coloc_explicit)) {
                pcmk__add_this_with(list, colocation, orig_rsc);
            }
        }
    }
}

/*!
 * \internal
 * \brief Return action flags for a given primitive resource action
 *
 * \param[in,out] action  Action to get flags for
 * \param[in]     node    If not NULL, limit effects to this node (ignored)
 *
 * \return Flags appropriate to \p action on \p node
 */
uint32_t
pcmk__primitive_action_flags(pcmk_action_t *action, const pcmk_node_t *node)
{
    CRM_ASSERT(action != NULL);
    return (uint32_t) action->flags;
}

/*!
 * \internal
 * \brief Check whether a node is a multiply active resource's expected node
 *
 * \param[in] rsc  Resource to check
 * \param[in] node  Node to check
 *
 * \return true if \p rsc is multiply active with multiple-active set to
 *         stop_unexpected, and \p node is the node where it will remain active
 * \note This assumes that the resource's next role cannot be changed to stopped
 *       after this is called, which should be reasonable if status has already
 *       been unpacked and resources have been assigned to nodes.
 */
static bool
is_expected_node(const pcmk_resource_t *rsc, const pcmk_node_t *node)
{
    return pcmk_all_flags_set(rsc->flags,
                              pcmk_rsc_stop_unexpected|pcmk_rsc_restarting)
           && (rsc->next_role > pcmk_role_stopped)
           && pe__same_node(rsc->allocated_to, node);
}

/*!
 * \internal
 * \brief Schedule actions needed to stop a resource wherever it is active
 *
 * \param[in,out] rsc       Resource being stopped
 * \param[in]     node      Node where resource is being stopped (ignored)
 * \param[in]     optional  Whether actions should be optional
 */
static void
stop_resource(pcmk_resource_t *rsc, pcmk_node_t *node, bool optional)
{
    for (GList *iter = rsc->running_on; iter != NULL; iter = iter->next) {
        pcmk_node_t *current = (pcmk_node_t *) iter->data;
        pcmk_action_t *stop = NULL;

        if (is_expected_node(rsc, current)) {
            /* We are scheduling restart actions for a multiply active resource
             * with multiple-active=stop_unexpected, and this is where it should
             * not be stopped.
             */
            pcmk__rsc_trace(rsc,
                            "Skipping stop of multiply active resource %s "
                            "on expected node %s",
                            rsc->id, pe__node_name(current));
            continue;
        }

        if (rsc->partial_migration_target != NULL) {
            // Continue migration if node originally was and remains target
            if (pe__same_node(current, rsc->partial_migration_target)
                && pe__same_node(current, rsc->allocated_to)) {
                pcmk__rsc_trace(rsc,
                                "Skipping stop of %s on %s "
                                "because partial migration there will continue",
                                rsc->id, pe__node_name(current));
                continue;
            } else {
                pcmk__rsc_trace(rsc,
                                "Forcing stop of %s on %s "
                                "because migration target changed",
                                rsc->id, pe__node_name(current));
                optional = false;
            }
        }

        pcmk__rsc_trace(rsc, "Scheduling stop of %s on %s",
                        rsc->id, pe__node_name(current));
        stop = stop_action(rsc, current, optional);

        if (rsc->allocated_to == NULL) {
            pe_action_set_reason(stop, "node availability", true);
        } else if (pcmk_all_flags_set(rsc->flags, pcmk_rsc_restarting
                                                  |pcmk_rsc_stop_unexpected)) {
            /* We are stopping a multiply active resource on a node that is
             * not its expected node, and we are still scheduling restart
             * actions, so the stop is for being multiply active.
             */
            pe_action_set_reason(stop, "being multiply active", true);
        }

        if (!pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
            pe__clear_action_flags(stop, pcmk_action_runnable);
        }

        if (pcmk_is_set(rsc->cluster->flags, pcmk_sched_remove_after_stop)) {
            pcmk__schedule_cleanup(rsc, current, optional);
        }

        if (pcmk_is_set(rsc->flags, pcmk_rsc_needs_unfencing)) {
            pcmk_action_t *unfence = pe_fence_op(current, PCMK_ACTION_ON, true,
                                                 NULL, false, rsc->cluster);

            order_actions(stop, unfence, pcmk__ar_then_implies_first);
            if (!pcmk__node_unfenced(current)) {
                pcmk__sched_err("Stopping %s until %s can be unfenced",
                                rsc->id, pe__node_name(current));
            }
        }
    }
}

/*!
 * \internal
 * \brief Schedule actions needed to start a resource on a node
 *
 * \param[in,out] rsc       Resource being started
 * \param[in,out] node      Node where resource should be started
 * \param[in]     optional  Whether actions should be optional
 */
static void
start_resource(pcmk_resource_t *rsc, pcmk_node_t *node, bool optional)
{
    pcmk_action_t *start = NULL;

    CRM_ASSERT(node != NULL);

    pcmk__rsc_trace(rsc, "Scheduling %s start of %s on %s (score %d)",
                    (optional? "optional" : "required"), rsc->id,
                    pe__node_name(node), node->weight);
    start = start_action(rsc, node, TRUE);

    pcmk__order_vs_unfence(rsc, node, start, pcmk__ar_first_implies_then);

    if (pcmk_is_set(start->flags, pcmk_action_runnable) && !optional) {
        pe__clear_action_flags(start, pcmk_action_optional);
    }

    if (is_expected_node(rsc, node)) {
        /* This could be a problem if the start becomes necessary for other
         * reasons later.
         */
        pcmk__rsc_trace(rsc,
                        "Start of multiply active resouce %s "
                        "on expected node %s will be a pseudo-action",
                        rsc->id, pe__node_name(node));
        pe__set_action_flags(start, pcmk_action_pseudo);
    }
}

/*!
 * \internal
 * \brief Schedule actions needed to promote a resource on a node
 *
 * \param[in,out] rsc       Resource being promoted
 * \param[in]     node      Node where resource should be promoted
 * \param[in]     optional  Whether actions should be optional
 */
static void
promote_resource(pcmk_resource_t *rsc, pcmk_node_t *node, bool optional)
{
    GList *iter = NULL;
    GList *action_list = NULL;
    bool runnable = true;

    CRM_ASSERT(node != NULL);

    // Any start must be runnable for promotion to be runnable
    action_list = pe__resource_actions(rsc, node, PCMK_ACTION_START, true);
    for (iter = action_list; iter != NULL; iter = iter->next) {
        pcmk_action_t *start = (pcmk_action_t *) iter->data;

        if (!pcmk_is_set(start->flags, pcmk_action_runnable)) {
            runnable = false;
        }
    }
    g_list_free(action_list);

    if (runnable) {
        pcmk_action_t *promote = promote_action(rsc, node, optional);

        pcmk__rsc_trace(rsc, "Scheduling %s promotion of %s on %s",
                        (optional? "optional" : "required"), rsc->id,
                        pe__node_name(node));

        if (is_expected_node(rsc, node)) {
            /* This could be a problem if the promote becomes necessary for
             * other reasons later.
             */
            pcmk__rsc_trace(rsc,
                            "Promotion of multiply active resouce %s "
                            "on expected node %s will be a pseudo-action",
                            rsc->id, pe__node_name(node));
            pe__set_action_flags(promote, pcmk_action_pseudo);
        }
    } else {
        pcmk__rsc_trace(rsc, "Not promoting %s on %s: start unrunnable",
                        rsc->id, pe__node_name(node));
        action_list = pe__resource_actions(rsc, node, PCMK_ACTION_PROMOTE,
                                           true);
        for (iter = action_list; iter != NULL; iter = iter->next) {
            pcmk_action_t *promote = (pcmk_action_t *) iter->data;

            pe__clear_action_flags(promote, pcmk_action_runnable);
        }
        g_list_free(action_list);
    }
}

/*!
 * \internal
 * \brief Schedule actions needed to demote a resource wherever it is active
 *
 * \param[in,out] rsc       Resource being demoted
 * \param[in]     node      Node where resource should be demoted (ignored)
 * \param[in]     optional  Whether actions should be optional
 */
static void
demote_resource(pcmk_resource_t *rsc, pcmk_node_t *node, bool optional)
{
    /* Since this will only be called for a primitive (possibly as an instance
     * of a collective resource), the resource is multiply active if it is
     * running on more than one node, so we want to demote on all of them as
     * part of recovery, regardless of which one is the desired node.
     */
    for (GList *iter = rsc->running_on; iter != NULL; iter = iter->next) {
        pcmk_node_t *current = (pcmk_node_t *) iter->data;

        if (is_expected_node(rsc, current)) {
            pcmk__rsc_trace(rsc,
                            "Skipping demote of multiply active resource %s "
                            "on expected node %s",
                            rsc->id, pe__node_name(current));
        } else {
            pcmk__rsc_trace(rsc, "Scheduling %s demotion of %s on %s",
                            (optional? "optional" : "required"), rsc->id,
                            pe__node_name(current));
            demote_action(rsc, current, optional);
        }
    }
}

static void
assert_role_error(pcmk_resource_t *rsc, pcmk_node_t *node, bool optional)
{
    CRM_ASSERT(false);
}

/*!
 * \internal
 * \brief Schedule cleanup of a resource
 *
 * \param[in,out] rsc       Resource to clean up
 * \param[in]     node      Node to clean up on
 * \param[in]     optional  Whether clean-up should be optional
 */
void
pcmk__schedule_cleanup(pcmk_resource_t *rsc, const pcmk_node_t *node,
                       bool optional)
{
    /* If the cleanup is required, its orderings are optional, because they're
     * relevant only if both actions are required. Conversely, if the cleanup is
     * optional, the orderings make the then action required if the first action
     * becomes required.
     */
    uint32_t flag = optional? pcmk__ar_first_implies_then : pcmk__ar_ordered;

    CRM_CHECK((rsc != NULL) && (node != NULL), return);

    if (pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {
        pcmk__rsc_trace(rsc, "Skipping clean-up of %s on %s: resource failed",
                        rsc->id, pe__node_name(node));
        return;
    }

    if (node->details->unclean || !node->details->online) {
        pcmk__rsc_trace(rsc, "Skipping clean-up of %s on %s: node unavailable",
                        rsc->id, pe__node_name(node));
        return;
    }

    crm_notice("Scheduling clean-up of %s on %s", rsc->id, pe__node_name(node));
    delete_action(rsc, node, optional);

    // stop -> clean-up -> start
    pcmk__order_resource_actions(rsc, PCMK_ACTION_STOP,
                                 rsc, PCMK_ACTION_DELETE, flag);
    pcmk__order_resource_actions(rsc, PCMK_ACTION_DELETE,
                                 rsc, PCMK_ACTION_START, flag);
}

/*!
 * \internal
 * \brief Add primitive meta-attributes relevant to graph actions to XML
 *
 * \param[in]     rsc  Primitive resource whose meta-attributes should be added
 * \param[in,out] xml  Transition graph action attributes XML to add to
 */
void
pcmk__primitive_add_graph_meta(const pcmk_resource_t *rsc, xmlNode *xml)
{
    char *name = NULL;
    char *value = NULL;
    const pcmk_resource_t *parent = NULL;

    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive)
               && (xml != NULL));

    /* Clone instance numbers get set internally as meta-attributes, and are
     * needed in the transition graph (for example, to tell unique clone
     * instances apart).
     */
    value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_INCARNATION);
    if (value != NULL) {
        name = crm_meta_name(XML_RSC_ATTR_INCARNATION);
        crm_xml_add(xml, name, value);
        free(name);
    }

    // Not sure if this one is really needed ...
    value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_REMOTE_NODE);
    if (value != NULL) {
        name = crm_meta_name(XML_RSC_ATTR_REMOTE_NODE);
        crm_xml_add(xml, name, value);
        free(name);
    }

    /* The container meta-attribute can be set on the primitive itself or one of
     * its parents (for example, a group inside a container resource), so check
     * them all, and keep the highest one found.
     */
    for (parent = rsc; parent != NULL; parent = parent->parent) {
        if (parent->container != NULL) {
            crm_xml_add(xml, CRM_META "_" XML_RSC_ATTR_CONTAINER,
                        parent->container->id);
        }
    }

    /* Bundle replica children will get their external-ip set internally as a
     * meta-attribute. The graph action needs it, but under a different naming
     * convention than other meta-attributes.
     */
    value = g_hash_table_lookup(rsc->meta, "external-ip");
    if (value != NULL) {
        crm_xml_add(xml, "pcmk_external_ip", value);
    }
}

// Primitive implementation of pcmk_assignment_methods_t:add_utilization()
void
pcmk__primitive_add_utilization(const pcmk_resource_t *rsc,
                                const pcmk_resource_t *orig_rsc,
                                GList *all_rscs, GHashTable *utilization)
{
    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive)
               && (orig_rsc != NULL) && (utilization != NULL));

    if (!pcmk_is_set(rsc->flags, pcmk_rsc_unassigned)) {
        return;
    }

    pcmk__rsc_trace(orig_rsc,
                    "%s: Adding primitive %s as colocated utilization",
                    orig_rsc->id, rsc->id);
    pcmk__release_node_capacity(utilization, rsc);
}

/*!
 * \internal
 * \brief Get epoch time of node's shutdown attribute (or now if none)
 *
 * \param[in,out] node  Node to check
 *
 * \return Epoch time corresponding to shutdown attribute if set or now if not
 */
static time_t
shutdown_time(pcmk_node_t *node)
{
    const char *shutdown = pe_node_attribute_raw(node, XML_CIB_ATTR_SHUTDOWN);
    time_t result = 0;

    if (shutdown != NULL) {
        long long result_ll;

        if (pcmk__scan_ll(shutdown, &result_ll, 0LL) == pcmk_rc_ok) {
            result = (time_t) result_ll;
        }
    }
    return (result == 0)? get_effective_time(node->details->data_set) : result;
}

/*!
 * \internal
 * \brief Ban a resource from a node if it's not locked to the node
 *
 * \param[in]     data       Node to check
 * \param[in,out] user_data  Resource to check
 */
static void
ban_if_not_locked(gpointer data, gpointer user_data)
{
    const pcmk_node_t *node = (const pcmk_node_t *) data;
    pcmk_resource_t *rsc = (pcmk_resource_t *) user_data;

    if (strcmp(node->details->uname, rsc->lock_node->details->uname) != 0) {
        resource_location(rsc, node, -CRM_SCORE_INFINITY,
                          XML_CONFIG_ATTR_SHUTDOWN_LOCK, rsc->cluster);
    }
}

// Primitive implementation of pcmk_assignment_methods_t:shutdown_lock()
void
pcmk__primitive_shutdown_lock(pcmk_resource_t *rsc)
{
    const char *class = NULL;

    CRM_ASSERT((rsc != NULL) && (rsc->variant == pcmk_rsc_variant_primitive));

    class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

    // Fence devices and remote connections can't be locked
    if (pcmk__str_eq(class, PCMK_RESOURCE_CLASS_STONITH, pcmk__str_null_matches)
        || pe__resource_is_remote_conn(rsc)) {
        return;
    }

    if (rsc->lock_node != NULL) {
        // The lock was obtained from resource history

        if (rsc->running_on != NULL) {
            /* The resource was started elsewhere even though it is now
             * considered locked. This shouldn't be possible, but as a
             * failsafe, we don't want to disturb the resource now.
             */
            pcmk__rsc_info(rsc,
                           "Cancelling shutdown lock "
                           "because %s is already active", rsc->id);
            pe__clear_resource_history(rsc, rsc->lock_node);
            rsc->lock_node = NULL;
            rsc->lock_time = 0;
        }

    // Only a resource active on exactly one node can be locked
    } else if (pcmk__list_of_1(rsc->running_on)) {
        pcmk_node_t *node = rsc->running_on->data;

        if (node->details->shutdown) {
            if (node->details->unclean) {
                pcmk__rsc_debug(rsc,
                                "Not locking %s to unclean %s for shutdown",
                                rsc->id, pe__node_name(node));
            } else {
                rsc->lock_node = node;
                rsc->lock_time = shutdown_time(node);
            }
        }
    }

    if (rsc->lock_node == NULL) {
        // No lock needed
        return;
    }

    if (rsc->cluster->shutdown_lock > 0) {
        time_t lock_expiration = rsc->lock_time + rsc->cluster->shutdown_lock;

        pcmk__rsc_info(rsc, "Locking %s to %s due to shutdown (expires @%lld)",
                       rsc->id, pe__node_name(rsc->lock_node),
                       (long long) lock_expiration);
        pe__update_recheck_time(++lock_expiration, rsc->cluster,
                                "shutdown lock expiration");
    } else {
        pcmk__rsc_info(rsc, "Locking %s to %s due to shutdown",
                       rsc->id, pe__node_name(rsc->lock_node));
    }

    // If resource is locked to one node, ban it from all other nodes
    g_list_foreach(rsc->cluster->nodes, ban_if_not_locked, rsc);
}
