/*-------------------------------------------------------------------------
 *
 * veccache.c
 *		Backend-local query result cache for vector (HNSW) index scans.
 *
 * See veccache.h and Vector_Query_Result_Cache_Design.md (sections 4, 5, 8).
 *
 * Correctness model (v2, hardened per code review)
 *   - Key = (index relfilenumber, ef_search, distance-op OID,
 *            normalized query vector dim+float-bytes).
 *   - Same-backend freshness: the entry records the backend-local write
 *     generation of the index at fill time; a later lookup compares against the
 *     current generation and a mismatch makes the entry stale and evicts it
 *     (writes by THIS backend are strictly invalidated).
 *   - Cross-backend freshness: every entry carries a creation timestamp.  An
 *     entry older than pgvector.result_cache_ttl (default 60 s) is treated as
 *     expired on lookup and evicted.  This mirrors ClickHouse's
 *     QueryResultCache contract: the cache is explicitly NOT transactionally
 *     consistent across backends, and the TTL is the mechanism that bounds the
 *     cross-backend staleness window to at most TTL seconds.
 *   - Replacement policy: FIFO (insertion order), not LRU.  On admission of a
 *     new entry at capacity (entry count or total bytes), the oldest-inserted
 *     entries are evicted first.  Expired entries are also removed lazily on
 *     lookup, and - being the oldest - first by the FIFO eviction path.
 *   - Resource limits: max entry count, max rows and bytes per entry, and a
 *     total byte budget are all GUC-configurable (see VecCacheInit).
 *   - relfilenumber changes on REINDEX / VACUUM FULL, also invalidating.
 *   - Only plain `vector` indexes are cached.  halfvec / sparsevec / bit use
 *     different Datum layouts and would be mis-read by the vector hash /
 *     memcmp; the caller (hnswscan.c) gates on opclass input type == vector.
 *   - The cache is backend-local; see the caveats in veccache.h.
 *
 * Data layout
 *   A small open hash table (buckets[] chains) plus a FIFO doubly-linked list
 *   for eviction.  Entries and their payloads live in CacheMemoryContext.
 *
 * Threading / lifecycle
 *   - Single backend process: index scans run serially within a backend, so no
 *     lock is required for the hash buckets or the FIFO list.
 *   - All entry allocations happen in the dedicated vector_cache_ctx memory
 *     context, which is reset/aged only by FIFO eviction and VecCacheReset(),
 *     not by per-tuple contexts.  Therefore cached blobs outlive the scan that
 *     produced them and stay valid for later queries in the same backend.
 *
 * Portions Copyright (c) 2026
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "veccache.h"

/* -------------------------------------------------------------------------
 * GUCs
 * ------------------------------------------------------------------------- */
/* Master switch: when off, lookups always miss and stores are no-ops. */
static bool vec_cache_enabled = true;
/* Hard cap on the number of live cache entries; enforced by FIFO eviction. */
static int	vec_cache_max_entries = 4096;
/* Per-entry caps: max candidate rows and max bytes (kB; 0 = no byte cap). */
static int	vec_cache_max_entry_rows = 1000;
static int	vec_cache_max_entry_size = 1024;	/* kB */
/* Total cache byte budget in kB (0 = unlimited). */
static int	vec_cache_max_size = 65536;	/* kB (64 MB default) */
/* Entry lifetime in seconds; 0 = never expire. */
static int	vec_cache_ttl = 60;
/* Debug logging switch: gates vc_debug() and VecCacheLog() output. */
static bool vec_cache_debug = false;

/* Number of buckets in the open hash table (must remain a power of two so the
 * mask-based bucket computation in lookup()/store() stays correct). */
#define VEC_CACHE_BUCKETS 1024

/* -------------------------------------------------------------------------
 * Per-cached-result row: one ORDER BY candidate (an element + its heap TIDs).
 * HNSW_HEAPTIDS is fixed (hnsw.h) so a fixed array keeps replay cheap.
 *
 * We persist only what is needed to faithfully replay the scan output:
 *   - distance: the similarity distance observed by the original search;
 *   - heaptids: the heap TIDs the candidate maps to, so the executor can walk
 *     them exactly as it would after a real HNSW search.
 * ------------------------------------------------------------------------- */
