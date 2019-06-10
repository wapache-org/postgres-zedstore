/*
 * zedstore_tidpage.c
 *		Routines for handling the TID tree.
 *
 * A Zedstore table consists of multiple B-trees, one for each attribute. The
 * functions in this file deal with one B-tree at a time, it is the caller's
 * responsibility to tie together the scans of each btree.
 *
 * Operations:
 *
 * - Sequential scan in TID order
 *  - must be efficient with scanning multiple trees in sync
 *
 * - random lookups, by TID (for index scan)
 *
 * - range scans by TID (for bitmap index scan)
 *
 * NOTES:
 * - Locking order: child before parent, left before right
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstore_tidpage.c
 */
#include "postgres.h"

#include "access/zedstore_internal.h"
#include "access/zedstore_undo.h"
#include "lib/integerset.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/rel.h"

/* prototypes for local functions */
static void zsbt_tid_recompress_replace(Relation rel, Buffer oldbuf, List *items);
static bool zsbt_tid_fetch(Relation rel, zstid tid,
						   Buffer *buf_p, ZSUndoRecPtr *undo_ptr_p, bool *isdead_p);
static void zsbt_tid_add_items(Relation rel, Buffer buf, List *newitems);
static void zsbt_tid_replace_item(Relation rel, Buffer buf,
								  zstid oldtid, ZSTidArrayItem *replacementitem);
static ZSTidArrayItem *zsbt_tid_create_item(zstid tid, ZSUndoRecPtr undo_ptr, int nelements);

static TM_Result zsbt_tid_update_lock_old(Relation rel, zstid otid,
									  TransactionId xid, CommandId cid, bool key_update, Snapshot snapshot,
									  Snapshot crosscheck, bool wait, TM_FailureData *hufd, ZSUndoRecPtr *prevundoptr_p);
static void zsbt_tid_update_insert_new(Relation rel, zstid *newtid,
					   TransactionId xid, CommandId cid, ZSUndoRecPtr prevundoptr);
static void zsbt_tid_mark_old_updated(Relation rel, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, bool key_update, Snapshot snapshot);
static int zsbt_binsrch_tidpage(zstid key, ZSTidArrayItem *arr, int arr_elems);

/* ----------------------------------------------------------------
 *						 Public interface
 * ----------------------------------------------------------------
 */

/*
 * Begin a scan of the btree.
 */
void
zsbt_tid_begin_scan(Relation rel, zstid starttid,
					zstid endtid, Snapshot snapshot, ZSBtreeScan *scan)
{
	Buffer		buf;

	scan->rel = rel;
	scan->attno = ZS_META_ATTRIBUTE_NUM;
	scan->tupledesc = NULL;

	scan->snapshot = snapshot;
	scan->context = CurrentMemoryContext;
	scan->lastoff = InvalidOffsetNumber;
	scan->has_decompressed = false;
	scan->nexttid = starttid;
	scan->endtid = endtid;
	memset(&scan->recent_oldest_undo, 0, sizeof(scan->recent_oldest_undo));
	memset(&scan->array_undoptr, 0, sizeof(scan->array_undoptr));
	scan->array_datums = palloc(sizeof(Datum));
	scan->array_isnulls = palloc(sizeof(bool));
	scan->array_datums_allocated_size = 1;
	scan->array_num_elements = 0;
	scan->array_next_datum = 0;
	scan->nonvacuumable_status = ZSNV_NONE;

	buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, starttid, 0, true);
	if (!BufferIsValid(buf))
	{
		/* completely empty tree */
		scan->active = false;
		scan->lastbuf = InvalidBuffer;
		return;
	}
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	scan->active = true;
	scan->lastbuf = buf;

	zs_decompress_init(&scan->decompressor);
	scan->recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
}

/*
 * Reset the 'next' TID in a scan to the given TID.
 */
void
zsbt_tid_reset_scan(ZSBtreeScan *scan, zstid starttid)
{
	if (starttid < scan->nexttid)
	{
		/* have to restart from scratch. */
		scan->array_num_elements = 0;
		scan->array_next_datum = 0;
		scan->nexttid = starttid;
		scan->has_decompressed = false;
		if (scan->lastbuf != InvalidBuffer)
			ReleaseBuffer(scan->lastbuf);
		scan->lastbuf = InvalidBuffer;
	}
	else
		zsbt_scan_skip(scan, starttid);
}

void
zsbt_tid_end_scan(ZSBtreeScan *scan)
{
	if (!scan->active)
		return;

	if (scan->lastbuf != InvalidBuffer)
		ReleaseBuffer(scan->lastbuf);
	zs_decompress_free(&scan->decompressor);

	scan->active = false;
	scan->array_num_elements = 0;
	scan->array_next_datum = 0;
}

/*
 * Helper function of zsbt_scan_next(), to extract Datums from the given
 * array item into the scan->array_* fields.
 */
static void
zsbt_tid_scan_extract_array(ZSBtreeScan *scan, ZSTidArrayItem *aitem)
{
	int			nelements = aitem->t_nelements;
	zstid		tid = aitem->t_tid;

	/* skip over elements that we are not interested in */
	while (tid < scan->nexttid && nelements > 0)
	{
		tid++;
		nelements--;
	}

	/* leave out elements that are past end of range */
	if (tid + nelements > scan->endtid)
		nelements = scan->endtid - tid;

	scan->array_undoptr = aitem->t_undo_ptr;
	scan->array_num_elements = nelements;
	scan->array_next_datum = 0;
	if (scan->nexttid < tid)
		scan->nexttid = tid;
}

/*
 * Advance scan to next item.
 *
 * Return true if there was another item. The Datum/isnull of the item is
 * placed in scan->array_* fields. For a pass-by-ref datum, it's a palloc'd
 * copy that's valid until the next call.
 *
 * This is normally not used directly. See zsbt_scan_next_tid() and
 * zsbt_scan_next_fetch() wrappers, instead.
 */
