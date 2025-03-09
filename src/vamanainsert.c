#include "postgres.h"

#include <math.h>

#include "access/generic_xlog.h"
#include "vamana.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/datum.h"
#include "utils/memutils.h"

/*
 * Insert a tuple into the index
 */
bool vamanainsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
                Relation heap, IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
                ,
                bool indexUnchanged
#endif
                ,
                IndexInfo *indexInfo)
{
    return false;
}