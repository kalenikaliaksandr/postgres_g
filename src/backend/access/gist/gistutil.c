/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  utilities routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gist/gistutil.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist_private.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_opclass.h"
#include "common/pg_prng.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Write itup vector to page, has no control of free space.
 */
void
gistfillbuffer(Page page, IndexTuple *itup, int len, OffsetNumber off)
{
	int			i;

	if (off == InvalidOffsetNumber)
		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

	for (i = 0; i < len; i++)
	{
		Size		sz = IndexTupleSize(itup[i]);
		OffsetNumber l;

		l = PageAddItem(page, (Item) itup[i], sz, off, false, false);
		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to GiST index page, item %d out of %d, size %d bytes",
				 i, len, (int) sz);
		off++;
	}
}

/*
 * Check space for itup vector on page
 */
bool
gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace)
{
	unsigned int size = freespace,
				deleted = 0;
	int			i;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	if (todelete != InvalidOffsetNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, todelete));

		deleted = IndexTupleSize(itup) + sizeof(ItemIdData);
	}

	return (PageGetFreeSpace(page) + deleted < size);
}

bool
gistfitpage(IndexTuple *itvec, int len)
{
	int			i;
	Size		size = 0;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	/* TODO: Consider fillfactor */
	return (size <= GiSTPageSize);
}

/*
 * Read buffer into itup vector
 */
IndexTuple *
gistextractpage(Page page, int *len /* out */ )
{
	OffsetNumber i,
				maxoff;
	IndexTuple *itvec;

	maxoff = PageGetMaxOffsetNumber(page);
	*len = maxoff;
	itvec = palloc(sizeof(IndexTuple) * maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		itvec[i - FirstOffsetNumber] = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

	return itvec;
}

/*
 * join two vectors into one
 */
IndexTuple *
gistjoinvector(IndexTuple *itvec, int *len, IndexTuple *additvec, int addlen)
{
	itvec = (IndexTuple *) repalloc((void *) itvec, sizeof(IndexTuple) * ((*len) + addlen));
	memmove(&itvec[*len], additvec, sizeof(IndexTuple) * addlen);
	*len += addlen;
	return itvec;
}

/*
 * make plain IndexTuple vector
 */

IndexTupleData *
gistfillitupvec(IndexTuple *vec, int veclen, int *memlen)
{
	char	   *ptr,
			   *ret;
	int			i;

	*memlen = 0;

	for (i = 0; i < veclen; i++)
		*memlen += IndexTupleSize(vec[i]);

	ptr = ret = palloc(*memlen);

	for (i = 0; i < veclen; i++)
	{
		memcpy(ptr, vec[i], IndexTupleSize(vec[i]));
		ptr += IndexTupleSize(vec[i]);
	}

	return (IndexTupleData *) ret;
}

/*
 * Make unions of keys in IndexTuple vector (one union datum per index column).
 * Union Datums are returned into the attr/isnull arrays.
 * Resulting Datums aren't compressed.
 */
void
gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
				   Datum *attr, bool *isnull)
{
	int			i;
	GistEntryVector *evec;
	int			attrsize;

	evec = (GistEntryVector *) palloc((len + 2) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = 0; i < giststate->nonLeafTupdesc->natts; i++)
	{
		int			j;

		/* Collect non-null datums for this column */
		evec->n = 0;
		for (j = 0; j < len; j++)
		{
			Datum		datum;
			bool		IsNull;

			datum = index_getattr(itvec[j], i + 1, giststate->leafTupdesc,
								  &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   evec->vector + evec->n,
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   false, IsNull);
			evec->n++;
		}

		/* If this column was all NULLs, the union is NULL */
		if (evec->n == 0)
		{
			attr[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			if (evec->n == 1)
			{
				/* unionFn may expect at least two inputs */
				evec->n = 2;
				evec->vector[1] = evec->vector[0];
			}

			/* Make union and store in attr array */
			attr[i] = FunctionCall2Coll(&giststate->unionFn[i],
										giststate->supportCollation[i],
										PointerGetDatum(evec),
										PointerGetDatum(&attrsize));

			isnull[i] = false;
		}
	}
}

/*
 * Return an IndexTuple containing the result of applying the "union"
 * method to the specified IndexTuple vector.
 */
IndexTuple
gistunion(Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate)
{
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	gistMakeUnionItVec(giststate, itvec, len, attr, isnull);

	return gistFormTuple(giststate, r, attr, isnull, false);
}

/*
 * makes union of two key
 */
void
gistMakeUnionKey(GISTSTATE *giststate, int attno,
				 GISTENTRY *entry1, bool isnull1,
				 GISTENTRY *entry2, bool isnull2,
				 Datum *dst, bool *dstisnull)
{
	/* we need a GistEntryVector with room for exactly 2 elements */
	union
	{
		GistEntryVector gev;
		char		padding[2 * sizeof(GISTENTRY) + GEVHDRSZ];
	}			storage;
	GistEntryVector *evec = &storage.gev;
	int			dstsize;

	evec->n = 2;

	if (isnull1 && isnull2)
	{
		*dstisnull = true;
		*dst = (Datum) 0;
	}
	else
	{
		if (isnull1 == false && isnull2 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry2;
		}
		else if (isnull1 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry1;
		}
		else
		{
			evec->vector[0] = *entry2;
			evec->vector[1] = *entry2;
		}

		*dstisnull = false;
		*dst = FunctionCall2Coll(&giststate->unionFn[attno],
								 giststate->supportCollation[attno],
								 PointerGetDatum(evec),
								 PointerGetDatum(&dstsize));
	}
}

bool
gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b)
{
	bool		result;

	FunctionCall3Coll(&giststate->equalFn[attno],
					  giststate->supportCollation[attno],
					  a, b,
					  PointerGetDatum(&result));
	return result;
}