zstid
zsbt_tid_scan_next(ZSBtreeScan *scan)
{
	Buffer		buf;
	bool		buf_is_locked = false;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSTidArrayItem *tiditems;
	int			ntiditems;
	BlockNumber	next;
	bool		visible;

	if (!scan->active)
		return InvalidZSTid;

	/*
	 * Process items, until we find something that is visible to the snapshot.
	 *
	 * This advances scan->nexttid as it goes.
	 */
	while (scan->nexttid < scan->endtid)
	{
		/*
		 * If we are still processing an array item, return next element from it.
		 */
		if (scan->array_next_datum < scan->array_num_elements)
			goto have_array;

		/*
		 * Scan the page for the next item.
		 */
		buf = scan->lastbuf;
		if (!buf_is_locked)
		{
			if (BufferIsValid(buf))
			{
				LockBuffer(buf, BUFFER_LOCK_SHARE);
				buf_is_locked = true;

				/*
				 * It's possible that the page was concurrently split or recycled by
				 * another backend (or ourselves). Have to re-check that the page is
				 * still valid.
				 */
				if (!zsbt_page_is_expected(scan->rel, scan->attno, scan->nexttid, 0, buf))
				{
					/*
					 * It's not valid for the TID we're looking for, but maybe it was the
					 * right page for the previous TID. In that case, we don't need to
					 * restart from the root, we can follow the right-link instead.
					 */
					if (zsbt_page_is_expected(scan->rel, scan->attno, scan->nexttid - 1, 0, buf))
					{
						page = BufferGetPage(buf);
						opaque = ZSBtreePageGetOpaque(page);
						next = opaque->zs_next;
						if (next != InvalidBlockNumber)
						{
							LockBuffer(buf, BUFFER_LOCK_UNLOCK);
							buf_is_locked = false;
							buf = ReleaseAndReadBuffer(buf, scan->rel, next);
							scan->lastbuf = buf;
							continue;
						}
					}

					UnlockReleaseBuffer(buf);
					buf_is_locked = false;
					buf = scan->lastbuf = InvalidBuffer;
				}
			}

			if (!BufferIsValid(buf))
			{
				buf = scan->lastbuf = zsbt_descend(scan->rel, scan->attno, scan->nexttid, 0, true);
				buf_is_locked = true;
			}
		}
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);
		Assert(opaque->zs_page_id == ZS_BTREE_PAGE_ID);

		/* TODO: check the last offset first, as an optimization */
		tiditems = PageGetZSTidArray(page);
		ntiditems = PageGetNumZSTidItems(page);
		for (int i = 0; i < ntiditems; i++)
		{
			ZSTidArrayItem *item = &tiditems[i];
			zstid		lasttid;
			TransactionId obsoleting_xid = InvalidTransactionId;
			ZSTidArrayItem *aitem;

			lasttid = zsbt_tid_item_lasttid(item);

			if (scan->nexttid > lasttid)
				continue;

			if (item->t_tid >= scan->endtid)
			{
				scan->nexttid = scan->endtid;
				break;
			}

			/* dead items are never considered visible. */
			if ((item->t_flags & ZSBT_TID_DEAD) != 0)
				visible = false;
			else
				visible = zs_SatisfiesVisibility(scan, item->t_undo_ptr, &obsoleting_xid, NULL);

			if (!visible)
			{
				if (scan->serializable && TransactionIdIsValid(obsoleting_xid))
					CheckForSerializableConflictOut(scan->rel, obsoleting_xid, scan->snapshot);
				scan->nexttid = lasttid + 1;
				continue;
			}

			/* copy the item, because we can't hold a lock on the page  */

			aitem = MemoryContextAlloc(scan->context, sizeof(ZSTidArrayItem));
			memcpy(aitem, item, sizeof(ZSTidArrayItem));

			zsbt_tid_scan_extract_array(scan, aitem);

			if (scan->array_next_datum < scan->array_num_elements)
			{
				LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
				buf_is_locked = false;
				break;
			}
		}

		if (scan->array_next_datum < scan->array_num_elements)
			continue;

		/* No more items on this page. Walk right, if possible */
		if (scan->nexttid < opaque->zs_hikey)
			scan->nexttid = opaque->zs_hikey;
		next = opaque->zs_next;
		if (next == BufferGetBlockNumber(buf))
			elog(ERROR, "btree page %u next-pointer points to itself", next);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		buf_is_locked = false;

		if (next == InvalidBlockNumber || scan->nexttid >= scan->endtid)
		{
			scan->active = false;
			scan->array_num_elements = 0;
			scan->array_next_datum = 0;
			ReleaseBuffer(scan->lastbuf);
			scan->lastbuf = InvalidBuffer;
			break;
		}

		scan->lastbuf = ReleaseAndReadBuffer(scan->lastbuf, scan->rel, next);
	}

	return InvalidZSTid;

have_array:
	/*
	 * If we are still processing an array item, return next element from it.
	 */
	Assert(scan->array_next_datum < scan->array_num_elements);

	scan->array_next_datum++;
	return scan->nexttid++;
}

/*
 * Get the last tid (plus one) in the tree.
 */
zstid
zsbt_get_last_tid(Relation rel)
{
	zstid		rightmostkey;
	zstid		tid;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	int			ntiditems;

	/* Find the rightmost leaf */
	rightmostkey = MaxZSTid;
	buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, rightmostkey, 0, true);
	if (!BufferIsValid(buf))
	{
		return MinZSTid;
	}
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);

	/*
	 * Look at the last item, for its tid.
	 */
	ntiditems = PageGetNumZSTidItems(page);
	if (ntiditems > 0)
	{
		ZSTidArrayItem *lastitem = &PageGetZSTidArray(page)[ntiditems - 1];

		tid = zsbt_tid_item_lasttid(lastitem) + 1;
	}
	else
	{
		tid = opaque->zs_lokey;
	}
	UnlockReleaseBuffer(buf);

	return tid;
}

/*
 * Insert a multiple TIDs.
 *
 * Populates the TIDs of the new tuples.
 *
 * If 'tid' in list is valid, then that TID is used. It better not be in use already. If
 * it's invalid, then a new TID is allocated, as we see best. (When inserting the
 * first column of the row, pass invalid, and for other columns, pass the TID
 * you got for the first column.)
 */
