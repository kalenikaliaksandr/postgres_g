
# Copyright (c) 2021, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent modifications
use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;

use Test::More tests => 4;

my ($node, $result);

#
# Test set-up
#
$node = PostgresNode->new('CIC_test');
$node->init;
$node->append_conf('postgresql.conf', 'lock_timeout = 180000');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int)));
$node->safe_psql('postgres', q(CREATE INDEX idx on tbl(i)));

#
# Stress CIC with pgbench
#

# Run background pgbench with CIC. We cannot mix-in this script into single
# pgbench: CIC will deadlock with itself occasionally.
my $pgbench_out   = '';
my $pgbench_timer = IPC::Run::timeout(180);
my $pgbench_h     = $node->background_pgbench(
	'--no-vacuum --client=1 --time=100',
	{
		'002_pgbench_concurrent_cic' => q(
			REINDEX INDEX CONCURRENTLY idx;
			SELECT bt_index_check('idx',true);
		   )
	},
	\$pgbench_out,
	$pgbench_timer);

# Run pgbench.
$node->pgbench(
	'--no-vacuum --client=5 --time=100',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs',
	{
		'002_pgbench_concurrent_transaction' => q(
			BEGIN;
			SELECT pg_sleep(0.001);
			INSERT INTO tbl VALUES(0);
			COMMIT;
		  ),
		'002_pgbench_concurrent_transaction_savepoints' => q(
			BEGIN;
			SELECT pg_sleep(0.001);
			SAVEPOINT s1;
			SELECT pg_sleep(0.001);
			INSERT INTO tbl VALUES(0);
			COMMIT;
		  )
	});

$pgbench_h->pump_nb;
$pgbench_h->finish();
$result =
    ($Config{osname} eq "MSWin32")
  ? ($pgbench_h->full_results)[0]
  : $pgbench_h->result(0);
is($result, 0, "pgbench with CIC works");

# done
$node->stop;
done_testing();