/*
 * Decompress all keys in tuple
 */
void
gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
				  OffsetNumber o, GISTENTRY *attdata, bool *isnull)
{
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);
		gistdentryinit(giststate, i, &attdata[i],
					   datum, r, p, o,
					   false, isnull[i]);
	}
}

/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	bool		neednew = false;
	GISTENTRY	oldentries[INDEX_MAX_KEYS],
				addentries[INDEX_MAX_KEYS];
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	int			i;

	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldentries, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addentries, addisnull);

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		gistMakeUnionKey(giststate, i,
						 oldentries + i, oldisnull[i],
						 addentries + i, addisnull[i],
						 attr + i, isnull + i);

		if (neednew)
			/* we already need new key, so we can skip check */
			continue;

		if (isnull[i])
			/* union of key may be NULL if and only if both keys are NULL */
			continue;

		if (!addisnull[i])
		{
			if (oldisnull[i] ||
				!gistKeyIsEQ(giststate, i, oldentries[i].key, attr[i]))
				neednew = true;
		}
	}

	if (neednew)
	{
		/* need to update key */
		newtup = gistFormTuple(giststate, r, attr, isnull, false);
		newtup->t_tid = oldtup->t_tid;
	}

	return newtup;
}

OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it has compressed entry */
		   GISTSTATE *giststate)
{
	OffsetNumber result;
	OffsetNumber maxoff;
	OffsetNumber i;
	GistEntryVector *evec;
	GISTENTRY identry[INDEX_MAX_KEYS];
	bool isnull[INDEX_MAX_KEYS];

	Assert(!GistPageIsLeaf(p));

	gistDeCompressAtt(giststate, r,
					  it, NULL, (OffsetNumber) 0,
					  identry, isnull);

	/* we'll return FirstOffsetNumber if page is empty (shouldn't happen) */
	result = FirstOffsetNumber;

	/*
	 * Loop over tuples on page.
	 */
	maxoff = PageGetMaxOffsetNumber(p);
	Assert(maxoff >= FirstOffsetNumber);

	evec = (GistEntryVector *) palloc((maxoff + 2) * sizeof(GISTENTRY) * GEVHDRSZ);
	evec->n = 0;
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		Datum datum;
		bool isNull;
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));

		datum = index_getattr(itup, 1, giststate->leafTupdesc, &isNull);
		if (isNull)
			continue;
		
		gistdentryinit(giststate, 0,
						evec->vector + evec->n,
						datum,
						r, p, (OffsetNumber) i,
						false, isNull);
		evec->n++;
	}

	if (evec->n == 0)
		return result;

	FunctionCall3Coll(&giststate->choosesubtreeFn[0],
								giststate->supportCollation[0],
								PointerGetDatum(evec),
								PointerGetDatum(&(identry[0])),
								PointerGetDatum(&result));

	return result;
}