void
zsbt_tid_multi_insert(Relation rel, zstid *tids, int nitems,
					  TransactionId xid, CommandId cid, uint32 speculative_token, ZSUndoRecPtr prevundoptr)
{
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	int			ntiditems;
	zstid		insert_target_key;
	ZSUndoRec_Insert undorec;
	List	   *newitems;
	ZSUndoRecPtr undorecptr;
	zstid		endtid;
	zstid		tid;
	ZSTidArrayItem  *newitem;

	/*
	 * Insert to the rightmost leaf.
	 *
	 * TODO: use a Free Space Map to find suitable target.
	 */
	insert_target_key = MaxZSTid;
	buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, insert_target_key, 0, false);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);
	ntiditems = PageGetNumZSTidItems(page);

	/*
	 * Look at the last item, for its tid.
	 *
	 * assign TIDS for each item.
	 */
	if (ntiditems > 0)
	{
		ZSTidArrayItem *lastitem = &PageGetZSTidArray(page)[ntiditems - 1];

		endtid = lastitem->t_tid + lastitem->t_nelements;
	}
	else
	{
		endtid = opaque->zs_lokey;
	}
	tid = endtid;

	/* Form an undo record */
	if (xid != FrozenTransactionId)
	{
		undorec.rec.size = sizeof(ZSUndoRec_Insert);
		undorec.rec.type = ZSUNDO_TYPE_INSERT;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;
		undorec.rec.speculative_token = speculative_token;
		undorec.rec.prevundorec = prevundoptr;
		undorec.endtid = tid + nitems - 1;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}
	else
	{
		undorecptr = InvalidUndoPtr;
	}

	/*
	 * Create a single array item to represent all the TIDs.
	 */
	newitem = zsbt_tid_create_item(tid, undorecptr, nitems);
	newitems = list_make1(newitem);

	/* recompress and possibly split the page */
	zsbt_tid_add_items(rel, buf, newitems);
	/* zsbt_tid_replace_item unlocked 'buf' */
	ReleaseBuffer(buf);

	/* Return the TIDs to the caller */
	for (int i = 0; i < nitems; i++)
		tids[i] = tid + i;
}

TM_Result
zsbt_tid_delete(Relation rel, zstid tid,
			TransactionId xid, CommandId cid,
			Snapshot snapshot, Snapshot crosscheck, bool wait,
			TM_FailureData *hufd, bool changingPart)
{
	ZSUndoRecPtr recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
	ZSUndoRecPtr item_undoptr;
	bool		item_isdead;
	bool		found;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSTidArrayItem *deleteditem;
	Buffer		buf;
	zstid		next_tid;

	/* Find the item to delete. (It could be compressed) */
	found = zsbt_tid_fetch(rel, tid, &buf, &item_undoptr, &item_isdead);
	if (!found)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to delete with TID (%u, %u) in TID tree",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid));
	}
	if (item_isdead)
	{
		elog(ERROR, "cannot delete tuple that is already marked DEAD (%u, %u)",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid));
	}

	if (snapshot)
	{
		result = zs_SatisfiesUpdate(rel, snapshot, recent_oldest_undo,
									tid, item_undoptr, LockTupleExclusive,
									&keep_old_undo_ptr, hufd, &next_tid);
		if (result != TM_Ok)
		{
			UnlockReleaseBuffer(buf);
			/* FIXME: We should fill TM_FailureData *hufd correctly */
			return result;
		}

		if (crosscheck != InvalidSnapshot && result == TM_Ok)
		{
			/* Perform additional check for transaction-snapshot mode RI updates */
			/* FIXME: dummmy scan */
			ZSBtreeScan scan;
			TransactionId obsoleting_xid;

			memset(&scan, 0, sizeof(scan));
			scan.rel = rel;
			scan.snapshot = crosscheck;
			scan.recent_oldest_undo = recent_oldest_undo;

			if (!zs_SatisfiesVisibility(&scan, item_undoptr, &obsoleting_xid, NULL))
			{
				UnlockReleaseBuffer(buf);
				/* FIXME: We should fill TM_FailureData *hufd correctly */
				result = TM_Updated;
			}
		}
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Delete undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Delete);
		undorec.rec.type = ZSUNDO_TYPE_DELETE;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;
		undorec.changedPart = changingPart;

		if (keep_old_undo_ptr)
			undorec.rec.prevundorec = item_undoptr;
		else
			undorec.rec.prevundorec = InvalidUndoPtr;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with one with the new UNDO pointer. */
	deleteditem = zsbt_tid_create_item(tid, undorecptr, 1);

	zsbt_tid_replace_item(rel, buf, tid, deleteditem);
	ReleaseBuffer(buf); 	/* zsbt_tid_replace_item unlocked 'buf' */

	pfree(deleteditem);

	return TM_Ok;
}

void
zsbt_find_latest_tid(Relation rel, zstid *tid, Snapshot snapshot)
{
	ZSUndoRecPtr recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
	ZSUndoRecPtr item_undoptr;
	bool		item_isdead;
	bool		found;
	Buffer		buf;
	/* Just using meta attribute, we can follow the update chain */
	zstid curr_tid = *tid;

	for(;;)
	{
		zstid next_tid = InvalidZSTid;
		if (curr_tid == InvalidZSTid)
			break;

		/* Find the item */
		found = zsbt_tid_fetch(rel, curr_tid, &buf, &item_undoptr, &item_isdead);
		if (!found || item_isdead)
			break;

		if (snapshot)
		{
			/* FIXME: dummmy scan */
			ZSBtreeScan scan;
			TransactionId obsoleting_xid;

			memset(&scan, 0, sizeof(scan));
			scan.rel = rel;
			scan.snapshot = snapshot;
			scan.recent_oldest_undo = recent_oldest_undo;

			if (zs_SatisfiesVisibility(&scan, item_undoptr,
									   &obsoleting_xid, &next_tid))
			{
				*tid = curr_tid;
			}

			curr_tid = next_tid;
			UnlockReleaseBuffer(buf);
		}
	}
}

/*
 * A new TID is allocated, as we see best and returned to the caller. This
 * function is only called for META attribute btree. Data columns will use the
 * returned tid to insert new items.
 */
TM_Result
zsbt_tid_update(Relation rel, zstid otid,
				TransactionId xid, CommandId cid, bool key_update, Snapshot snapshot,
				Snapshot crosscheck, bool wait, TM_FailureData *hufd,
				zstid *newtid_p)
{
	TM_Result	result;
	ZSUndoRecPtr prevundoptr;

	/*
	 * This is currently only used on the meta-attribute. The other attributes
	 * don't need to carry visibility information, so the caller just inserts
	 * the new values with (multi_)insert() instead. This will change once we
	 * start doing the equivalent of HOT updates, where the TID doesn't change.
	 */
	Assert(*newtid_p == InvalidZSTid);

	/*
	 * Find and lock the old item.
	 *
	 * TODO: If there's free TID space left on the same page, we should keep the
	 * buffer locked, and use the same page for the new tuple.
	 */
	result = zsbt_tid_update_lock_old(rel, otid,
									  xid, cid, key_update, snapshot,
									  crosscheck, wait, hufd, &prevundoptr);

	if (result != TM_Ok)
		return result;

	/* insert new version */
	zsbt_tid_update_insert_new(rel, newtid_p, xid, cid, prevundoptr);

	/* update the old item with the "t_ctid pointer" for the new item */
	zsbt_tid_mark_old_updated(rel, otid, *newtid_p, xid, cid, key_update, snapshot);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): locks the old item for update.
 */
