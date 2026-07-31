#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PG_CONFIG=${PG_CONFIG:-/usr/local/pgsql/bin/pg_config}
PSQL=${PSQL:-"$($PG_CONFIG --bindir)/psql"}

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-55432}
PGUSER_NAME=${PGUSER_NAME:-postgres}
DATABASE=${DATABASE:-postgres}
BUILD_INSTALL=${BUILD_INSTALL:-1}
RUN_TAP=${RUN_TAP:-0}

if [ "$BUILD_INSTALL" = "1" ]; then
	echo "== Build and install pgvector =="
	make -C "$ROOT_DIR" PG_CONFIG="$PG_CONFIG"
	make -C "$ROOT_DIR" PG_CONFIG="$PG_CONFIG" install
	echo
fi

echo "== SQL smoke on running PostgreSQL =="
output=$(
	PGOPTIONS="-c client_min_messages=warning" \
	"$PSQL" -X -qAt -F ',' -v ON_ERROR_STOP=1 \
		-h "$HOST" -p "$PORT" -U "$PGUSER_NAME" -d "$DATABASE" <<'SQL'
CREATE EXTENSION IF NOT EXISTS vector;
LOAD 'vector';
SELECT 'guc', current_setting('hnsw.in_traversal_filter');

CREATE TEMP TABLE hnsw_in_traversal_latest_smoke (
	id int PRIMARY KEY,
	keep bool NOT NULL,
	v vector(3) NOT NULL
);

INSERT INTO hnsw_in_traversal_latest_smoke
SELECT i, i > 990, ARRAY[i::float4, 0, 0]::vector
FROM generate_series(1, 1000) i;

CREATE INDEX ON hnsw_in_traversal_latest_smoke USING hnsw (v vector_l2_ops);
ANALYZE hnsw_in_traversal_latest_smoke;

SET enable_seqscan = off;
SET hnsw.iterative_scan = off;
SET hnsw.ef_search = 5;
SET hnsw.in_traversal_filter = off;
SELECT 'postfilter', count(*) FROM (
	SELECT id
	FROM hnsw_in_traversal_latest_smoke
	WHERE keep
	ORDER BY v <-> '[1,0,0]'
	LIMIT 5
) s;

SET hnsw.in_traversal_filter = on;
SELECT 'in_traversal_hnsw', count(*) FROM (
	SELECT id
	FROM hnsw_in_traversal_latest_smoke
	WHERE keep
	ORDER BY v <-> '[1,0,0]'
	LIMIT 5
) s;

SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;
RESET enable_seqscan;
SET hnsw.in_traversal_filter = off;
SELECT 'in_traversal_exact', count(*) FROM (
	SELECT id
	FROM hnsw_in_traversal_latest_smoke
	WHERE keep
	ORDER BY v <-> '[1,0,0]'
	LIMIT 5
) s;
SQL
)

echo "$output"

guc=$(echo "$output" | awk -F',' '$1 == "guc" {print $2}')
postfilter=$(echo "$output" | awk -F',' '$1 == "postfilter" {print $2}')
in_traversal_hnsw=$(echo "$output" | awk -F',' '$1 == "in_traversal_hnsw" {print $2}')
in_traversal_exact=$(echo "$output" | awk -F',' '$1 == "in_traversal_exact" {print $2}')

[ "$guc" = "off" ] || { echo "unexpected GUC default: $guc" >&2; exit 1; }
[ "${postfilter:-999}" -lt 5 ] || { echo "postfilter should underfill but returned $postfilter" >&2; exit 1; }
[ "$in_traversal_hnsw" = "5" ] || { echo "in_traversal_hnsw expected 5 rows but returned $in_traversal_hnsw" >&2; exit 1; }
[ "$in_traversal_exact" = "5" ] || { echo "in_traversal_exact expected 5 rows but returned $in_traversal_exact" >&2; exit 1; }

echo "SQL smoke passed"

if [ "$RUN_TAP" = "1" ]; then
	echo
	echo "== TAP test =="
	if [ "$(id -u)" = "0" ]; then
		echo "TAP tests create a temporary PostgreSQL cluster and cannot run as root." >&2
		echo "Run this script as a non-root user or leave RUN_TAP=0." >&2
		exit 1
	fi

	PERL5LIB_PREFIX=""
	if [ -d /home/postgres/src/test/perl ]; then
		PERL5LIB_PREFIX="/home/postgres/src/test/perl"
	fi

	cd "$ROOT_DIR"
	PERL5LIB="${PERL5LIB_PREFIX}${PERL5LIB:+:${PERL5LIB:-}}" \
		PATH="$($PG_CONFIG --bindir):$PATH" \
		PGPORT="${PGPORT:-65432}" \
		prove -I "$PERL5LIB_PREFIX" -I "$ROOT_DIR" -I "$ROOT_DIR/test/perl" \
		test/t/049_hnsw_in_traversal_filter.pl
fi