/*
 * initialize a GiST entry with a decompressed version of key
 */
void
gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   bool l, bool isNull)
{
	if (!isNull)
	{
		GISTENTRY  *dep;

		gistentryinit(*e, k, r, pg, o, l);

		/* there may not be a decompress function in opclass */
		if (!OidIsValid(giststate->decompressFn[nkey].fn_oid))
			return;

		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1Coll(&giststate->decompressFn[nkey],
											  giststate->supportCollation[nkey],
											  PointerGetDatum(e)));
		/* decompressFn may just return the given pointer */
		if (dep != e)
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, l);
}

IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  Datum *attdata, bool *isnull, bool isleaf)
{
	Datum		compatt[INDEX_MAX_KEYS];
	IndexTuple	res;

	gistCompressValues(giststate, r, attdata, isnull, isleaf, compatt);

	res = index_form_tuple(isleaf ? giststate->leafTupdesc :
						   giststate->nonLeafTupdesc,
						   compatt, isnull);

	/*
	 * The offset number on tuples on internal pages is unused. For historical
	 * reasons, it is set to 0xffff.
	 */
	ItemPointerSetOffsetNumber(&(res->t_tid), 0xffff);
	return res;
}

void
gistCompressValues(GISTSTATE *giststate, Relation r,
				   Datum *attdata, bool *isnull, bool isleaf, Datum *compatt)
{
	int			i;

	/*
	 * Call the compress method on each attribute.
	 */
	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		if (isnull[i])
			compatt[i] = (Datum) 0;
		else
		{
			GISTENTRY	centry;
			GISTENTRY  *cep;

			gistentryinit(centry, attdata[i], r, NULL, (OffsetNumber) 0,
						  isleaf);
			/* there may not be a compress function in opclass */
			if (OidIsValid(giststate->compressFn[i].fn_oid))
				cep = (GISTENTRY *)
					DatumGetPointer(FunctionCall1Coll(&giststate->compressFn[i],
													  giststate->supportCollation[i],
													  PointerGetDatum(&centry)));
			else
				cep = &centry;
			compatt[i] = cep->key;
		}
	}

	if (isleaf)
	{
		/*
		 * Emplace each included attribute if any.
		 */
		for (; i < r->rd_att->natts; i++)
		{
			if (isnull[i])
				compatt[i] = (Datum) 0;
			else
				compatt[i] = attdata[i];
		}
	}
}

/*
 * initialize a GiST entry with fetched value in key field
 */
static Datum
gistFetchAtt(GISTSTATE *giststate, int nkey, Datum k, Relation r)
{
	GISTENTRY	fentry;
	GISTENTRY  *fep;

	gistentryinit(fentry, k, r, NULL, (OffsetNumber) 0, false);

	fep = (GISTENTRY *)
		DatumGetPointer(FunctionCall1Coll(&giststate->fetchFn[nkey],
										  giststate->supportCollation[nkey],
										  PointerGetDatum(&fentry)));

	/* fetchFn set 'key', return it to the caller */
	return fep->key;
}

/*
 * Fetch all keys in tuple.
 * Returns a new HeapTuple containing the originally-indexed data.
 */
HeapTuple
gistFetchTuple(GISTSTATE *giststate, Relation r, IndexTuple tuple)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(giststate->tempCxt);
	Datum		fetchatt[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);

		if (giststate->fetchFn[i].fn_oid != InvalidOid)
		{
			if (!isnull[i])
				fetchatt[i] = gistFetchAtt(giststate, i, datum, r);
			else
				fetchatt[i] = (Datum) 0;
		}
		else if (giststate->compressFn[i].fn_oid == InvalidOid)
		{
			/*
			 * If opclass does not provide compress method that could change
			 * original value, att is necessarily stored in original form.
			 */
			if (!isnull[i])
				fetchatt[i] = datum;
			else
				fetchatt[i] = (Datum) 0;
		}
		else
		{
			/*
			 * Index-only scans not supported for this column. Since the
			 * planner chose an index-only scan anyway, it is not interested
			 * in this column, and we can replace it with a NULL.
			 */
			isnull[i] = true;
			fetchatt[i] = (Datum) 0;
		}
	}

	/*
	 * Get each included attribute.
	 */
	for (; i < r->rd_att->natts; i++)
	{
		fetchatt[i] = index_getattr(tuple, i + 1, giststate->leafTupdesc,
									&isnull[i]);
	}
	MemoryContextSwitchTo(oldcxt);

	return heap_form_tuple(giststate->fetchTupdesc, fetchatt, isnull);
}

