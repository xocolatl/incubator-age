/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "executor/cypher_executor.h"
#include "executor/cypher_utils.h"
#include "parser/cypher_parse_node.h"
#include "nodes/cypher_nodes.h"
#include "utils/agtype.h"
#include "utils/graphid.h"

static void begin_cypher_delete(CustomScanState *node, EState *estate,
                                int eflags);
static TupleTableSlot *exec_cypher_delete(CustomScanState *node);
static void end_cypher_delete(CustomScanState *node);
static void rescan_cypher_delete(CustomScanState *node);

static void process_delete_list(CustomScanState *node);

static void find_connected_edges(CustomScanState *node, char *graph_name, List *labels,
                                 char *var_name, graphid id, bool detach_delete);
static agtype_value *extract_entity(CustomScanState *node, TupleTableSlot *scanTupleSlot,
                                    int entity_position);
static void delete_entity(CustomScanState *node, char *graph_name,
                          char *label_name, HeapTuple tuple);

const CustomExecMethods cypher_delete_exec_methods = {DELETE_SCAN_STATE_NAME,
                                                      begin_cypher_delete,
                                                      exec_cypher_delete,
                                                      end_cypher_delete,
                                                      rescan_cypher_delete,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL};

/*
 * Initialization at the beginning of execution. Setup the child node,
 * setup its scan tuple slot and projection infp, expression context,
 * collect metadata about visible edges, and alter the commandid for
 * the transaction.
 */
static void begin_cypher_delete(CustomScanState *node, EState *estate,
                             int eflags)
{
    cypher_delete_custom_scan_state *css =
        (cypher_delete_custom_scan_state *)node;
    Plan *subplan;

    Assert(list_length(css->cs->custom_plans) == 1);

    // setup child
    subplan = linitial(css->cs->custom_plans);
    node->ss.ps.lefttree = ExecInitNode(subplan, estate, eflags);

    // setup expr context
    ExecAssignExprContext(estate, &node->ss.ps);

    // setup scan tuple slot and projection info
    ExecInitScanTupleSlot(estate, &node->ss,
                          ExecGetResultType(node->ss.ps.lefttree));

    if (!CYPHER_CLAUSE_IS_TERMINAL(css->flags))
    {
        TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

        ExecAssignProjectionInfo(&node->ss.ps, tupdesc);
    }

    /*
     *  Get all the labels that are visible to this delete clause at this point
     *  in the transaction. To be used later when the delete clause finds vertices.
     */
    css->edge_labels = get_all_edge_labels_per_graph(estate, css->delete_data->graph_oid);

    /*
     * Postgres does not assign the es_output_cid in queries that do
     * not write to disk, ie: SELECT commands. We need the command id
     * for our clauses, and we may need to initialize it. We cannot use
     * GetCurrentCommandId because there may be other cypher clauses
     * that have modified the command id.
     */
    if (estate->es_output_cid == 0)
        estate->es_output_cid = estate->es_snapshot->curcid;

    CommandCounterIncrement();
    Increment_Estate_CommandId(estate);
}

/*
 * Called once per tuple. If this is a terminal DELETE clause
 * process everyone of its child tuple, otherwise process the
 * next tuple.
 */
static TupleTableSlot *exec_cypher_delete(CustomScanState *node)
{
    cypher_delete_custom_scan_state *css =
        (cypher_delete_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    TupleTableSlot *slot;

    if (CYPHER_CLAUSE_IS_TERMINAL(css->flags))
    {
        /*
         * If the DELETE clause was the final cypher clause written
         * then we aren't returning anything from this result node.
         * So the exec_cypher_delete function will only be called once.
         * Therefore we will process all tuples from the subtree at once.
         */
        while(true)
        {
            //Process the subtree first
            Decrement_Estate_CommandId(estate)
            slot = ExecProcNode(node->ss.ps.lefttree);
            Increment_Estate_CommandId(estate)

            if (TupIsNull(slot))
                break;

            // setup the scantuple that the process_delete_list needs
            econtext->ecxt_scantuple =
                node->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;

            css->tuple_info = NIL;

            process_delete_list(node);
        }

        return NULL;
    }
    else
    {
        //Process the subtree first
        Decrement_Estate_CommandId(estate)
        slot = ExecProcNode(node->ss.ps.lefttree);
        Increment_Estate_CommandId(estate)

        if (TupIsNull(slot))
            return NULL;

        // setup the scantuple that the process_delete_list needs
        econtext->ecxt_scantuple =
            node->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;

        css->tuple_info = NIL;

        process_delete_list(node);

        econtext->ecxt_scantuple =
            ExecProject(node->ss.ps.lefttree->ps_ProjInfo);

        return ExecProject(node->ss.ps.ps_ProjInfo);
    }
}

/*
 * Called at the end of execution. Tell its child to
 * end its execution.
 */
static void end_cypher_delete(CustomScanState *node)
{
    ExecEndNode(node->ss.ps.lefttree);
}

/*
 * Used for rewinding the scan state and reprocessing the results.
 *
 * XXX: This is not currently supported. We need to find out
 * when this function will be called and determine a process
 * for allowing the Delete clause to run multiple times without
 * redundant edits to the database.
 */
static void rescan_cypher_delete(CustomScanState *node)
{
     ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cypher DELETE clause cannot be rescaned"),
                    errhint("its unsafe to use joins in a query with a Cypher DELETE clause")));
}