typedef struct VectorResultCacheRow
{
	double		distance;
	int			nheaptids;
	ItemPointerData heaptids[HNSW_HEAPTIDS];
}			VectorResultCacheRow;

/* -------------------------------------------------------------------------
 * A single cache entry: the persisted answer for one (index, query vector,
 * ef_search, distance operator) tuple.
 *
 * Field roles:
 *   hashkey        combined hash for O(1) bucket selection and fast reject;
 *   relfilenumber  "which index"; also doubles as invalidation signal because
 *                  REINDEX / VACUUM FULL allocate a new relfilenumber;
 *   ef_search      HNSW recall knob (different ef -> different result set);
 *   distanceFunc   OID of the ORDER BY distance operator function, i.e. the
 *                  metric/opclass discriminator (<=> / <-> / <#> ...);
 *   dim            query vector dimension, part of the exact-equality test;
 *   vecbytes       byte copy of the (normalized) query vector;
 *   vecbytes_hash  hash of vecbytes, a cheap pre-filter before memcmp;
 *   writegen       index write generation at fill time (same-backend staleness);
 *   created_at     fill timestamp; TTL expiry check (cross-backend staleness);
 *   entry_bytes    accounting weight for the total-byte budget / FIFO eviction;
 *   nrows/rows     the ordered candidate rows returned by the scan.
 * ------------------------------------------------------------------------- */
typedef struct VectorResultCacheEntry
{
	struct VectorResultCacheEntry *next;	/* bucket chain */
	struct VectorResultCacheEntry *fifo_prev;	/* FIFO list: prev=older */
	struct VectorResultCacheEntry *fifo_next;	/* FIFO list: next=newer */
	uint64		hashkey;		/* combined hash for fast reject */
	RelFileNumber relfilenumber;
	int			ef_search;
	Oid			distanceFunc;
	int16		dim;
	uint64		vecbytes_hash;	/* hash of query vector float bytes */
	uint64		writegen;		/* index write generation at fill */
	TimestampTz created_at;		/* fill time (TTL expiry anchor) */
	int			entry_bytes;	/* total bytes owned by this entry */
	int			nrows;			/* number of candidate rows */
	uint32		vecbytes_len;	/* dim * sizeof(float) */
	float	   *vecbytes;		/* copy of the (normalized) query vector */
	VectorResultCacheRow *rows; /* ordered candidates returned by the scan */
}			VectorResultCacheEntry;

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
/* Dedicated context holding all entries; created lazily on first store. */
static MemoryContext vector_cache_ctx = NULL;
/* Open hash table: head pointer per bucket. */
static VectorResultCacheEntry *cache_buckets[VEC_CACHE_BUCKETS];
/* FIFO list: fifo_head is oldest-inserted (eviction target), fifo_tail newest. */
static VectorResultCacheEntry *fifo_head = NULL;
static VectorResultCacheEntry *fifo_tail = NULL;
/* Number of live entries and total bytes currently in the cache. */
static int	cache_count = 0;
static size_t cache_bytes = 0;

/* backend-local write-generation map: RelFileNumber -> uint64
 * Kept separate from the entry table so a write only touches one entry and we
 * never scan the whole cache to invalidate. */
static HTAB *write_gen_map = NULL;

/* Counter bucket used for diagnostics and future pgvector.result_cache_stats(). */
static VecCacheStats cache_stats;

/* -------------------------------------------------------------------------
 * Debug helpers
 * ------------------------------------------------------------------------- */

/*
 * vc_debug() -- low-level debug logger used inside veccache.c.
 * Gated by result_cache_debug GUC; emits with the canonical "pgvector result
 * cache:" prefix so log greps can correlate lookups/stores with downstream
 * VecCacheLog() output from other modules (e.g. hnswscan).
 */
static void
vc_debug(const char *fmt, ...) pg_attribute_printf(1, 2);