float
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *orig, bool isNullOrig,
			GISTENTRY *add, bool isNullAdd)
{
	float		penalty = 0.0;

	if (giststate->penaltyFn[attno].fn_strict == false ||
		(isNullOrig == false && isNullAdd == false))
	{
		FunctionCall3Coll(&giststate->penaltyFn[attno],
						  giststate->supportCollation[attno],
						  PointerGetDatum(orig),
						  PointerGetDatum(add),
						  PointerGetDatum(&penalty));
		/* disallow negative or NaN penalty */
		if (isnan(penalty) || penalty < 0.0)
			penalty = 0.0;
	}
	else if (isNullOrig && isNullAdd)
		penalty = 0.0;
	else
	{
		/* try to prevent mixing null and non-null values */
		penalty = get_float4_infinity();
	}

	return penalty;
}

/*
 * Initialize a new index page
 */
void
gistinitpage(Page page, uint32 f)
{
	GISTPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(GISTPageOpaqueData));

	opaque = GistPageGetOpaque(page);
	opaque->rightlink = InvalidBlockNumber;
	opaque->flags = f;
	opaque->gist_page_id = GIST_PAGE_ID;
}

/*
 * Initialize a new index buffer
 */
void
GISTInitBuffer(Buffer b, uint32 f)
{
	Page		page;

	page = BufferGetPage(b);
	gistinitpage(page, f);
}

/*
 * Verify that a freshly-read page looks sane.
 */
void
gistcheckpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GISTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}


/*
 * Allocate a new page (either by recycling, or by extending the index file)
 *
 * The returned buffer is already pinned and exclusive-locked
 *
 * Caller is responsible for initializing the page by calling GISTInitBuffer
 */