static TM_Result
zsbt_tid_update_lock_old(Relation rel, zstid otid,
					 TransactionId xid, CommandId cid, bool key_update, Snapshot snapshot,
					 Snapshot crosscheck, bool wait, TM_FailureData *hufd, ZSUndoRecPtr *prevundoptr_p)
{
	ZSUndoRecPtr recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
	Buffer		buf;
	ZSUndoRecPtr olditem_undoptr;
	bool		olditem_isdead;
	bool		found;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	zstid		next_tid;

	/*
	 * Find the item to delete.
	 */
	found = zsbt_tid_fetch(rel, otid, &buf, &olditem_undoptr, &olditem_isdead);
	if (!found || olditem_isdead)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) in TID tree",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid));
	}
	*prevundoptr_p = olditem_undoptr;

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(rel, snapshot, recent_oldest_undo,
								otid, olditem_undoptr,
								key_update ? LockTupleExclusive : LockTupleNoKeyExclusive,
								&keep_old_undo_ptr, hufd, &next_tid);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		/* FIXME: dummmy scan */
		ZSBtreeScan scan;
		TransactionId obsoleting_xid;

		memset(&scan, 0, sizeof(scan));
		scan.rel = rel;
		scan.snapshot = crosscheck;
		scan.recent_oldest_undo = recent_oldest_undo;

		if (!zs_SatisfiesVisibility(&scan, olditem_undoptr, &obsoleting_xid, NULL))
		{
			UnlockReleaseBuffer(buf);
			/* FIXME: We should fill TM_FailureData *hufd correctly */
			result = TM_Updated;
		}
	}

	/*
	 * TODO: tuple-locking not implemented. Pray that there is no competing
	 * concurrent update!
	 */

	UnlockReleaseBuffer(buf);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): inserts the new, updated, item.
 */
static void
zsbt_tid_update_insert_new(Relation rel,
					   zstid *newtid,
					   TransactionId xid, CommandId cid, ZSUndoRecPtr prevundoptr)
{
	zsbt_tid_multi_insert(rel, newtid, 1, xid, cid, INVALID_SPECULATIVE_TOKEN, prevundoptr);
}

/*
 * Subroutine of zsbt_update(): mark old item as updated.
 */
static void
zsbt_tid_mark_old_updated(Relation rel, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, bool key_update, Snapshot snapshot)
{
	ZSUndoRecPtr recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
	Buffer		buf;
	ZSUndoRecPtr olditem_undoptr;
	bool		olditem_isdead;
	bool		found;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	TM_FailureData tmfd;
	ZSUndoRecPtr undorecptr;
	ZSTidArrayItem *deleteditem;
	zstid		next_tid;

	/*
	 * Find the item to delete.  It could be part of a compressed item,
	 * we let zsbt_fetch() handle that.
	 */
	found = zsbt_tid_fetch(rel, otid, &buf, &olditem_undoptr, &olditem_isdead);
	if (!found || olditem_isdead)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) in TID tree",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid));
	}

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(rel, snapshot, recent_oldest_undo,
								otid, olditem_undoptr,
								key_update ? LockTupleExclusive : LockTupleNoKeyExclusive,
								&keep_old_undo_ptr, &tmfd, &next_tid);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "tuple concurrently updated - not implemented");
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Update undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Update);
		undorec.rec.type = ZSUNDO_TYPE_UPDATE;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = otid;
		if (keep_old_undo_ptr)
			undorec.rec.prevundorec = olditem_undoptr;
		else
			undorec.rec.prevundorec = InvalidUndoPtr;
		undorec.newtid = newtid;
		undorec.key_update = key_update;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with one with the updated undo pointer. */
	deleteditem = zsbt_tid_create_item(otid, undorecptr, 1);

	zsbt_tid_replace_item(rel, buf, otid, deleteditem);
	ReleaseBuffer(buf);		/* zsbt_recompress_replace released */

	pfree(deleteditem);
}

TM_Result
zsbt_tid_lock(Relation rel, zstid tid,
			   TransactionId xid, CommandId cid,
			   LockTupleMode mode, Snapshot snapshot,
			   TM_FailureData *hufd, zstid *next_tid)
{
	ZSUndoRecPtr recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);
	Buffer		buf;
	ZSUndoRecPtr item_undoptr;
	bool		item_isdead;
	bool		found;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSTidArrayItem *newitem;

	*next_tid = tid;

	/* Find the item to delete. (It could be compressed) */
	found = zsbt_tid_fetch(rel, tid, &buf, &item_undoptr, &item_isdead);
	if (!found || item_isdead)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to lock with TID (%u, %u)",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid));
	}
	result = zs_SatisfiesUpdate(rel, snapshot, recent_oldest_undo,
								tid, item_undoptr, mode,
								&keep_old_undo_ptr, hufd, next_tid);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		return result;
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_TupleLock undorec;

		undorec.rec.size = sizeof(ZSUndoRec_TupleLock);
		undorec.rec.type = ZSUNDO_TYPE_TUPLE_LOCK;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;
		undorec.lockmode = mode;
		if (keep_old_undo_ptr)
			undorec.rec.prevundorec = item_undoptr;
		else
			undorec.rec.prevundorec = InvalidUndoPtr;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the item with an identical one, but with updated undo pointer. */
	newitem = zsbt_tid_create_item(tid, undorecptr, 1);

	zsbt_tid_replace_item(rel, buf, tid, newitem);
	ReleaseBuffer(buf); 	/* zsbt_tid_replace_item unlocked 'buf' */

	pfree(newitem);

	return TM_Ok;
}

/*
 * Collect all TIDs marked as dead in the TID tree.
 *
 * This is used during VACUUM.
 */
IntegerSet *
zsbt_collect_dead_tids(Relation rel, zstid starttid, zstid *endtid)
{
	Buffer		buf = InvalidBuffer;
	IntegerSet *result;
	ZSBtreePageOpaque *opaque;
	zstid		nexttid;
	BlockNumber	nextblock;

	result = intset_create();

	nexttid = starttid;
	nextblock = InvalidBlockNumber;
	for (;;)
	{
		ZSTidArrayItem *tiditems;
		int			ntiditems;
		Page		page;

		if (nextblock != InvalidBlockNumber)
		{
			buf = ReleaseAndReadBuffer(buf, rel, nextblock);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);

			if (!zsbt_page_is_expected(rel, ZS_META_ATTRIBUTE_NUM, nexttid, 0, buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
		}

		if (!BufferIsValid(buf))
		{
			buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, nexttid, 0, true);
			if (!BufferIsValid(buf))
				return result;
			page = BufferGetPage(buf);
		}

		tiditems = PageGetZSTidArray(page);
		ntiditems = PageGetNumZSTidItems(page);
		for (int i = 0; i < ntiditems; i++)
		{
			ZSTidArrayItem *item = &tiditems[i];

			if ((item->t_flags & ZSBT_TID_DEAD) != 0)
			{
				for (int j = 0; j < item->t_nelements; j++)
					intset_add_member(result, item->t_tid + j);
			}
		}

		opaque = ZSBtreePageGetOpaque(page);
		nexttid = opaque->zs_hikey;
		nextblock = opaque->zs_next;

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (nexttid == MaxPlusOneZSTid)
		{
			Assert(nextblock == InvalidBlockNumber);
			break;
		}

		if (intset_memory_usage(result) > (uint64) maintenance_work_mem * 1024)
			break;
	}

	if (BufferIsValid(buf))
		ReleaseBuffer(buf);

	*endtid = nexttid;
	return result;
}

