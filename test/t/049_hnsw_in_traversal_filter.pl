use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $dim = 3;

# Initialize node
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# Create table
$node->safe_psql("postgres", "CREATE EXTENSION vector;");
$node->safe_psql("postgres", "CREATE TABLE tst (i int4 PRIMARY KEY, keep bool, v vector($dim));");
$node->safe_psql("postgres", qq(
	INSERT INTO tst
	SELECT i, i > 990, ARRAY[i::float4, 0, 0]
	FROM generate_series(1, 1000) i;
));
$node->safe_psql("postgres", "CREATE INDEX ON tst USING hnsw (v vector_l2_ops);");

my $guc = $node->safe_psql("postgres", "SHOW hnsw.in_traversal_filter;");
is($guc, "off", "hnsw.in_traversal_filter defaults to off");

my $exact_count = $node->safe_psql("postgres", qq(
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) FROM (
		SELECT i FROM tst
		WHERE keep
		ORDER BY v <-> '[1,0,0]'
		LIMIT 5
	) s;
));
is($exact_count, 5, "exact filtered scan can return five rows");

my $postfilter_count = $node->safe_psql("postgres", qq(
	SET enable_seqscan = off;
	SET hnsw.iterative_scan = off;
	SET hnsw.ef_search = 5;
	SET hnsw.in_traversal_filter = off;
	SELECT count(*) FROM (
		SELECT i FROM tst
		WHERE keep
		ORDER BY v <-> '[1,0,0]'
		LIMIT 5
	) s;
));
cmp_ok($postfilter_count, '<', 5, "postfilter can underfill with selective scalar quals");

my $in_traversal_count = $node->safe_psql("postgres", qq(
	SET enable_seqscan = off;
	SET hnsw.iterative_scan = off;
	SET hnsw.ef_search = 5;
	SET hnsw.in_traversal_filter = on;
	SELECT count(*) FROM (
		SELECT i FROM tst
		WHERE keep
		ORDER BY v <-> '[1,0,0]'
		LIMIT 5
	) s;
));
is($in_traversal_count, 5, "in-traversal filtering fills the filtered top-k result");

done_testing();