Buffer
gistNewBuffer(Relation r)
{
	Buffer		buffer;
	bool		needLock;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(r);

		if (blkno == InvalidBlockNumber)
			break;				/* nothing left in FSM */

		buffer = ReadBuffer(r, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			/*
			 * If the page was never initialized, it's OK to use.
			 */
			if (PageIsNew(page))
				return buffer;

			gistcheckpage(r, buffer);

			/*
			 * Otherwise, recycle it if deleted, and too old to have any
			 * processes interested in it.
			 */
			if (gistPageRecyclable(page))
			{
				/*
				 * If we are generating WAL for Hot Standby then create a WAL
				 * record that will allow us to conflict with queries running
				 * on standby, in case they have snapshots older than the
				 * page's deleteXid.
				 */
				if (XLogStandbyInfoActive() && RelationNeedsWAL(r))
					gistXLogPageReuse(r, blkno, GistPageGetDeleteXid(page));

				return buffer;
			}

			LockBuffer(buffer, GIST_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	needLock = !RELATION_IS_LOCAL(r);

	if (needLock)
		LockRelationForExtension(r, ExclusiveLock);

	buffer = ReadBuffer(r, P_NEW);
	LockBuffer(buffer, GIST_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(r, ExclusiveLock);

	return buffer;
}

/* Can this page be recycled yet? */
bool
gistPageRecyclable(Page page)
{
	if (PageIsNew(page))
		return true;
	if (GistPageIsDeleted(page))
	{
		/*
		 * The page was deleted, but when? If it was just deleted, a scan
		 * might have seen the downlink to it, and will read the page later.
		 * As long as that can happen, we must keep the deleted page around as
		 * a tombstone.
		 *
		 * For that check if the deletion XID could still be visible to
		 * anyone. If not, then no scan that's still in progress could have
		 * seen its downlink, and we can recycle it.
		 */
		FullTransactionId deletexid_full = GistPageGetDeleteXid(page);

		return GlobalVisCheckRemovableFullXid(NULL, deletexid_full);
	}
	return false;
}

bytea *
gistoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(GiSTOptions, fillfactor)},
		{"buffering", RELOPT_TYPE_ENUM, offsetof(GiSTOptions, buffering_mode)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_GIST,
									  sizeof(GiSTOptions),
									  tab, lengthof(tab));
}

/*
 *	gistproperty() -- Check boolean properties of indexes.
 *
 * This is optional for most AMs, but is required for GiST because the core
 * property code doesn't support AMPROP_DISTANCE_ORDERABLE.  We also handle
 * AMPROP_RETURNABLE here to save opening the rel to call gistcanreturn.
 */
bool
gistproperty(Oid index_oid, int attno,
			 IndexAMProperty prop, const char *propname,
			 bool *res, bool *isnull)
{
	Oid			opclass,
				opfamily,
				opcintype;
	int16		procno;

	/* Only answer column-level inquiries */
	if (attno == 0)
		return false;

	/*
	 * Currently, GiST distance-ordered scans require that there be a distance
	 * function in the opclass with the default types (i.e. the one loaded
	 * into the relcache entry, see initGISTstate).  So we assume that if such
	 * a function exists, then there's a reason for it (rather than grubbing
	 * through all the opfamily's operators to find an ordered one).
	 *
	 * Essentially the same code can test whether we support returning the
	 * column data, since that's true if the opclass provides a fetch proc.
	 */

	switch (prop)
	{
		case AMPROP_DISTANCE_ORDERABLE:
			procno = GIST_DISTANCE_PROC;
			break;
		case AMPROP_RETURNABLE:
			procno = GIST_FETCH_PROC;
			break;
		default:
			return false;
	}

	/* First we need to know the column's opclass. */
	opclass = get_index_column_opclass(index_oid, attno);
	if (!OidIsValid(opclass))
	{
		*isnull = true;
		return true;
	}

	/* Now look up the opclass family and input datatype. */
	if (!get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		*isnull = true;
		return true;
	}

	/* And now we can check whether the function is provided. */

	*res = SearchSysCacheExists4(AMPROCNUM,
								 ObjectIdGetDatum(opfamily),
								 ObjectIdGetDatum(opcintype),
								 ObjectIdGetDatum(opcintype),
								 Int16GetDatum(procno));

	/*
	 * Special case: even without a fetch function, AMPROP_RETURNABLE is true
	 * if the opclass has no compress function.
	 */
	if (prop == AMPROP_RETURNABLE && !*res)
	{
		*res = !SearchSysCacheExists4(AMPROCNUM,
									  ObjectIdGetDatum(opfamily),
									  ObjectIdGetDatum(opcintype),
									  ObjectIdGetDatum(opcintype),
									  Int16GetDatum(GIST_COMPRESS_PROC));
	}

	*isnull = false;

	return true;
}

/*
 * Some indexes are not WAL-logged, but we need LSNs to detect concurrent page
 * splits anyway. This function provides a fake sequence of LSNs for that
 * purpose.
 */
XLogRecPtr
gistGetFakeLSN(Relation rel)
{
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		/*
		 * Temporary relations are only accessible in our session, so a simple
		 * backend-local counter will do.
		 */
		static XLogRecPtr counter = FirstNormalUnloggedLSN;

		return counter++;
	}
	else if (RelationIsPermanent(rel))
	{
		/*
		 * WAL-logging on this relation will start after commit, so its LSNs
		 * must be distinct numbers smaller than the LSN at the next commit.
		 * Emit a dummy WAL record if insert-LSN hasn't advanced after the
		 * last call.
		 */
		static XLogRecPtr lastlsn = InvalidXLogRecPtr;
		XLogRecPtr	currlsn = GetXLogInsertRecPtr();

		/* Shouldn't be called for WAL-logging relations */
		Assert(!RelationNeedsWAL(rel));

		/* No need for an actual record if we already have a distinct LSN */
		if (!XLogRecPtrIsInvalid(lastlsn) && lastlsn == currlsn)
			currlsn = gistXLogAssignLSN();

		lastlsn = currlsn;
		return currlsn;
	}
	else
	{
		/*
		 * Unlogged relations are accessible from other backends, and survive
		 * (clean) restarts. GetFakeLSNForUnloggedRel() handles that for us.
		 */
		Assert(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED);
		return GetFakeLSNForUnloggedRel();
	}
}