/*
 * Mark item with given TID as dead.
 *
 * This is used when UNDO actions are performed, after a transaction becomes
 * old enough.
 */
void
zsbt_tid_mark_dead(Relation rel, zstid tid)
{
	Buffer		buf;
	ZSUndoRecPtr item_undoptr;
	bool		found;
	bool		isdead;
	ZSTidArrayItem deaditem;

	/* Find the item to delete. (It could be compressed) */
	found = zsbt_tid_fetch(rel, tid, &buf, &item_undoptr, &isdead);
	if (!found)
	{
		elog(WARNING, "could not find tuple to mark dead with TID (%u, %u)",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid));
		return;
	}

	/* Replace the ZSBreeItem with a DEAD item. (Unless it's already dead) */
	if (isdead)
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	memset(&deaditem, 0, sizeof(ZSTidArrayItem));
	deaditem.t_tid = tid;
	deaditem.t_flags = ZSBT_TID_DEAD;
	deaditem.t_undo_ptr = InvalidUndoPtr;
	deaditem.t_nelements = 1;

	zsbt_tid_replace_item(rel, buf, tid, &deaditem);
	ReleaseBuffer(buf); 	/* zsbt_tid_replace_item unlocked 'buf' */
}


/*
 * Remove items for the given TIDs from the TID tree.
 *
 * This is used during VACUUM.
 */
void
zsbt_tid_remove(Relation rel, IntegerSet *tids)
{
	zstid		nexttid;

	intset_begin_iterate(tids);
	if (!intset_iterate_next(tids, &nexttid))
		nexttid = MaxZSTid;

	while (nexttid < MaxZSTid)
	{
		Buffer		buf;
		Page		page;
		List	   *newitems;
		ZSTidArrayItem *tiditems;
		int			ntiditems;
		int			i;

		/*
		 * Find the leaf page containing the next item to remove
		 */
		buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, nexttid, 0, false);
		page = BufferGetPage(buf);

		/*
		 * Rewrite the items on the page, removing all TIDs that need to be
		 * removed from the page.
		 */
		tiditems = PageGetZSTidArray(page);
		ntiditems = PageGetNumZSTidItems(page);
		newitems = NIL;

		for (i = 0; i < ntiditems; i++)
		{
			ZSTidArrayItem *item = &tiditems[i];
			zstid		old_firsttid = item->t_tid;
			int			old_nelements = item->t_nelements;

			if (item->t_tid <= nexttid && nexttid < old_firsttid + old_nelements)
			{
				while (old_nelements > 0)
				{
					/* skip any to-be-removed items from the beginning. */
					while (old_nelements > 0 && old_firsttid == nexttid)
					{
						old_firsttid++;
						old_nelements--;
						if (!intset_iterate_next(tids, &nexttid))
							nexttid = MaxZSTid;
					}

					if (old_nelements > 0)
					{
						ZSTidArrayItem *newitem;
						zstid		endtid;
						int			new_nelements;

						/* add as many TIDs as we can to this item */
						endtid = Min(old_firsttid + old_nelements, nexttid);
						new_nelements = endtid - old_firsttid;

						newitem = zsbt_tid_create_item(old_firsttid, item->t_undo_ptr, new_nelements);
						newitem->t_flags = item->t_flags;
						newitems = lappend(newitems, newitem);

						old_firsttid += new_nelements;
						old_nelements -= new_nelements;
					}
				}
			}
			else
			{
				/* keep this item unmodified */
				newitems = lappend(newitems, item);
			}
		}

		/* Pass the list to the recompressor. */
		IncrBufferRefCount(buf);
		if (newitems)
		{
			zsbt_tid_recompress_replace(rel, buf, newitems);
		}
		else
		{
			zs_split_stack *stack;

			stack = zsbt_unlink_page(rel, ZS_META_ATTRIBUTE_NUM, buf, 0);

			if (!stack)
			{
				/* failed. */
				Page		newpage = PageGetTempPageCopySpecial(BufferGetPage(buf));

				stack = zs_new_split_stack_entry(buf, newpage);
			}

			/* apply the changes */
			zs_apply_split_changes(rel, stack);
		}

		list_free(newitems);

		ReleaseBuffer(buf);
	}
}

/*
 * Clear an item's UNDO pointer.
 *
 * This is used during VACUUM, to clear out aborted deletions.
 */
void
zsbt_tid_undo_deletion(Relation rel, zstid tid, ZSUndoRecPtr undoptr)
{
	Buffer		buf;
	ZSUndoRecPtr item_undoptr;
	bool		found;
	ZSTidArrayItem *copy;

	/* Find the item to delete. (It could be compressed) */
	found = zsbt_tid_fetch(rel, tid, &buf, &item_undoptr, NULL);
	if (!found)
	{
		elog(WARNING, "could not find aborted tuple to remove with TID (%u, %u)",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid));
		return;
	}

	if (ZSUndoRecPtrEquals(item_undoptr, undoptr))
	{
		copy = zsbt_tid_create_item(tid, InvalidUndoPtr, 1);
		zsbt_tid_replace_item(rel, buf, tid, copy);
		ReleaseBuffer(buf); 	/* zsbt_tid_replace_item unlocked 'buf' */
	}
	else
	{
		Assert(item_undoptr.counter > undoptr.counter ||
			   !IsZSUndoRecPtrValid(&item_undoptr));
		UnlockReleaseBuffer(buf);
	}
}

/* ----------------------------------------------------------------
 *						 Internal routines
 * ----------------------------------------------------------------
 */

