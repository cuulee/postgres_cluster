/*
 * pg_dtm.c
 *
 * Pluggable distributed transaction manager
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "access/xlogdefs.h"
#include "access/xact.h"
#include "access/xtm.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "access/twophase.h"
#include <utils/guc.h>
#include "utils/hsearch.h"
#include "utils/tqual.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "libdtm.h"

typedef struct  
{	
    LWLockId	lock; /* protect access to hash table */
} DtmState;


#define DTM_SHMEM_SIZE (1024*1024)
#define DTM_HASH_SIZE  1003
#define XTM_CONNECT_ATTEMPTS 10


void _PG_init(void);
void _PG_fini(void);

static void DtmEnsureConnection(void);
static Snapshot DtmGetSnapshot(void);
static void DtmMergeSnapshots(Snapshot dst, Snapshot src);
static Snapshot DtmCopySnapshot(Snapshot snapshot);
static XidStatus DtmGetTransactionStatus(TransactionId xid, XLogRecPtr *lsn);
static void DtmSetTransactionStatus(TransactionId xid, int nsubxids, TransactionId *subxids, XidStatus status, XLogRecPtr lsn);
static void DtmUpdateRecentXmin(void);
static void DtmInitialize();
static void DtmXactCallback(XactEvent event, void *arg);

static bool TransactionIdIsInDtmSnapshot(TransactionId xid);
static bool TransactionIdIsInDoubt(TransactionId xid);

static void dtm_shmem_startup(void);

static shmem_startup_hook_type prev_shmem_startup_hook;
static HTAB* xid_in_doubt;
static DtmState* dtm;
static TransactionId DtmCurrentXid = InvalidTransactionId;
static Snapshot CurrentTransactionSnapshot;

static NodeId DtmNodeId;
static DTMConn DtmConn;
static SnapshotData DtmSnapshot = { HeapTupleSatisfiesMVCC };
static SnapshotData DtmLocalSnapshot = { HeapTupleSatisfiesMVCC };
static bool DtmGlobalTransaction = false;
static TransactionManager DtmTM = { DtmGetTransactionStatus, DtmSetTransactionStatus, DtmGetSnapshot, DtmCopySnapshot };
static DTMConn DtmConn;

#define XTM_TRACE(fmt, ...) 
#define XTM_INFO(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
//#define XTM_INFO(fmt, ...) 

static void DtmEnsureConnection(void)
{
    int attempt = 0;
    XTM_TRACE("XTM: DtmEnsureConnection\n");
    while (attempt < XTM_CONNECT_ATTEMPTS) { 
        if (DtmConn) {
            return;
        }
        XTM_TRACE("XTM: DtmEnsureConnection, attempt #%u\n", attempt);
        DtmConn = DtmConnect("127.0.0.1", 5431);
        attempt++;
    }
    elog(ERROR, "Failed to connect to DTMD");
}

static void DumpSnapshot(Snapshot s, char *name)
{
	int i;
	char buf[10000];
	char *cursor = buf;
	cursor += sprintf(
		cursor,
		"snapshot %s for transaction %d: xmin=%d, xmax=%d, active=[",
		name, GetCurrentTransactionId(), s->xmin, s->xmax
	);
	for (i = 0; i < s->xcnt; i++) {
		if (i == 0) {
			cursor += sprintf(cursor, "%d", s->xip[i]);
		} else {
			cursor += sprintf(cursor, ", %d", s->xip[i]);
		}
	}
	cursor += sprintf(cursor, "]");
	XTM_INFO("%s\n", buf);
}

static bool TransactionIdIsInDtmSnapshot(TransactionId xid)
{
    return xid >= DtmSnapshot.xmax 
        || bsearch(&xid, DtmSnapshot.xip, DtmSnapshot.xcnt, sizeof(TransactionId), xidComparator) != NULL;
}

static bool TransactionIdIsInDoubt(TransactionId xid)
{
    bool inDoubt;

    if (!TransactionIdIsInDtmSnapshot(xid)) {
        LWLockAcquire(dtm->lock, LW_SHARED); 
        inDoubt = hash_search(xid_in_doubt, &xid, HASH_FIND, NULL) != NULL;
        LWLockRelease(dtm->lock);
        if (!inDoubt) {
            XLogRecPtr lsn;
            inDoubt = CLOGTransactionIdGetStatus(xid, &lsn) != TRANSACTION_STATUS_IN_PROGRESS;
        }
        if (inDoubt) {
            XTM_INFO("Wait for transaction %d to complete\n", xid);
            XactLockTableWait(xid, NULL, NULL, XLTW_None);
            return true;
        }
    }
    return false;
}
 
