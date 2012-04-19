/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef BRT_FLUSHER
#define BRT_FLUSHER
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

// This must be first to make the 64-bit file mode work right in Linux
#include <brttypes.h>

typedef enum {
    BRT_FLUSHER_CLEANER_TOTAL_NODES = 0,     // total number of nodes whose buffers are potentially flushed by cleaner thread
    BRT_FLUSHER_CLEANER_H1_NODES,            // number of nodes of height one whose message buffers are flushed by cleaner thread
    BRT_FLUSHER_CLEANER_HGT1_NODES,          // number of nodes of height > 1 whose message buffers are flushed by cleaner thread
    BRT_FLUSHER_CLEANER_EMPTY_NODES,         // number of nodes that are selected by cleaner, but whose buffers are empty
    BRT_FLUSHER_CLEANER_NODES_DIRTIED,       // number of nodes that are made dirty by the cleaner thread
    BRT_FLUSHER_CLEANER_MAX_BUFFER_SIZE,     // max number of bytes in message buffer flushed by cleaner thread
    BRT_FLUSHER_CLEANER_MIN_BUFFER_SIZE,
    BRT_FLUSHER_CLEANER_TOTAL_BUFFER_SIZE,
    BRT_FLUSHER_CLEANER_MAX_BUFFER_WORKDONE, // max workdone value of any message buffer flushed by cleaner thread
    BRT_FLUSHER_CLEANER_MIN_BUFFER_WORKDONE,
    BRT_FLUSHER_CLEANER_TOTAL_BUFFER_WORKDONE,
    BRT_FLUSHER_CLEANER_NUM_LEAF_MERGES_STARTED,     // number of times cleaner thread tries to merge a leaf
    BRT_FLUSHER_CLEANER_NUM_LEAF_MERGES_RUNNING,     // number of cleaner thread leaf merges in progress
    BRT_FLUSHER_CLEANER_NUM_LEAF_MERGES_COMPLETED,   // number of times cleaner thread successfully merges a leaf
    BRT_FLUSHER_CLEANER_NUM_DIRTIED_FOR_LEAF_MERGE,  // nodes dirtied by the "flush from root" process to merge a leaf node
    BRT_FLUSHER_FLUSH_TOTAL,                 // total number of flushes done by flusher threads or cleaner threads
    BRT_FLUSHER_FLUSH_IN_MEMORY,             // number of in memory flushes
    BRT_FLUSHER_FLUSH_NEEDED_IO,             // number of flushes that had to read a child (or part) off disk
    BRT_FLUSHER_FLUSH_CASCADES,              // number of flushes that triggered another flush in the child
    BRT_FLUSHER_FLUSH_CASCADES_1,            // number of flushes that triggered 1 cascading flush
    BRT_FLUSHER_FLUSH_CASCADES_2,            // number of flushes that triggered 2 cascading flushes
    BRT_FLUSHER_FLUSH_CASCADES_3,            // number of flushes that triggered 3 cascading flushes
    BRT_FLUSHER_FLUSH_CASCADES_4,            // number of flushes that triggered 4 cascading flushes
    BRT_FLUSHER_FLUSH_CASCADES_5,            // number of flushes that triggered 5 cascading flushes
    BRT_FLUSHER_FLUSH_CASCADES_GT_5,         // number of flushes that triggered more than 5 cascading flushes
    BRT_FLUSHER_SPLIT_LEAF,                  // number of leaf nodes split
    BRT_FLUSHER_SPLIT_NONLEAF,               // number of nonleaf nodes split
    BRT_FLUSHER_MERGE_LEAF,                  // number of times leaf nodes are merged
    BRT_FLUSHER_MERGE_NONLEAF,               // number of times nonleaf nodes are merged
    BRT_FLUSHER_BALANCE_LEAF,                // number of times a leaf node is balanced inside brt
    BRT_FLUSHER_STATUS_NUM_ROWS
} brt_flusher_status_entry;

typedef struct {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[BRT_FLUSHER_STATUS_NUM_ROWS];
} BRT_FLUSHER_STATUS_S, *BRT_FLUSHER_STATUS;

void toku_brt_flusher_status_init(void) __attribute__((__constructor__));
void toku_brt_flusher_get_status(BRT_FLUSHER_STATUS);

/**
 * Only for testing, not for production.
 *
 * Set a callback the flusher thread will use to signal various points
 * during its execution.
 */
void
toku_flusher_thread_set_callback(
    void (*callback_f)(int, void*),
    void* extra
    );

/**
 * Puts a workitem on the flusher thread queue, scheduling the node to be
 * flushed by flush_some_child.
 */
void
flush_node_on_background_thread(
    BRT brt,
    BRTNODE parent
    );

/**
 * Effect: Split a leaf node.
 * Argument "node" is node to be split.
 * Upon return:
 *   nodea and nodeb point to new nodes that result from split of "node"
 *   nodea is the left node that results from the split
 *   splitk is the right-most key of nodea
 */
void
brtleaf_split(
    struct brt_header* h,
    BRTNODE node,
    BRTNODE *nodea,
    BRTNODE *nodeb,
    DBT *splitk,
    BOOL create_new_node,
    u_int32_t num_dependent_nodes,
    BRTNODE* dependent_nodes
    );

/**
 * Effect: node must be a node-leaf node.  It is split into two nodes, and
 *         the fanout is split between them.
 *    Sets splitk->data pointer to a malloc'd value
 *    Sets nodea, and nodeb to the two new nodes.
 *    The caller must replace the old node with the two new nodes.
 *    This function will definitely reduce the number of children for the node,
 *    but it does not guarantee that the resulting nodes are smaller than nodesize.
 */
void
brt_nonleaf_split(
    struct brt_header* h,
    BRTNODE node,
    BRTNODE *nodea,
    BRTNODE *nodeb,
    DBT *splitk,
    u_int32_t num_dependent_nodes,
    BRTNODE* dependent_nodes
    );



/************************************************************************
 * HOT optimize, should perhaps be factored out to its own header file  *
 ************************************************************************
 */

typedef enum {
    BRT_HOT_NUM_STARTED = 0,      // number of HOT operations that have begun
    BRT_HOT_NUM_COMPLETED,        // number of HOT operations that have successfully completed
    BRT_HOT_NUM_ABORTED,          // number of HOT operations that have been aborted
    BRT_HOT_MAX_ROOT_FLUSH_COUNT, // max number of flushes from root ever required to optimize a tree
    BRT_HOT_STATUS_NUM_ROWS
} brt_hot_status_entry;

typedef struct {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[BRT_HOT_STATUS_NUM_ROWS];
} BRT_HOT_STATUS_S, *BRT_HOT_STATUS;

void toku_brt_hot_status_init(void) __attribute__((__constructor__));
void toku_brt_hot_get_status(BRT_HOT_STATUS);

/**
 * Takes given BRT and pushes all pending messages to the leaf nodes.
 */
int
toku_brt_hot_optimize(BRT brt,
                      int (*progress_callback)(void *extra, float progress),
                      void *progress_extra);

#endif // End of header guardian.