void
zsbt_tid_clear_speculative_token(Relation rel, zstid tid, uint32 spectoken, bool forcomplete)
{
	Buffer		buf;
	ZSUndoRecPtr item_undoptr;
	bool		item_isdead;
	bool		found;

	found = zsbt_tid_fetch(rel, tid, &buf, &item_undoptr, &item_isdead);
	if (!found || item_isdead)
		elog(ERROR, "couldn't find item for meta column for inserted tuple with TID (%u, %u) in rel %s",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), rel->rd_rel->relname.data);

	zsundo_clear_speculative_token(rel, item_undoptr);

	UnlockReleaseBuffer(buf);
}

/*
 * Fetch the item with given TID. The page containing the item is kept locked, and
 * returned to the caller in *buf_p. This is used to locate a tuple for updating
 * or deleting it.
 */
static bool
zsbt_tid_fetch(Relation rel, zstid tid, Buffer *buf_p, ZSUndoRecPtr *undoptr_p, bool *isdead_p)
{
	Buffer		buf;
	Page		page;
	ZSTidArrayItem *item = NULL;
	bool		found = false;
	int			ntiditems;
	ZSTidArrayItem *tiditems;

	buf = zsbt_descend(rel, ZS_META_ATTRIBUTE_NUM, tid, 0, false);
	if (buf == InvalidBuffer)
	{
		*buf_p = InvalidBuffer;
		*undoptr_p = InvalidUndoPtr;
		return false;
	}
	page = BufferGetPage(buf);

	/* Find the item on the page that covers the target TID */
	ntiditems = PageGetNumZSTidItems(page);
	tiditems = PageGetZSTidArray(page);
	for (int i = 0; i < ntiditems; i++)
	{
		item = &tiditems[i];

		if (item->t_tid <= tid && item->t_tid + item->t_nelements > tid)
		{
			found = true;
			break;
		}
	}

	if (found)
	{
		*undoptr_p = item->t_undo_ptr;
		*buf_p = buf;
		if (isdead_p)
			*isdead_p = (item->t_flags & ZSBT_TID_DEAD) != 0;
		return true;
	}
	else
	{
		UnlockReleaseBuffer(buf);
		*buf_p = InvalidBuffer;
		return false;
	}
}

/*
 * Form a ZSTidArrayItem for the 'nelements' consecutive TIDs, starting with 'tid'.
 */
static ZSTidArrayItem *
zsbt_tid_create_item(zstid tid, ZSUndoRecPtr undo_ptr, int nelements)
{
	ZSTidArrayItem *newitem;
	Size		itemsz;

	Assert(nelements > 0);

	itemsz = sizeof(ZSTidArrayItem);

	newitem = palloc(itemsz);
	newitem->t_tid = tid;
	newitem->t_flags = 0;
	newitem->t_nelements = nelements;
	newitem->t_undo_ptr = undo_ptr;

	return newitem;
}

/*
 * This helper function is used to implement INSERT.
 *
 * The items in 'newitems' are added to the page, to the correct position.
 * FIXME: Actually, they're always just added to the end of the page, and that
 * better be the correct position.
 *
 * This function handles splitting the page if needed.
 */
static void
zsbt_tid_add_items(Relation rel, Buffer buf, List *newitems)
{
	Page		page = BufferGetPage(buf);
	ZSTidArrayItem *tiditems;
	int			ntiditems;

	Size		newitemsize;
	ListCell   *lc;

	tiditems = PageGetZSTidArray(page);
	ntiditems = PageGetNumZSTidItems(page);

	newitemsize = list_length(newitems) * sizeof(ZSTidArrayItem);
	if (newitemsize <= PageGetExactFreeSpace(page))
	{
		/* The new items fit on the page. Add them. */

		START_CRIT_SECTION();

		foreach(lc, newitems)
		{
			ZSTidArrayItem *item = (ZSTidArrayItem *) lfirst(lc);

			tiditems[ntiditems++] = *item;
		}
		((PageHeader) page)->pd_lower += newitemsize;

		MarkBufferDirty(buf);

		/* TODO: WAL-log */

		END_CRIT_SECTION();

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		List	   *items = NIL;

		/* Loop through all old items on the page */
		for (int i = 0; i < ntiditems; i++)
		{
			ZSTidArrayItem *item = &tiditems[i];

			/*
			 * Get the next item to process from the page.
			 */
			items = lappend(items, item);
		}

		/* Add any new items to the end */
		if (newitems)
			items = list_concat(items, newitems);

		/* Now pass the list to the recompressor. */
		IncrBufferRefCount(buf);
		if (items)
		{
			zsbt_tid_recompress_replace(rel, buf, items);
		}
		else
		{
			zs_split_stack *stack;

			stack = zsbt_unlink_page(rel, ZS_META_ATTRIBUTE_NUM, buf, 0);

			if (!stack)
			{
				/* failed. */
				Page		newpage = PageGetTempPageCopySpecial(BufferGetPage(buf));

				stack = zs_new_split_stack_entry(buf, newpage);
			}

			/* apply the changes */
			zs_apply_split_changes(rel, stack);
		}

		list_free(items);
	}
}


/*
 * This helper function is used to implement INSERT, UPDATE and DELETE.
 *
 * If 'olditem' is not NULL, then 'olditem' on the page is replaced with
 * 'replacementitem'. 'replacementitem' can be NULL, to remove an old item.
 *
 * If 'newitems' is not empty, the items in the list are added to the page,
 * to the correct position. FIXME: Actually, they're always just added to
 * the end of the page, and that better be the correct position.
 *
 * This function handles decompressing and recompressing items, and splitting
 * the page if needed.
 */