static void
vc_debug(const char *fmt, ...)
{
	char		buf[512];
	va_list		args;

	if (!vec_cache_debug)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ereport(INFO, (errmsg_internal("pgvector result cache: %s", buf)));
}

/*
 * Backend-visible diagnostic logger.  Reflected by the result_cache_debug GUC,
 * so other modules (e.g. hnswscan) can emit per-query diagnostics that are
 * easy to correlate with VecCacheLookup/Store activity.
 */
void
VecCacheLog(const char *fmt, ...) pg_attribute_printf(1, 2);

void
VecCacheLog(const char *fmt, ...)
{
	va_list		args;
	char		buf[512];

	if (!vec_cache_debug)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ereport(INFO, (errmsg_internal("pgvector result cache: %s", buf)));
}

/* -------------------------------------------------------------------------
 * Hash helpers
 * ------------------------------------------------------------------------- */

/*
 * vector_bytes_hash() -- deterministic hash over the raw query-vector bytes.
 *
 * We hash the exact float bytes (the same memcmp input) rather than the parsed
 * numeric values, so a lookup reproduces the store-time hash bit-for-bit.  The
 * hash is only a fast-reject filter; correctness always falls back to memcmp.
 */
static uint64
vector_bytes_hash(const float *x, int dim, uint64 seed)
{
	const unsigned char *k = (const unsigned char *) x;
	return hash_bytes_extended(k, dim * (int) sizeof(float), seed);
}

/*
 * compute_hashkey() -- fold every key component into one uint64.
 *
 * hash_bytes_extended is seeded with the prior hash so each field statistically
 * contributes to the final value; the exact-equality test in lookup() still
 * compares each field independently (memcmp on bytes), so aliasing here can
 * only cause a probe falling into the wrong bucket, never a false hit.
 */
static uint64
compute_hashkey(RelFileNumber rel, int ef, Oid uproc, int16 dim, uint64 vhash)
{
	uint64		h = vhash;

	h = hash_bytes_extended((const unsigned char *) &rel, sizeof(RelFileNumber), h);
	h = hash_bytes_extended((const unsigned char *) &ef, sizeof(ef), h);
	h = hash_bytes_extended((const unsigned char *) &uproc, sizeof(uproc), h);
	h = hash_bytes_extended((const unsigned char *) &dim, sizeof(dim), h);
	return h;
}

/* -------------------------------------------------------------------------
 * Write-generation map (dynahash, keyed by relfilenumber).
 * Entry layout: [ sizeof(RelFileNumber) bytes of key ][ uint64 value ]
 * ------------------------------------------------------------------------- */
/* Value pointer is stored right after the fixed-size key in each hash entry. */
#define VEC_GEN_VALUE(_ent)		((uint64 *) ((char *) (_ent) + sizeof(RelFileNumber)))

/*
 * get_write_gen() -- current write generation for a relation, or 0 if never
 * written (i.e. unknown/missing).  0 is also what brand-new entries cache with,
 * so an untouched index keeps producing valid hits.
 */
static uint64
get_write_gen(RelFileNumber rel)
{
	uint64	   *gen;

	if (write_gen_map == NULL)
		return 0;

	gen = (uint64 *) hash_search(write_gen_map, &rel, HASH_FIND, NULL);
	if (gen == NULL)
		return 0;

	return *VEC_GEN_VALUE(gen);
}

/*
 * bump_write_gen() -- increment (or initialize to 1) the generation of a
 * relation.  Called whenever the index content may have changed; any cache
 * entry filled before this bump becomes stale on the next lookup.
 */
static void
bump_write_gen(RelFileNumber rel)
{
	bool		found;
	uint64	   *ent;

	if (write_gen_map == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(RelFileNumber);
		ctl.entrysize = sizeof(RelFileNumber) + sizeof(uint64);
		ctl.hcxt = CacheMemoryContext ? CacheMemoryContext : TopMemoryContext;

		write_gen_map = hash_create("vector result-cache write generations", 64,
									&ctl, HASH_ELEM | HASH_BLOBS);
	}

	ent = (uint64 *) hash_search(write_gen_map, &rel, HASH_ENTER, &found);
	if (found)
		(*VEC_GEN_VALUE(ent))++;
	else
		*VEC_GEN_VALUE(ent) = 1;
}