/*
 * Create the CustomScanState from the CustomScan and pass
 * necessary metadata.
 */
Node *create_cypher_delete_plan_state(CustomScan *cscan)
{
    cypher_delete_custom_scan_state *cypher_css =
        palloc0(sizeof(cypher_delete_custom_scan_state));
    cypher_delete_information *delete_data;
    char *serialized_data;
    Const *c;

    cypher_css->cs = cscan;

    // get the serialized data structure from the Const and deserialize it.
    c = linitial(cscan->custom_private);
    serialized_data = (char *)c->constvalue;
    delete_data = stringToNode(serialized_data);

    Assert(is_ag_node(delete_data, cypher_delete_information));

    cypher_css->delete_data = delete_data;
    cypher_css->flags = delete_data->flags;

    cypher_css->css.ss.ps.type = T_CustomScanState;
    cypher_css->css.methods = &cypher_delete_exec_methods;

    return (Node *)cypher_css;
}

/*
 * Extract the vertex or edge to be deleted, perform some type checking to validate
 * datum is an agtype vertex or edge.
 */
static agtype_value *extract_entity(CustomScanState *node, TupleTableSlot *scanTupleSlot,
                                    int entity_position)
{
    agtype_value *original_entity_value;
    agtype *original_entity;
    TupleDesc tupleDescriptor;

    tupleDescriptor = scanTupleSlot->tts_tupleDescriptor;

    // type checking, make sure the entity is an agtype vertex or edge
    if (tupleDescriptor->attrs[entity_position -1].atttypid != AGTYPEOID)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("DELETE clause can only delete agtype")));

    original_entity = DATUM_GET_AGTYPE_P(scanTupleSlot->tts_values[entity_position - 1]);
    original_entity_value = get_ith_agtype_value_from_container(&original_entity->root, 0);

    if (original_entity_value->type != AGTV_VERTEX && original_entity_value->type != AGTV_EDGE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("DELETE clause can only update vertex and edges")));

    return original_entity_value;
}

/*
 * Try and delete the entity that is describe by the HeapTuple in
 * the table described by the graph_name and label_name.
 */
static void delete_entity(CustomScanState *node, char *graph_name,
                          char *label_name, HeapTuple tuple)
{
    cypher_delete_custom_scan_state *css =
        (cypher_delete_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;
    ResultRelInfo *resultRelInfo, *saved_resultRelInfo;
    LockTupleMode lockmode;
    HeapUpdateFailureData hufd;
    HTSU_Result lock_result;
    HTSU_Result delete_result;
    Buffer buffer;

    resultRelInfo = create_entity_result_rel_info(estate, graph_name, label_name);
    ExecOpenIndices(resultRelInfo, false);

    // Find the physical tuple, this variable is coming from
    saved_resultRelInfo = estate->es_result_relation_info;
    estate->es_result_relation_info = resultRelInfo;

    lockmode = ExecUpdateLockMode(estate, resultRelInfo);

    lock_result = heap_lock_tuple(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
                                  lockmode, LockWaitBlock, false, &buffer, &hufd);

    /*
     * It is possible the entity may have already been deleted. If the tuple
     * can be deleted, the lock result will be HeapTupleMayBeUpdated. If the
     * tuple was already deleted by this DELETE clause, the result would be
     * HeapTupleSelfUpdated, if the result was deleted by a previous delete
     * clause, the result will HeapTupleInvisible. Throw an error if any
     * other result was returned.
     */
    if (lock_result == HeapTupleMayBeUpdated)
    {
        delete_result = heap_delete(resultRelInfo->ri_RelationDesc, &tuple->t_self,
                            estate->es_output_cid, estate->es_crosscheck_snapshot, true,
                            &hufd, false);

        /*
         * Unlike locking, the heap_delete either succeeded HeapTupleMayBeUpdate,
         * or it failed and returned any other result.
         */
        switch (delete_result)
        {
                case HeapTupleMayBeUpdated:
                        break;
                case HeapTupleSelfUpdated:
                        ereport(ERROR,
                                        (errcode(ERRCODE_INTERNAL_ERROR),
                                         errmsg("deleting the same entity more than once cannot happen")));
                        break;
                case HeapTupleUpdated:
                        ereport(ERROR,
                                        (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                                         errmsg("could not serialize access due to concurrent update")));
                        return;
                default:
                        elog(ERROR, "Entity failed to be update");
                        return;
        }

    }
    else if (lock_result != HeapTupleInvisible && lock_result != HeapTupleSelfUpdated)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("Entity could not be locked for updating")));

    }

    ReleaseBuffer(buffer);

    estate->es_result_relation_info = saved_resultRelInfo;

    ExecCloseIndices(resultRelInfo);
    heap_close(resultRelInfo->ri_RelationDesc, RowExclusiveLock);
}