static void
zsbt_tid_replace_item(Relation rel, Buffer buf, zstid oldtid, ZSTidArrayItem *replacementitem)
{
	Page		page = BufferGetPage(buf);
	ZSTidArrayItem *tiditems;
	int			ntiditems;
	ZSTidArrayItem *olditem;
	int			itemno;

	Assert(!replacementitem || (replacementitem->t_tid == oldtid && replacementitem->t_nelements == 1));

	if (replacementitem)
		Assert(replacementitem->t_tid == oldtid);

	/*
	 * Find the item that covers the given tid.
	 */
	tiditems = PageGetZSTidArray(page);
	ntiditems = PageGetNumZSTidItems(page);

	itemno = zsbt_binsrch_tidpage(oldtid, tiditems, ntiditems);
	if (itemno < 0)
		elog(ERROR, "could not find item to replace for tid (%u, %u)",
			 ZSTidGetBlockNumber(oldtid), ZSTidGetOffsetNumber(oldtid));
	olditem = &tiditems[itemno];

	if (oldtid < olditem->t_tid || oldtid >= olditem->t_tid + olditem->t_nelements)
		elog(ERROR, "could not find item to replace for tid (%u, %u)",
			 ZSTidGetBlockNumber(oldtid), ZSTidGetOffsetNumber(oldtid));

	{
		/*
		 * The target TID might be part of an array item. We have to split
		 * the array item into two, and put the replacement item in the middle.
		 */
		int			cutoff;
		int			nelements = olditem->t_nelements;
		ZSTidArrayItem *item_before = NULL;
		ZSTidArrayItem *item_after = NULL;
		int			n_replacements = -1;

		cutoff = oldtid - olditem->t_tid;

		/* Array slice before the target TID */
		if (cutoff > 0)
		{
			item_before = zsbt_tid_create_item(olditem->t_tid, olditem->t_undo_ptr,
											   cutoff);
			n_replacements++;
		}

		if (replacementitem)
			n_replacements++;

		/* Array slice after the target */
		if (cutoff + 1 < nelements)
		{
			item_after = zsbt_tid_create_item(oldtid + 1, olditem->t_undo_ptr,
											  nelements - (cutoff + 1));
			n_replacements++;
		}

		/* Can we fit them? */
		if (n_replacements * sizeof(ZSTidArrayItem) <= PageGetExactFreeSpace(page))
		{
			ZSTidArrayItem *dstitem = olditem;

			START_CRIT_SECTION();

			/* move existing items */
			if (n_replacements == -1)
				memmove(olditem, olditem + n_replacements,
						(ntiditems - itemno - 1) * sizeof(ZSTidArrayItem));
			else
			{
				memmove(olditem + n_replacements + 1, olditem + 1,
						(ntiditems - itemno - 1) * sizeof(ZSTidArrayItem));

				if (item_before)
					*(dstitem++) = *item_before;
				if (replacementitem)
					*(dstitem++) = *replacementitem;
				if (item_after)
					*(dstitem++) = *item_after;
			}

			((PageHeader) page)->pd_lower += n_replacements * sizeof(ZSTidArrayItem);

			MarkBufferDirty(buf);
			/* TODO: WAL-log */
			END_CRIT_SECTION();

			{
				zstid lasttid = 0;
				ntiditems = PageGetNumZSTidItems(page);
				for (int i = 0; i < ntiditems; i++)
				{
					Assert(tiditems[i].t_tid > lasttid);
					lasttid = zsbt_tid_item_lasttid(&tiditems[i]);
				}
			}

			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		else
		{
			/* Have to split the page. */
			List	   *items = NIL;
			int			i;

			for (i = 0; i < itemno; i++)
				items = lappend(items, &tiditems[i]);

			if (item_before)
				items = lappend(items, item_before);
			if (replacementitem)
				items = lappend(items, replacementitem);
			if (item_after)
				items = lappend(items, item_after);

			i++;
			for (; i < ntiditems; i++)
				items = lappend(items, &tiditems[i]);



			{
				zstid lasttid = 0;
				ListCell *lc;
				ntiditems = PageGetNumZSTidItems(page);
				foreach (lc, items)
				{
					ZSTidArrayItem *i = (ZSTidArrayItem *) lfirst(lc);
					Assert(i->t_tid > lasttid);
					lasttid = zsbt_tid_item_lasttid(i);
				}
			}

			/* Pass the list to the recompressor. */
			IncrBufferRefCount(buf);
			if (items)
			{
				zsbt_tid_recompress_replace(rel, buf, items);
			}
			else
			{
				zs_split_stack *stack;

				stack = zsbt_unlink_page(rel, ZS_META_ATTRIBUTE_NUM, buf, 0);

				if (!stack)
				{
					/* failed. */
					Page		newpage = PageGetTempPageCopySpecial(BufferGetPage(buf));

					stack = zs_new_split_stack_entry(buf, newpage);
				}

				/* apply the changes */
				zs_apply_split_changes(rel, stack);
			}

			list_free(items);
		}
	}
}

/*
 * Recompressor routines
 */
typedef struct
{
	Page		currpage;

	/* first page writes over the old buffer, subsequent pages get newly-allocated buffers */
	zs_split_stack *stack_head;
	zs_split_stack *stack_tail;

	int			num_pages;
	int			free_space_per_page;

	zstid		hikey;
} zsbt_tid_recompress_context;

static void
zsbt_tid_recompress_newpage(zsbt_tid_recompress_context *cxt, zstid nexttid, int flags)
{
	Page		newpage;
	ZSBtreePageOpaque *newopaque;
	zs_split_stack *stack;

	if (cxt->currpage)
	{
		/* set the last tid on previous page */
		ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(cxt->currpage);

		oldopaque->zs_hikey = nexttid;
	}

	newpage = (Page) palloc(BLCKSZ);
	PageInit(newpage, BLCKSZ, sizeof(ZSBtreePageOpaque));

	stack = zs_new_split_stack_entry(InvalidBuffer, /* will be assigned later */
									 newpage);
	if (cxt->stack_tail)
		cxt->stack_tail->next = stack;
	else
		cxt->stack_head = stack;
	cxt->stack_tail = stack;

	cxt->currpage = newpage;

	newopaque = ZSBtreePageGetOpaque(newpage);
	newopaque->zs_attno = ZS_META_ATTRIBUTE_NUM;
	newopaque->zs_next = InvalidBlockNumber; /* filled in later */
	newopaque->zs_lokey = nexttid;
	newopaque->zs_hikey = cxt->hikey;		/* overwritten later, if this is not last page */
	newopaque->zs_level = 0;
	newopaque->zs_flags = flags;
	newopaque->zs_page_id = ZS_BTREE_PAGE_ID;
}

static void
zsbt_tid_recompress_add_to_page(zsbt_tid_recompress_context *cxt, ZSTidArrayItem *item)
{
	ZSTidArrayItem *tiditems;
	int			ntiditems;
	Size		freespc;

	freespc = PageGetExactFreeSpace(cxt->currpage);
	if (freespc < MAXALIGN(sizeof(ZSTidArrayItem)) ||
		freespc < cxt->free_space_per_page)
	{
		zsbt_tid_recompress_newpage(cxt, item->t_tid, 0);
	}

	tiditems = PageGetZSTidArray(cxt->currpage);
	ntiditems = PageGetNumZSTidItems(cxt->currpage);

	tiditems[ntiditems] = *item;
	((PageHeader) cxt->currpage)->pd_lower += sizeof(ZSTidArrayItem);
}

/*
 * Subroutine of zsbt_tid_recompress_replace.  Compute how much space the
 * items will take, and compute how many pages will be needed for them, and
 * decide how to distribute any free space thats's left over among the
 * pages.
 *
 * Like in B-tree indexes, we aim for 50/50 splits, except for the
 * rightmost page where aim for 90/10, so that most of the free space is
 * left to the end of the index, where it's useful for new inserts. The
 * 90/10 splits ensure that the we don't waste too much space on a table
 * that's loaded at the end, and never updated.
 */
static void
zsbt_tid_recompress_picksplit(zsbt_tid_recompress_context *cxt, List *items)
{
	int			total_items = list_length(items);
	size_t		total_sz;
	int			num_pages;
	int			space_on_empty_page;
	Size		free_space_per_page;

	space_on_empty_page = BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(ZSBtreePageOpaque));

	/* Compute total space needed for all the items. */
	total_sz = total_items * sizeof(ZSTidArrayItem);

	/* How many pages will we need for them? */
	num_pages = (total_sz + space_on_empty_page - 1) / space_on_empty_page;

	/* If everything fits on one page, don't split */
	if (num_pages == 1)
	{
		free_space_per_page = 0;
	}
	/* If this is the rightmost page, do a 90/10 split */
	else if (cxt->hikey == MaxPlusOneZSTid)
	{
		/*
		 * What does 90/10 mean if we have to use more than two pages? It means
		 * that 10% of the items go to the last page, and 90% are distributed to
		 * all the others.
		 */
		double		total_free_space;

		total_free_space = space_on_empty_page * num_pages - total_sz;

		free_space_per_page = total_free_space * 0.1 / (num_pages - 1);
	}
	/* Otherwise, aim for an even 50/50 split */
	else
	{
		free_space_per_page = (space_on_empty_page * num_pages - total_sz) / num_pages;
	}

	cxt->num_pages = num_pages;
	cxt->free_space_per_page = free_space_per_page;
}