/*
 * VecCacheBumpWriteGen() -- public wrapper for the write paths (hnswinsert,
 * hnswvacuum, hnswbuild).  Marks the index dirty so future lookups miss.
 */
void
VecCacheBumpWriteGen(Relation index)
{
	if (!vec_cache_enabled)
		return;

	bump_write_gen(index->rd_locator.relNumber);
}

/* -------------------------------------------------------------------------
 * FIFO maintenance
 * ------------------------------------------------------------------------- */

/* Detach e from the FIFO list (e may or may not be in it). */
static void
fifo_unlink(VectorResultCacheEntry *e)
{
	if (e->fifo_prev)
		e->fifo_prev->fifo_next = e->fifo_next;
	else
		fifo_head = e->fifo_next;

	if (e->fifo_next)
		e->fifo_next->fifo_prev = e->fifo_prev;
	else
		fifo_tail = e->fifo_prev;

	e->fifo_prev = e->fifo_next = NULL;
}

/* Append e at the tail (newest).  FIFO eviction always takes the head. */
static void
fifo_append(VectorResultCacheEntry *e)
{
	e->fifo_prev = fifo_tail;
	e->fifo_next = NULL;
	if (fifo_tail)
		fifo_tail->fifo_next = e;
	else
		fifo_head = e;
	fifo_tail = e;
}

/* Remove e from its bucket chain given its hashkey-derived bucket index. */
static void
bucket_unlink(VectorResultCacheEntry *e)
{
	uint32		b = (uint32) (e->hashkey & (VEC_CACHE_BUCKETS - 1));
	VectorResultCacheEntry **pp = &cache_buckets[b];

	while (*pp != NULL)
	{
		if (*pp == e)
		{
			*pp = e->next;
			return;
		}
		pp = &(*pp)->next;
	}
}

/* Release all resources owned by e and drop it from both indexes. */
static void
free_entry(VectorResultCacheEntry *e)
{
	fifo_unlink(e);
	bucket_unlink(e);
	if (e->vecbytes)
		pfree(e->vecbytes);
	if (e->rows)
		pfree(e->rows);
	pfree(e);
	cache_count--;
	cache_bytes -= e->entry_bytes;
	cache_stats.evictions++;
}

/*
 * VecCacheReset() -- drop every entry.  Safe to call regardless of whether the
 * cache/disabling switch is on (it stays on the "reset on demand" contract).
 */
void
VecCacheReset(void)
{
	int			i;

	for (i = 0; i < VEC_CACHE_BUCKETS; i++)
		cache_buckets[i] = NULL;

	while (fifo_head)
		free_entry(fifo_head);

	Assert(cache_count == 0);
	Assert(cache_bytes == 0);
	cache_stats.resets++;
}

/*
 * entry_expired() -- has entry e aged past pgvector.result_cache_ttl?
 * A TTL of 0 means "never expire".
 */
static bool
entry_expired(VectorResultCacheEntry *e, TimestampTz now)
{
	if (vec_cache_ttl <= 0)
		return false;
	return (now - e->created_at > (int64) vec_cache_ttl * USECS_PER_SEC);
}

/* -------------------------------------------------------------------------
 * Admission / capacity control (FIFO + TTL)
 * ------------------------------------------------------------------------- */

/*
 * make_room_for() -- enforce the entry-count and total-byte budgets for a new
 * entry weighing entry_bytes.  Expired entries (always the oldest) are dropped
 * first, then the FIFO head (oldest-inserted) is evicted until the new entry
 * fits.  Returns true if room was made, false if the *entire* cache would have
 * to be evicted (i.e. the new entry alone exceeds the budgets).
 */
