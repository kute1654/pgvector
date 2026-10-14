/*-------------------------------------------------------------------------
 *
 * veccache.h
 *		Backend-local query result cache for vector (HNSW) index scans.
 *
 * Design: see Vector_Query_Result_Cache_Design.md
 *
 * Layer-2 "query result cache".  When the same normalized query vector (plus
 * ef_search / distance operator / index) is requested again and the index has
 * not been written since the previous answer was cached, we replay the stored
 * ordered candidate list and skip the entire HNSW graph search.
 *
 * v2 scope / safety note (hardened per code review; mirrors ClickHouse
 * QueryResultCache's "not transactionally consistent, TTL-bounded" contract)
 * ---------------------
 *   * The cache is backend-local (per backend process), mirroring existing
 *     per-backend caches such as index->rd_amcache.
 *   * Same-backend invalidation uses a backend-local "write generation" bumped
 *     on insert / vacuum / (re)build, keyed by the index relfilenumber.
 *     relfilenumber changes on REINDEX / VACUUM FULL and therefore also
 *     invalidates.  Writes by THIS backend are strictly invalidating.
 *   * Cross-backend invalidation is time-based: every entry carries a creation
 *     timestamp and is discarded once pgvector.result_cache_ttl (default 60 s)
 *     elapses.  This bounds the staleness window caused by writes of OTHER
 *     backends to at most TTL seconds -- the same trade-off ClickHouse's query
 *     cache makes (see ClickHouse QueryResultCache.h: "The cache does not aim
 *     to be transactionally consistent ... a expiry time (TTL) must be
 *     specified for each cache entry after which it becomes stale").
 *   * Replacement policy is FIFO (insertion order) with a total-byte budget,
 *     both tunable via pgvector.result_cache_* GUCs.
 *   * Only plain `vector` indexes are cached (opclass input type == vector);
 *     halfvec / sparsevec / bit are excluded because their Datum layouts differ
 *     from the Vector layout the hash/copy code assumes.  Gated in hnswscan.c.
 *   * Iterative scans (hnsw.iterative_scan <> off) are not cached.
 *
 * Portions Copyright (c) 2026
 *-------------------------------------------------------------------------
 */
#ifndef VECCACHE_H
#define VECCACHE_H

#include "postgres.h"

#include "access/genam.h"
#include "nodes/pg_list.h"
#include "utils/relptr.h"

#include "hnsw.h"

/* Statistics bucket exposed to _PG_init / debug code */
typedef struct VecCacheStats
{
	uint64		hits;
	uint64		misses;
	uint64		fills;
	uint64		evictions;
	uint64		staleInvalids;
	uint64		resets;
	uint64		expired;		/* entries discarded because the TTL elapsed */
	uint64		rejected;		/* stores refused by admission control */
}			VecCacheStats;

/*
 * Try to answer the KNN scan from the result cache.
 *
 * On a hit, *result points to a freshly built List of HnswSearchCandidate*
 * (allocated in CurrentMemoryContext) that reproduces the exact tuple order of
 * the original HNSW scan, and true is returned.  On a miss false is returned
 * and *result is left untouched.
 *
 * q is the already-normalized query vector (as returned by GetScanValue),
 * ef_search the current hnsw.ef_search, distanceFunc the OID of the ORDER BY
 * distance operator function (discriminates metric/opclass per query).
 */
extern bool VecCacheLookup(Relation index, Vector *q, int ef_search, Oid distanceFunc, List **result);

/*
 * Store the result of a HNSW scan so a later identical query can be answered
 * from cache.  candidates is the List of HnswSearchCandidate* produced by the
 * scan; its contents are deep-copied into the cache context.
 */
extern void VecCacheStore(Relation index, Vector *q, int ef_search, Oid distanceFunc, List *candidates);

/* Bump the write generation of the given index (call from write paths) */
extern void VecCacheBumpWriteGen(Relation index);

/* Invalidate all cached results (used by reset paths and GUC reloads) */
extern void VecCacheReset(void);

/* Register GUCs; call from _PG_init */
extern void VecCacheInit(void);

/* Backend-visible diagnostic logger (gated by pgvector.result_cache_debug) */
extern void VecCacheLog(const char *fmt, ...) pg_attribute_printf(1, 2);

/* Accessors for tests / diagnostics */
extern const VecCacheStats *VecCacheGetStats(void);

#endif							/* VECCACHE_H */