/*
 * Rewrite a leaf page, with given 'items' as the new content.
 *
 * If there are any uncompressed items in the list, we try to compress them.
 * Any already-compressed items are added as is.
 *
 * If the items no longer fit on the page, then the page is split. It is
 * entirely possible that they don't fit even on two pages; we split the page
 * into as many pages as needed. Hopefully not more than a few pages, though,
 * because otherwise you might hit limits on the number of buffer pins (with
 * tiny shared_buffers).
 *
 * On entry, 'oldbuf' must be pinned and exclusive-locked. On exit, the lock
 * is released, but it's still pinned.
 *
 * TODO: Try to combine single items, and existing array-items, into new array
 * items.
 */
static void
zsbt_tid_recompress_replace(Relation rel, Buffer oldbuf, List *items)
{
	ListCell   *lc;
	zsbt_tid_recompress_context cxt;
	ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(BufferGetPage(oldbuf));
	BlockNumber orignextblk;
	zs_split_stack *stack;
	List	   *downlinks = NIL;

	orignextblk = oldopaque->zs_next;

	cxt.currpage = NULL;
	cxt.stack_head = cxt.stack_tail = NULL;
	cxt.hikey = oldopaque->zs_hikey;

	zsbt_tid_recompress_picksplit(&cxt, items);
	zsbt_tid_recompress_newpage(&cxt, oldopaque->zs_lokey, (oldopaque->zs_flags & ZSBT_ROOT));

	foreach(lc, items)
	{
		ZSTidArrayItem *item = (ZSTidArrayItem *) lfirst(lc);

		/* Store it uncompressed */
		zsbt_tid_recompress_add_to_page(&cxt, item);
	}

	/*
	 * Ok, we now have a list of pages, to replace the original page, as private
	 * in-memory copies. Allocate buffers for them, and write them out.
	 *
	 * allocate all the pages before entering critical section, so that
	 * out-of-disk-space doesn't lead to PANIC
	 */
	stack = cxt.stack_head;
	Assert(stack->buf == InvalidBuffer);
	stack->buf = oldbuf;
	while (stack->next)
	{
		Page	thispage = stack->page;
		ZSBtreePageOpaque *thisopaque = ZSBtreePageGetOpaque(thispage);
		ZSBtreeInternalPageItem *downlink;
		Buffer	nextbuf;

		Assert(stack->next->buf == InvalidBuffer);

		nextbuf = zspage_getnewbuf(rel, InvalidBuffer);
		stack->next->buf = nextbuf;

		thisopaque->zs_next = BufferGetBlockNumber(nextbuf);

		downlink = palloc(sizeof(ZSBtreeInternalPageItem));
		downlink->tid = thisopaque->zs_hikey;
		downlink->childblk = BufferGetBlockNumber(nextbuf);
		downlinks = lappend(downlinks, downlink);

		stack = stack->next;
	}
	/* last one in the chain */
	ZSBtreePageGetOpaque(stack->page)->zs_next = orignextblk;

	/*
	 * zsbt_tid_recompress_picksplit() calculated that we'd need
	 * 'cxt.num_pages' pages. Check that it matches with how many pages we
	 * actually created.
	 */
	Assert(list_length(downlinks) + 1 == cxt.num_pages);

	/* If we had to split, insert downlinks for the new pages. */
	if (cxt.stack_head->next)
	{
		oldopaque = ZSBtreePageGetOpaque(cxt.stack_head->page);

		if ((oldopaque->zs_flags & ZSBT_ROOT) != 0)
		{
			ZSBtreeInternalPageItem *downlink;

			downlink = palloc(sizeof(ZSBtreeInternalPageItem));
			downlink->tid = MinZSTid;
			downlink->childblk = BufferGetBlockNumber(cxt.stack_head->buf);
			downlinks = lcons(downlink, downlinks);

			cxt.stack_tail->next = zsbt_newroot(rel, ZS_META_ATTRIBUTE_NUM,
												oldopaque->zs_level + 1, downlinks);

			/* clear the ZSBT_ROOT flag on the old root page */
			oldopaque->zs_flags &= ~ZSBT_ROOT;
		}
		else
		{
			cxt.stack_tail->next = zsbt_insert_downlinks(rel, ZS_META_ATTRIBUTE_NUM,
														 oldopaque->zs_lokey, BufferGetBlockNumber(oldbuf), oldopaque->zs_level + 1,
														 downlinks);
		}
		/* note: stack_tail is not the real tail anymore */
	}

	/* Finally, overwrite all the pages we had to modify */
	zs_apply_split_changes(rel, cxt.stack_head);
}

static int
zsbt_binsrch_tidpage(zstid key, ZSTidArrayItem *arr, int arr_elems)
{
	int			low,
		high,
		mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (key >= arr[mid].t_tid)
			low = mid + 1;
		else
			high = mid;
	}
	return low - 1;
}
