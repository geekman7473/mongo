/**
 * Tests that retryable findAndModify statements that are executed with the image collection
 * disabled inside internal transactions that start and commit on the donor during a chunk migration
 * are retryable on the recipient after the migration.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/chunk_migration_test.js");

const transactionTest =
    new InternalTransactionChunkMigrationTest(false /* storeFindAndModifyImagesInSideCollection */);
transactionTest.runTestForFindAndModifyDuringChunkMigration(
    transactionTest.InternalTxnType.kRetryable, false /* abortOnInitialTry */);
transactionTest.stop();
})();