/*
 * After the delete's subtress has been processed, we then go through
 * the list of variables to be deleted
 */
static void process_delete_list(CustomScanState *node)
{
    cypher_delete_custom_scan_state *css =
        (cypher_delete_custom_scan_state *)node;
    ListCell *lc;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    TupleTableSlot *scanTupleSlot = econtext->ecxt_scantuple;

    foreach(lc, css->delete_data->delete_items)
    {
        cypher_delete_item *item;
        agtype_value *original_entity_value, *id, *label;
        HeapTuple heap_tuple;
        char *label_name;
        Value *pos;
        int entity_position;
        bool is_deleted;

        item = lfirst(lc);

        pos = item->entity_position;
        entity_position = pos->val.ival;

        // skip if the entity is null
        if (scanTupleSlot->tts_isnull[entity_position - 1])
            continue;

        /*
         * find the where the entity came from, if the tuple was deleted
         * by a previous DELETE clause its safe to skip this tuple
         */
        heap_tuple = get_heap_tuple(node, item->var_name, &is_deleted);
        if (is_deleted || heap_tuple == NULL)
            continue;

        original_entity_value = extract_entity(node, scanTupleSlot, entity_position);

        id = get_agtype_value_object_value(original_entity_value, "id");
        label = get_agtype_value_object_value(original_entity_value, "label");
        label_name = pnstrdup(label->val.string.val, label->val.string.len);

        /*
         * For vertices, we need to check if the vertex is connected to any edges,
         * if there are, we need to either delete them, or throw an error, depending
         * on if the query specified the DETACH option.
         */
        if (original_entity_value->type == AGTV_VERTEX)
            find_connected_edges(node, css->delete_data->graph_name, css->edge_labels,
                                 item->var_name, id->val.int_value, css->delete_data->detach);

        /*
         * At this point, we are ready to delete the node/vertex.
         */
        delete_entity(node, css->delete_data->graph_name, label_name, heap_tuple);

        /*
         * Add the deleted tuple to the custom scan state's info on updated
         * tuples.
         */
        css->tuple_info = add_tuple_info(css->tuple_info, heap_tuple, item->var_name);
    }
}

/*
 * Find the edges connected to the given node. If there is any edges either delete them
 * or throw an error, depending on the detach delete option.
 */
static void find_connected_edges(CustomScanState *node, char *graph_name, List *labels,
                                 char *var_name, graphid id, bool detach_delete)
{
    cypher_delete_custom_scan_state *css =
        (cypher_delete_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;
    ListCell *lc;

    Increment_Estate_CommandId(estate);

    /*
     * We need to scan through all the edges to see if this vertex has
     * any edges attached to it.
     *
     * XXX: If we implement an on-disc graph storage system. Such as
     * an adjacency matrix, the performace of this check can be massively
     * improved. However, right now we have to scan every edge to see if
     * one has this vertex as a start or end vertex.
     */
    foreach(lc, labels)
    {
        char *label_name = lfirst(lc);
        ResultRelInfo *resultRelInfo;
        HeapScanDesc scan_desc;
        HeapTuple tuple;
        TupleTableSlot *slot;

        resultRelInfo = create_entity_result_rel_info(estate, graph_name, label_name);

        scan_desc = heap_beginscan(resultRelInfo->ri_RelationDesc, estate->es_snapshot, 0, NULL);

        slot = ExecInitExtraTupleSlot(estate,
                    RelationGetDescr(resultRelInfo->ri_RelationDesc));

        // scan the table
        while(true)
        {
            graphid startid, endid;
            bool isNull;

            tuple = heap_getnext(scan_desc, ForwardScanDirection);

            // no more tuples to process, break and scan the next label.
            if (!HeapTupleIsValid(tuple))
                break;

            ExecStoreTuple(tuple, slot, InvalidBuffer, false);

            startid = GRAPHID_GET_DATUM(slot_getattr(slot, Anum_ag_label_edge_table_start_id, &isNull));
            endid = GRAPHID_GET_DATUM(slot_getattr(slot, Anum_ag_label_edge_table_end_id, &isNull));

            if (id == startid || id == endid)
            {
                /*
                 * We have found an edge that uses the vertex. Either delete the edge
                 * or throw an error. Depending on whether the DETACH option was
                 * specified in the query.
                 */
                if (detach_delete)
                    delete_entity(node, graph_name, label_name, tuple);
                else
                    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("Cannot delete vertex %s, because it still has edges attached. "
                                    "To delete this vertex, you must first delete the attached edges.",
                                    var_name)));
            }
        }

        heap_endscan(scan_desc);
        heap_close(resultRelInfo->ri_RelationDesc, RowExclusiveLock);
    }

    Decrement_Estate_CommandId(estate);
}