static bool
make_room_for(int entry_bytes)
{
	size_t		max_bytes;
	int			max_count;
	TimestampTz now = GetCurrentTimestamp();

	max_bytes = (vec_cache_max_size > 0) ? ((size_t) vec_cache_max_size * 1024) : (size_t) -1;
	max_count = vec_cache_max_entries;

	while (fifo_head != NULL)
	{
		bool		over_entries = (cache_count >= max_count);
		bool		over_bytes = (cache_bytes + (size_t) entry_bytes > max_bytes);
		bool		expired = entry_expired(fifo_head, now);

		if (!over_entries && !over_bytes && !expired)
			break;

		/* evict the oldest; expired entries are oldest by construction */
		if (expired)
			cache_stats.expired++;
		free_entry(fifo_head);
	}

	/* The new entry alone cannot fit inside the budgets: refuse to cache.
	 * This is only possible when the eviction loop above drained the cache
	 * (otherwise it would have stopped once the entry fit). */
	if (cache_bytes + (size_t) entry_bytes > max_bytes ||
		cache_count + 1 > max_count)
		return false;

	return true;
}

/* -------------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------------- */

/*
 * VecCacheLookup() -- try to answer a KNN scan from the result cache.
 *
 * On success builds a fresh List of HnswSearchCandidate* into *result (in the
 * caller's context) reproducing the exact tuple order the original HNSW scan
 * produced, and returns true.  On miss returns false and leaves *result alone.
 *
 * A hit is only reported when every key field matches, the index write
 * generation is still equal to the fill-time generation (same-backend
 * freshness), and the entry has not passed pgvector.result_cache_ttl
 * (cross-backend staleness bound).  Otherwise the entry is treated as stale,
 * evicted, and counted as a miss so the caller falls through to a real HNSW
 * search.
 */
bool
VecCacheLookup(Relation index, Vector *q, int ef_search, Oid distanceFunc, List **result)
{
	VectorResultCacheEntry *e;
	uint64		vhash;
	uint64		hashkey;
	uint64		curgen;
	uint32		b;
	RelFileNumber rel;
	TimestampTz now = GetCurrentTimestamp();

	/* Cache disabled, or no entry has ever been stored yet: definite miss. */
	if (!vec_cache_enabled || vector_cache_ctx == NULL)
	{
		cache_stats.misses++;
		return false;
	}

	rel = index->rd_locator.relNumber;
	curgen = get_write_gen(rel);

	/* Recompute the same key the store() computed, to land in the same bucket. */
	vhash = vector_bytes_hash(q->x, q->dim, 0);
	hashkey = compute_hashkey(rel, ef_search, distanceFunc, q->dim, vhash);
	b = (uint32) (hashkey & (VEC_CACHE_BUCKETS - 1));

	/* Walk the collision chain: cheap hash rejects first, exact test last. */
	for (e = cache_buckets[b]; e != NULL; e = e->next)
	{
		/* Fast path: skip unless the combined hash matches exactly. */
		if (e->hashkey != hashkey)
			continue;

		/*
		 * Exact-key test.  Each component is an AND-ed independent equality so
		 * that a hash collision can never produce a false positive.
		 */
		if (e->relfilenumber != rel ||
			e->ef_search != ef_search ||
			e->distanceFunc != distanceFunc ||
			e->dim != q->dim ||
			e->vecbytes_hash != vhash ||
			e->vecbytes_len != (uint32) (q->dim * sizeof(float)))
			continue;

		/* Bit-exact comparison of the full (normalized) query vector. */
		if (memcmp(e->vecbytes, q->x, e->vecbytes_len) != 0)
			continue;

		/*
		 * Freshness gate 1 (same-backend): if THIS backend wrote the index
		 * since we cached this answer, the result may no longer reflect the
		 * current index content.  Evict the stale entry, count a miss.
		 */
		if (e->writegen != curgen)
		{
			cache_stats.staleInvalids++;
			free_entry(e);
			cache_stats.misses++;
			return false;
		}

		/*
		 * Freshness gate 2 (cross-backend): TTL expiry.  Writes by OTHER
		 * backends are not visible through the per-backend write generation,
		 * so an entry older than result_cache_ttl is discarded.  This is the
		 * same contract as ClickHouse's QueryResultCache: the cache is not
		 * transactionally consistent, and the TTL bounds staleness.
		 */
		if (entry_expired(e, now))
		{
			cache_stats.expired++;
			free_entry(e);
			cache_stats.misses++;
			return false;
		}

		/* Hit: replay the ordered candidate list into a fresh List. */
		{
			List	   *res = NIL;
			int			i;

			for (i = 0; i < e->nrows; i++)
			{
				VectorResultCacheRow *r = &e->rows[i];
				HnswElementData *el = palloc0(sizeof(HnswElementData));
				HnswSearchCandidate *sc = palloc0(sizeof(HnswSearchCandidate));

				el->heaptidsLength = r->nheaptids;
				memcpy(el->heaptids, r->heaptids, sizeof(ItemPointerData) * r->nheaptids);
				sc->distance = r->distance;
				sc->element.ptr = el;
				res = lappend(res, sc);
			}

			*result = res;
		}

		/* FIFO: a hit does not reorder entries. */
		cache_stats.hits++;
		vc_debug("HIT rel=" UINT64_FORMAT " dim=%d ef=%d nrows=%d",
				 (uint64) rel, (int) q->dim, ef_search, e->nrows);
		return true;
	}

	cache_stats.misses++;
	return false;
}