static void DtmMergeSnapshots(Snapshot dst, Snapshot src)
{
    int i, j, n;
    TransactionId xid;
    Snapshot local;

    Assert(TransactionIdIsValid(src->xmin) && TransactionIdIsValid(src->xmax));

  GetLocalSnapshot:
    local = GetSnapshotData(&DtmLocalSnapshot);
    for (i = 0; i < local->xcnt; i++) {
        if (TransactionIdIsInDoubt(local->xip[i])) {
            goto GetLocalSnapshot;
        }   
    }
    for (xid = local->xmax; xid < src->xmax; xid++) { 
        if (TransactionIdIsInDoubt(xid)) { 
            goto GetLocalSnapshot;
        }
    }
    DumpSnapshot(local, "local");
    DumpSnapshot(src, "DTM");

    /* Merge two snapshots: produce most restrictive snapshots whihc includes running transactions from both of them */
    dst->xmin = local->xmin < src->xmin ? local->xmin : src->xmin;
    dst->xmax = local->xmax < src->xmax ? local->xmax : src->xmax;
    
    n = local->xcnt;
    for (xid = local->xmax; xid <= src->xmin; xid++) { 
        local->xip[n++] = xid;
    }
    memcpy(local->xip + n, src->xip, src->xcnt*sizeof(TransactionId));
    n += src->xcnt;
    Assert(n <= GetMaxSnapshotXidCount());
    
    qsort(local->xip, n, sizeof(TransactionId), xidComparator); 
    xid = InvalidTransactionId;
         
    for (i = 0, j = 0; i < n && local->xip[i] < dst->xmax; i++) { 
        if (local->xip[i] != xid) { 
            dst->xip[j++] = xid = local->xip[i];
        }
    }
    dst->xcnt = j;
    DumpSnapshot(dst, "merged");
}

static void DtmUpdateRecentXmin(void)
{
    TransactionId xmin = DtmSnapshot.xmin;

    XTM_TRACE("XTM: DtmUpdateRecentXmin \n");

    if (xmin != InvalidTransactionId) { 
        xmin -= vacuum_defer_cleanup_age;        
        if (!TransactionIdIsNormal(xmin)) {
            xmin = FirstNormalTransactionId;
        }
        if (RecentGlobalDataXmin > xmin) { 
            RecentGlobalDataXmin = xmin;
        }
        if (RecentGlobalXmin > xmin) { 
            RecentGlobalXmin = xmin;
        }
        if (RecentXmin > xmin) {
            RecentXmin = xmin;
        }
    }   
}

static Snapshot DtmCopySnapshot(Snapshot snapshot)
{
    Snapshot newsnap;
    Size size = sizeof(SnapshotData) + GetMaxSnapshotXidCount() * sizeof(TransactionId);
    Size subxipoff = size;
    if (snapshot->subxcnt > 0) {
        size += snapshot->subxcnt * sizeof(TransactionId);
    }
    newsnap = (Snapshot) MemoryContextAlloc(TopTransactionContext, size);
    memcpy(newsnap, snapshot, sizeof(SnapshotData));
    
    newsnap->regd_count = 0;
    newsnap->active_count = 0;
    newsnap->copied = true;
    
    newsnap->xip = (TransactionId *) (newsnap + 1);
    if (snapshot->xcnt > 0)
    {
        memcpy(newsnap->xip, snapshot->xip, snapshot->xcnt * sizeof(TransactionId));
    }
    if (snapshot->subxcnt > 0 &&
        (!snapshot->suboverflowed || snapshot->takenDuringRecovery))
    {
        newsnap->subxip = (TransactionId *) ((char *) newsnap + subxipoff);
        memcpy(newsnap->subxip, snapshot->subxip,
               snapshot->subxcnt * sizeof(TransactionId));
    }
    else
        newsnap->subxip = NULL;
    
    return newsnap;
}
           

static Snapshot DtmGetSnapshot()
{
    CurrentTransactionSnapshot = GetLocalTransactionSnapshot(); 
    return CurrentTransactionSnapshot;
}


static XidStatus DtmGetTransactionStatus(TransactionId xid, XLogRecPtr *lsn)
{
    XidStatus status = CLOGTransactionIdGetStatus(xid, lsn);
    XTM_TRACE("XTM: DtmGetTransactionStatus \n");
    return status;
}


static void DtmSetTransactionStatus(TransactionId xid, int nsubxids, TransactionId *subxids, XidStatus status, XLogRecPtr lsn)
{
    XTM_TRACE("XTM: DtmSetTransactionStatus %u = %u \n", xid, status);
    if (!RecoveryInProgress()) { 
        if (DtmGlobalTransaction) { 
            /* Already should be IN_PROGRESS */
            /* CLOGTransactionIdSetTreeStatus(xid, nsubxids, subxids, TRANSACTION_STATUS_IN_PROGRESS, lsn); */
            DtmGlobalTransaction = false;
            CurrentTransactionSnapshot = NULL;
            if (status == TRANSACTION_STATUS_ABORTED) { 
                CLOGTransactionIdSetTreeStatus(xid, nsubxids, subxids, status, lsn);
                DtmEnsureConnection();
                DtmGlobalSetTransStatus(DtmConn, DtmNodeId, xid, status, false);
                XTM_INFO("Abort transaction %d\n", xid);
                return;
            } else { 
                DtmEnsureConnection();
                XTM_INFO("Begin commit transaction %d\n", xid);
                DtmCurrentXid = xid;
                LWLockAcquire(dtm->lock, LW_EXCLUSIVE);
                hash_search(xid_in_doubt, &DtmCurrentXid, HASH_ENTER, NULL);
                LWLockRelease(dtm->lock);
                if (!DtmGlobalSetTransStatus(DtmConn, DtmNodeId, xid, status, true)) { 
                    elog(ERROR, "DTMD failed to set transaction status");
                }
                XTM_INFO("Commit transaction %d\n", xid);
            }
        } else {
            XTM_INFO("Set transaction %u status in local CLOG" , xid);
        }
    } else { 
        XidStatus gs;
        DtmEnsureConnection();
        gs = DtmGlobalGetTransStatus(DtmConn, DtmNodeId, xid, false);
        if (gs != TRANSACTION_STATUS_UNKNOWN) { 
            status = gs;
        }
    }
    CLOGTransactionIdSetTreeStatus(xid, nsubxids, subxids, status, lsn);
}

