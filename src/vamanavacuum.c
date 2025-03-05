#include "postgres.h"

#include <math.h>

#include "access/generic_xlog.h"
#include "commands/vacuum.h"
#include "vamana.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

#if PG_VERSION_NUM >= 180000
#define vacuum_delay_point() vacuum_delay_point(false)
#endif

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
vamanabulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	// HnswVacuumState vacuumstate;

	// InitVacuumState(&vacuumstate, info, stats, callback, callback_state);

	// /* Pass 1: Remove heap TIDs */
	// RemoveHeapTids(&vacuumstate);

	// /* Pass 2: Repair graph */
	// RepairGraph(&vacuumstate);

	// /* Pass 3: Mark as deleted */
	// MarkDeleted(&vacuumstate);

	// FreeVacuumState(&vacuumstate);

	// return vacuumstate.stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
vamanavacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	// Relation	rel = info->index;

	// if (info->analyze_only)
	// 	return stats;

	// /* stats is NULL if ambulkdelete not called */
	// /* OK to return NULL if index not changed */
	// if (stats == NULL)
	// 	return NULL;

	// stats->num_pages = RelationGetNumberOfBlocks(rel);

	// return stats;
}