/* -------------------------------------------------------------------------
 * Store
 * ------------------------------------------------------------------------- */

/*
 * VecCacheStore() -- persist the outcome of a real HNSW search so a later
 * identical query (same index, vector, ef_search, distance operator) can reuse
 * it without re-running the graph traversal.
 *
 * Enforcement / admission:
 *   - No-op when the cache is disabled.
 *   - Empty or oversized result sets (nrows > result_cache_max_entry_rows) are
 *     not cached, to bound per-entry memory.
 *   - Entries larger than result_cache_max_entry_size (kB) are not cached.
 *   - Entry count and total bytes are enforced by make_room_for(): expired and
 *     oldest-inserted entries (FIFO) are evicted until the new entry fits.
 *   - The query vector bytes and every candidate row are deep-copied into the
 *     dedicated vector_cache_ctx, decoupling them from the scan's lifetime.
 *   - The entry is stamped with the index's current write generation and the
 *     fill timestamp so both freshness gates can invalidate it later.
 */
void
VecCacheStore(Relation index, Vector *q, int ef_search, Oid distanceFunc, List *candidates)
{
	RelFileNumber rel;
	ListCell   *lc;
	int			nrows;
	int			i;
	VectorResultCacheEntry *e;
	uint64		curgen;
	uint64		vhash;
	uint64		hashkey;
	uint32		b;
	MemoryContext oldCtx;
	int			entry_bytes;

	if (!vec_cache_enabled)
		return;

	rel = index->rd_locator.relNumber;
	nrows = list_length(candidates);

	/* Do not cache tiny/empty or oversized scans (resource control). */
	if (nrows <= 0 || nrows > vec_cache_max_entry_rows)
		return;

	/* Per-entry byte cap (resource control). */
	entry_bytes = (int) sizeof(VectorResultCacheEntry)
		+ (int) (q->dim * sizeof(float))
		+ (int) (nrows * sizeof(VectorResultCacheRow));
	if (vec_cache_max_entry_size > 0 &&
		entry_bytes > vec_cache_max_entry_size * 1024)
	{
		cache_stats.rejected++;
		return;
	}

	/* Enforce entry-count and total-byte budgets (FIFO + TTL eviction). */
	if (!make_room_for(entry_bytes))
	{
		cache_stats.rejected++;
		return;
	}

	/* Lazily create the dedicated cache context on first use. */
	if (vector_cache_ctx == NULL)
	{
		MemoryContext parent = CacheMemoryContext ? CacheMemoryContext : TopMemoryContext;

		vector_cache_ctx = AllocSetContextCreate(parent,
												 "pgvector result cache",
												 ALLOCSET_DEFAULT_SIZES);
	}

	if (vec_cache_debug)
		vc_debug("STORE rel=" UINT64_FORMAT " dim=%d ef=%d nrows=%d",
				 (uint64) rel, (int) q->dim, ef_search, nrows);

	/* Snapshot the key, generation and time that this entry is matched by. */
	curgen = get_write_gen(rel);
	vhash = vector_bytes_hash(q->x, q->dim, 0);
	hashkey = compute_hashkey(rel, ef_search, distanceFunc, q->dim, vhash);

	/* Build the entry inside the cache context so it survives the scan. */
	oldCtx = MemoryContextSwitchTo(vector_cache_ctx);

	e = (VectorResultCacheEntry *) palloc0(sizeof(VectorResultCacheEntry));
	e->hashkey = hashkey;
	e->relfilenumber = rel;
	e->ef_search = ef_search;
	e->distanceFunc = distanceFunc;
	e->dim = q->dim;
	e->vecbytes_hash = vhash;
	e->vecbytes_len = (uint32) (q->dim * sizeof(float));
	e->writegen = curgen;
	e->created_at = GetCurrentTimestamp();
	e->entry_bytes = entry_bytes;
	e->nrows = nrows;
	e->vecbytes = (float *) palloc(e->vecbytes_len);
	memcpy(e->vecbytes, q->x, e->vecbytes_len);

	/* Deep-copy each candidate row: distance plus its heap TIDs. */
	e->rows = (VectorResultCacheRow *) palloc0(sizeof(VectorResultCacheRow) * nrows);
	i = 0;
	foreach(lc, candidates)
	{
		HnswSearchCandidate *sc = (HnswSearchCandidate *) lfirst(lc);
		VectorResultCacheRow *r = &e->rows[i];

		r->distance = sc->distance;

		if (sc->element.ptr != NULL)
		{
			HnswElementData *el = sc->element.ptr;

			r->nheaptids = el->heaptidsLength;
			if (r->nheaptids > HNSW_HEAPTIDS)
				r->nheaptids = HNSW_HEAPTIDS;
			if (r->nheaptids > 0)
				memcpy(r->heaptids, el->heaptids, sizeof(ItemPointerData) * r->nheaptids);
		}
		else
			r->nheaptids = 0;

		i++;
	}

	MemoryContextSwitchTo(oldCtx);

	/* Insert into the bucket chain and append to the FIFO tail. */
	b = (uint32) (hashkey & (VEC_CACHE_BUCKETS - 1));
	e->next = cache_buckets[b];
	cache_buckets[b] = e;
	fifo_append(e);
	cache_count++;
	cache_bytes += entry_bytes;

	cache_stats.fills++;
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

/* Return a read-only handle to the statistics bucket. */
const VecCacheStats *
VecCacheGetStats(void)
{
	return &cache_stats;
}

/* -------------------------------------------------------------------------
 * GUC registration
 * ------------------------------------------------------------------------- */

/*
 * VecCacheInit() -- register the module's GUCs and initialize the cache tables.
 * Called from _PG_init (see vector.c), before any query uses the extension.
 */
void
VecCacheInit(void)
{
	DefineCustomBoolVariable("pgvector.result_cache",
							 "Enables the backend-local vector query result cache.",
							 NULL,
							 &vec_cache_enabled,
							 true,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pgvector.result_cache_max_entries",
							"Maximum number of entries in the vector result cache.",
							NULL,
							&vec_cache_max_entries,
							4096, 1, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pgvector.result_cache_max_entry_rows",
							"Maximum number of rows in one cached result (0 = unlimited).",
							NULL,
							&vec_cache_max_entry_rows,
							1000, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pgvector.result_cache_max_entry_size",
							"Maximum size of one cached result in kB (0 = unlimited).",
							NULL,
							&vec_cache_max_entry_size,
							1024, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pgvector.result_cache_max_size",
							"Total memory budget of the vector result cache in kB (0 = unlimited).",
							NULL,
							&vec_cache_max_size,
							65536, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pgvector.result_cache_ttl",
							"How long a cached result stays valid in seconds (0 = never expire).",
							NULL,
							&vec_cache_ttl,
							60, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("pgvector.result_cache_debug",
							 "Logs vector result cache activity at level INFO.",
							 NULL,
							 &vec_cache_debug,
							 false,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("pgvector");

	memset(cache_buckets, 0, sizeof(cache_buckets));
}