static uint32 dtm_xid_hash_fn(const void *key, Size keysize)
{
	return (uint32)*(TransactionId*)key;
}

static int dtm_xid_match_fn(const void *key1, const void *key2, Size keysize)
{
	return *(TransactionId*)key1 - *(TransactionId*)key2;
}


static void DtmInitialize()
{
	bool found;
	static HASHCTL info;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	dtm = ShmemInitStruct("dtm", sizeof(DtmState), &found);    
	if (!found)
	{ 
        dtm->lock = LWLockAssign();
    }
	LWLockRelease(AddinShmemInitLock);

	info.keysize = sizeof(TransactionId);
	info.entrysize = sizeof(TransactionId);
	info.hash = dtm_xid_hash_fn;
	info.match = dtm_xid_match_fn;
	xid_in_doubt = ShmemInitHash("xid_in_doubt", DTM_HASH_SIZE, DTM_HASH_SIZE,
                                 &info,
                                 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
    
	RegisterXactCallback(DtmXactCallback, NULL);
    DtmInitSnapshot(&DtmLocalSnapshot);

    TM = &DtmTM;
}

static void
DtmXactCallback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && DtmCurrentXid != InvalidTransactionId) {
        LWLockAcquire(dtm->lock, LW_EXCLUSIVE); 
        hash_search(xid_in_doubt, &DtmCurrentXid, HASH_REMOVE, NULL);
        LWLockRelease(dtm->lock);
    }
}


/*
 *  ***************************************************************************
 */

void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the cs_* functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in imcs_shmem_startup().
	 */
	RequestAddinShmemSpace(DTM_SHMEM_SIZE);
	RequestAddinLWLocks(1);
 
	DefineCustomIntVariable("dtm.node_id",
                            "Identifier of node in distributed cluster for DTM",
							NULL,
							&DtmNodeId,
							0,
							0, 
                            INT_MAX,
							PGC_BACKEND,
							0,
							NULL,
							NULL,
							NULL);

	/*
	 * Install hooks.
	 */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = dtm_shmem_startup;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	shmem_startup_hook = prev_shmem_startup_hook;
}


static void dtm_shmem_startup(void)
{
	if (prev_shmem_startup_hook) {
		prev_shmem_startup_hook();
    }
	DtmInitialize();
}

/*
 *  ***************************************************************************
 */

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(dtm_begin_transaction);
PG_FUNCTION_INFO_V1(dtm_get_current_snapshot_xmax);
PG_FUNCTION_INFO_V1(dtm_get_current_snapshot_xmin);

Datum
dtm_begin_transaction(PG_FUNCTION_ARGS)
{
    GlobalTransactionId gtid;
    ArrayType* nodes = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType* xids = PG_GETARG_ARRAYTYPE_P(1);    
    gtid.xids = (TransactionId*)ARR_DATA_PTR(xids);
    gtid.nodes = (NodeId*)ARR_DATA_PTR(nodes);
    gtid.nNodes = ArrayGetNItems(ARR_NDIM(nodes), ARR_DIMS(nodes));    
    DtmGlobalTransaction = true;
    Assert(gtid.xids[DtmNodeId] == GetCurrentTransactionId());
    XTM_INFO("Start transaction {%d,%d} at node %d\n", gtid.xids[0], gtid.xids[1], DtmNodeId);
    if (DtmNodeId == gtid.nodes[0]) { 
        DtmEnsureConnection();
        DtmGlobalStartTransaction(DtmConn, &gtid);
    }
    DtmEnsureConnection();    
    DtmGlobalGetSnapshot(DtmConn, DtmNodeId, gtid.xids[DtmNodeId], &DtmSnapshot);    
    Assert(CurrentTransactionSnapshot != NULL);
    DtmMergeSnapshots(CurrentTransactionSnapshot, &DtmSnapshot);
    DtmUpdateRecentXmin();
	PG_RETURN_VOID();
}

Datum
dtm_get_current_snapshot_xmin(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(CurrentTransactionSnapshot->xmin);
}

Datum
dtm_get_current_snapshot_xmax(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(CurrentTransactionSnapshot->xmax);
}