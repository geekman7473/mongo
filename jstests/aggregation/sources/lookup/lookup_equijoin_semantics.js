/**
 * Tests for $lookup with localField/foreignField syntax.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fixture_helpers.js");  // For isSharded.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.

const localColl = db.lookup_arrays_semantics_local;
const foreignColl = db.lookup_arrays_semantics_foreign;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(foreignColl) && !isShardedLookupEnabled) {
    return;
}

/**
 * Executes $lookup with exactly one record in the foreign collection, so we don't need to check the
 * content of the "as" field but only that it's not empty for local records with ids in
 * 'idsExpectToMatch'.
 */
function runTest_SingleForeignRecord(
    {testDescription, localRecords, localField, foreignRecord, foreignField, idsExpectedToMatch}) {
    assert('object' === typeof (foreignRecord) && !Array.isArray(foreignRecord),
           "foreignRecord should be a single document");

    localColl.drop();
    assert.commandWorked(localColl.insert(localRecords));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecord));

    const results = localColl.aggregate([{
            $lookup: {
                from: foreignColl.getName(),
                localField: localField,
                foreignField: foreignField,
                as: "matched"
            }
        }]).toArray();

    // Build the array of ids for the results that have non-empty array in the "matched" field.
    const matchedIds = results
                           .filter(function(x) {
                               return tojson(x.matched) != tojson([]);
                           })
                           .map(x => (x._id));

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: idsExpectedToMatch,
        extraErrorMsg: " **TEST** " + testDescription
    });
}

/**
 * Executes $lookup with exactly one record in the local collection and checks that the "as" field
 * for it contains documents with ids from `idsExpectedToMatch`.
 */
function runTest_SingleLocalRecord(
    {testDescription, localRecord, localField, foreignRecords, foreignField, idsExpectedToMatch}) {
    assert('object' === typeof (localRecord) && !Array.isArray(localRecord),
           "localRecord should be a single document");

    localColl.drop();
    assert.commandWorked(localColl.insert(localRecord));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecords));

    const results = localColl.aggregate([{
            $lookup: {
                from: foreignColl.getName(),
                localField: localField,
                foreignField: foreignField,
                as: "matched"
            }
        }]).toArray();
    assert.eq(1, results.length);

    // Extract matched foreign ids from the "matched" field.
    const matchedIds = results[0].matched.map(x => (x._id));

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: idsExpectedToMatch,
        extraErrorMsg: " **TEST** " + testDescription
    });
}

/**
 * Executes $lookup and expects it to fail with the specified 'expectedErrorCode`.
 */
function runTest_ExpectFailure(
    {testDescription, localRecords, localField, foreignRecords, foreignField, expectedErrorCode}) {
    localColl.drop();
    assert.commandWorked(localColl.insert(localRecords));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecords));

    assert.commandFailedWithCode(
        localColl.runCommand("aggregate", {
            pipeline: [{
                $lookup: {
                    from: foreignColl.getName(),
                    localField: localField,
                    foreignField: foreignField,
                    as: "matched"
                }
            }],
            cursor: {}
        }),
        expectedErrorCode,
        "**TEST** " + testDescription);
}

/**
 * Tests.
 */
(function testMatchingNullAndMissing() {
    const docs = [
        {_id: 0, no_a: 1},
        {_id: 1, a: null},
        {_id: 2, a: [null, 1]},

        {_id: 10, a: []},

        {_id: 20, a: 1},
        {_id: 21, a: {x: null}},
        {_id: 22, a: [[null, 1], 2]},
        {_id: 23, a: {x: undefined}},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Null in foreign, top-level field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: null},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 10]
    });
    runTest_SingleLocalRecord({
        testDescription: "Null in local, top-level field in foreign",
        localRecord: {_id: 0, b: null},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [0, 1, 2]
    });

    runTest_SingleForeignRecord({
        testDescription: "Missing in foreign, top-level field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, no_b: 1},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 10]
    });
    runTest_SingleLocalRecord({
        testDescription: "Missing in local, top-level field in foreign",
        localRecord: {_id: 0, no_b: 1},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [0, 1, 2]
    });
})();

(function testMatchingUndefined() {
    if (checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
        jsTestLog(
            "SERVER-64586, SERVER-64587: Skipping 'undefined' test when $lookup is lowered into SBE.");
        return;
    }

    const docs = [
        {_id: 0, no_a: 1},
        {_id: 1, a: null},
        {_id: 2, a: []},
        {_id: 3, a: [null, 1]},

        {_id: 10, a: 1},
        {_id: 11, a: {x: null}},
        {_id: 12, a: [[null, 1], 2]},
        {_id: 13, a: {x: undefined}},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Undefined in foreign, top-level field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: undefined},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3]
    });

    // If the left-hand side collection has a value of undefined for "localField", then the query
    // will fail. This is a consequence of the fact that queries which explicitly compare to
    // undefined, such as {$eq:undefined}, are banned. Arguably this behavior could be improved, but
    // we are unlikely to change it given that the undefined BSON type has been deprecated for many
    // years.
    runTest_ExpectFailure({
        testDescription: "Undefined in local, top-level field in foreign",
        localRecords: {_id: 0, b: undefined},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        expectedErrorCode: ErrorCodes.BadValue
    });
})();

(function testMatchingNaN() {
    const docs = [
        {_id: 0, a: NaN},
        {_id: 1, a: NumberDecimal("NaN")},
        {_id: 2, a: [1, NaN]},
        {_id: 3, a: [1, NumberDecimal("NaN")]},

        {_id: 10, a: null},
        {_id: 11, no_a: 42},
    ];

    runTest_SingleForeignRecord({
        testDescription: "NaN in foreign, top-level field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: NaN},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3]
    });
    runTest_SingleLocalRecord({
        testDescription: "NaN in local, top-level field in foreign",
        localRecord: {_id: 0, b: NaN},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [0, 1, 2, 3]
    });
})();

(function testMatchingTopLevelFieldToScalar() {
    const docs = [
        // For these docs "a" resolves to a (logical) set that contains value "1".
        {_id: 0, a: 1, y: 2},
        {_id: 1, a: [1]},
        {_id: 2, a: [1, 2, 3]},
        {_id: 3, a: [1, [2, 3]]},
        {_id: 4, a: [1, [1, 2]]},
        {_id: 5, a: [1, 2, 1]},
        {_id: 6, a: [1, null]},
        {_id: 7, a: [1, []]},

        // For these docs "a" resolves to a (logical) set that does _not_ contain value "1".
        {_id: 10, a: 2},
        {_id: 11, a: [[1], 2]},
        {_id: 12, a: [2, 3]},
        {_id: 13, a: {y: 1}},
        {_id: 14, no_a: 1},
    ];

    // When matching a scalar, local and foreign collections are fully symmetric.
    runTest_SingleForeignRecord({
        testDescription: "Top-level field in local and top-level scalar in foreign",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: 1},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level scalar in local and top-level field in foreign",
        localRecord: {_id: 0, b: 1},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
    });
})();

(function testMatchingPathToScalar() {
    const docs = [
        // For these docs "a.x" resolves to a (logical) set that contains value "1".
        {_id: 0, a: {x: 1, y: 2}},
        {_id: 1, a: [{x: 1}, {x: 2}]},
        {_id: 2, a: [{x: 1}, {x: null}]},
        {_id: 3, a: [{x: 1}, {x: []}]},
        {_id: 4, a: [{x: 1}, {no_x: 2}]},
        {_id: 5, a: {x: [1, 2]}},
        {_id: 6, a: [{x: [1, 2]}]},
        {_id: 7, a: [{x: [1, 2]}, {no_x: 2}]},

        // For these docs "a.x" should resolve to a (logical) set that does _not_ contain value "1".
        {_id: 10, a: {x: 2, y: 1}},
        {_id: 11, a: {x: [2, 3], y: 1}},
        {_id: 12, a: [{no_x: 1}, {x: 2}, {x: 3}]},
        {_id: 13, a: {x: [[1], 2]}},
        {_id: 14, a: [{x: [[1], 2]}]},
        {_id: 15, a: {no_x: 1}},
    ];

    // When matching a scalar, local and foreign collections are fully symmetric.
    runTest_SingleForeignRecord({
        testDescription: "Path in local and top-level scalar in foreign",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: 1},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level scalar in local and path in foreign",
        localRecord: {_id: 0, b: 1},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
    });
})();

(function testMatchingDeepPathToScalar() {
    const docs = [
        // For these docs "a.b.c" resolves to a (logical) set that contains value "1" (and possibly
        // other values)
        {_id: 0, a: {b: {c: 1}}},
        {_id: 1, a: {b: {c: [[2, 3], 1]}}},

        {_id: 2, a: {b: [{c: 2}, {c: 1}]}},
        {_id: 3, a: {b: [{c: null}, {c: 1}]}},
        {_id: 4, a: {b: [{no_c: 2}, {c: 1}]}},
        {_id: 5, a: {b: [{c: []}, {c: 1}]}},
        {_id: 6, a: {b: [{c: [[2, 3], 1]}]}},
        {_id: 7, a: {b: [{c: 1}, {c: [[2, 3], 4]}]}},

        {_id: 8, a: [{b: {c: 2}}, {b: {c: 1}}]},
        {_id: 9, a: [{b: {c: null}}, {b: {c: 1}}]},
        {_id: 10, a: [{b: {no_c: 2}}, {b: {c: 1}}]},
        {_id: 11, a: [{b: {c: []}}, {b: {c: 1}}]},
        {_id: 12, a: [{b: {c: [[2, 3], 1]}}]},
        {_id: 13, a: [{b: {c: 4}}, {b: {c: [[2, 3], 1]}}]},
        {_id: 14, a: [{no_b: 2}, {b: {c: 1}}]},

        {_id: 15, a: [{b: [{c: 1}]}]},
        {_id: 16, a: [{b: {c: 3}}, {b: [{c: 1}, {c: 2}]}]},
        {_id: 17, a: [{b: {c: null}}, {b: [{c: 1}]}]},
        {_id: 18, a: [{b: {no_c: 2}}, {b: [{c: 1}]}]},
        {_id: 19, a: [{b: {c: []}}, {b: [{c: 1}]}]},
        {_id: 20, a: [{b: [{c: [[2, 3], 1]}]}]},
        {_id: 21, a: [{b: {c: 4}}, {b: [{c: [[2, 3], 1]}]}]},
        {_id: 22, a: [{no_b: 2}, {b: [{no_c: 3}, {c: 1}]}]},

        // For these docs "a.b.c" should resolve to a (logical) set that does _not_ contain value
        // "1" (but might contain other values).
        {_id: 100, a: {b: {c: [[1, 2], 3]}}},
        {_id: 101, a: {b: [{c: [[1, 2], 3]}]}},
        {_id: 102, a: [{b: {c: [[1, 2], 3]}}]},
        {_id: 103, a: [{b: [{c: [[1, 2], 3]}]}]},
    ];

    // When matching a scalar, local and foreign collections are fully symmetric.
    runTest_SingleForeignRecord({
        testDescription: "Deep path in local and top-level scalar in foreign",
        localRecords: docs,
        localField: "a.b.c",
        foreignRecord: {_id: 0, key: 1},
        foreignField: "key",
        idsExpectedToMatch:
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level scalar in local and deep path in foreign",
        localRecord: {_id: 0, key: 1},
        localField: "key",
        foreignRecords: docs,
        foreignField: "a.b.c",
        idsExpectedToMatch:
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]
    });
})();

(function testMatchingTopLevelFieldToArray() {
    const docs = [
        // For these docs "a" resolves to a (logical) set that contains [1,2] array as a value.
        {_id: 0, a: [[1, 2], 3], y: 4},
        {_id: 1, a: [[1, 2]]},

        // For these docs "a.x" contains [1,2], 1 and 2 values when in foreign, but in local
        // the contained values are 1 and 2 (no array).
        {_id: 2, a: [1, 2], y: 3},

        // For these docs "a" resolves to a (logical) set that does _not_ contain [1,2] as a value
        // in neither local nor foreign but might contain "1" and/or "2".
        {_id: 10, a: [[[1, 2], 3], 4]},
        {_id: 11, a: [2, 1]},
        {_id: 12, a: [[2, 1], 3], y: [1, 2]},
        {_id: 13, a: [[2, 1], 3], y: [[1, 2], 3]},
        {_id: 14, a: null},
        {_id: 15, no_a: [1, 2]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Top-level field in local and top-level array in foreign",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: [1, 2]},
        foreignField: "b",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, /*match on 1: */ 2, 11]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level array in local and top-level field in foreign",
        localRecord: {_id: 0, b: [1, 2]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [/*match on 1: */ 2, 11]
    });

    runTest_SingleForeignRecord({
        testDescription: "Top-level field in local and nested array in foreign",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: [[1, 2], 42]},
        foreignField: "b",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1]
    });
    runTest_SingleLocalRecord({
        testDescription: "Nested array in local and top-level field in foreign",
        localRecord: {_id: 0, b: [[1, 2], 42]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2]
    });
})();

(function testMatchingPathToArray() {
    const docs = [
        // For these docs "a.x" resolves to a (logical) set that contains [1,2] array as a value.
        {_id: 0, a: {x: [[1, 2], 3], y: 4}},
        {_id: 1, a: [{x: [[1, 2], 3]}, {x: 4}]},
        {_id: 2, a: [{x: [[1, 2], 3]}, {x: null}]},
        {_id: 3, a: [{x: [[1, 2], 3]}, {no_x: 4}]},

        // For these docs "a.x" contains [1,2], 1 and 2 values when in foreign, but in local
        // the contained values are 1 and 2 (no array).
        {_id: 4, a: {x: [1, 2], y: 4}},
        {_id: 5, a: [{x: [1, 2]}, {x: 4}]},
        {_id: 6, a: [{x: [1, 2]}, {x: null}]},
        {_id: 7, a: [{x: [1, 2]}, {no_x: 4}]},

        // For these docs "a.x" resolves to a (logical) set that doesn't contain [1,2] as a value
        // in neither local nor foreign but might contain "1" and/or "2".
        {_id: 10, a: {x: [2, 1], y: [1, 2]}},
        {_id: 11, a: {x: [[2, 1], 3], y: [[1, 2], 3]}},
        {_id: 12, a: [{x: 1}, {x: 2}]},
        {_id: 13, a: {x: [[[1, 2], 3]]}},
        {_id: 14, a: [{x: [[[1, 2], 3]]}]},
        {_id: 15, a: {no_x: [[1, 2], 3]}},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Path in local and top-level array in foreign",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: [1, 2]},
        foreignField: "b",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2, 3, /*match on 1: */ 4, 5, 6, 7, 10, 12]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level array in local and path in foreign",
        localRecord: {_id: 0, b: [1, 2]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [/*match on 1: */ 4, 5, 6, 7, 10, 12]
    });

    runTest_SingleForeignRecord({
        testDescription: "Path in local and nested array in foreign",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: [[1, 2], 42]},
        foreignField: "b",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2, 3]
    });
    runTest_SingleLocalRecord({
        testDescription: "Nested array in local and path in foreign",
        localRecord: {_id: 0, b: [[1, 2], 42]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2, 3, 4, 5, 6, 7]
    });
})();

(function testMatchingMissingOnPath() {
    const docs = [
        // "a.x" does not exist.
        {_id: 0, a: {no_x: 1}},
        {_id: 1, a: {no_x: [1, 2]}},
        {_id: 2, a: [{no_x: 1}, {no_x: 2}]},
        {_id: 3, a: [{no_x: 2}, {x: 1}]},
        {_id: 4, a: [{no_x: [1, 2]}, {x: 1}]},
        {_id: 5, a: {x: null}},
        {_id: 6, a: [{x: null}, {x: 1}]},
        {_id: 7, a: [{x: null}, {x: [1]}]},
        {_id: 8, no_a: 1},
        {_id: 9, a: [1]},
        {_id: 10, a: []},
        {_id: 11, a: [[1]]},
        {_id: 12, a: [[]]},

        // "a.x" exists.
        {_id: 20, a: {x: 2, y: 1}},
        {_id: 21, a: [{x: 2}, {x: 3}]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Missing in local path and top-level null in foreign",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: null},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level null in local and missing in foreign path",
        localRecord: {_id: 0, b: null},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7, 8]
    });

    runTest_SingleForeignRecord({
        testDescription: "Missing in local path and top-level missing in foreign",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, no_b: 1},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level missing in local and missing in foreign path",
        localRecord: {_id: 0, no_b: 1},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7, 8]
    });
})();

(function testMatchingScalarsOnPath() {
    const docs = [
        {_id: 0, a: 1},
        {_id: 1, no_a: 1},
        {_id: 2, a: [1]},

        {_id: 3, a: {b: 1}},
        {_id: 4, a: {no_b: 1}},
        {_id: 5, a: {b: [1]}},

        {_id: 6, a: [{b: {no_c: 1}}, 1]},
        {_id: 7, a: {b: [{no_c: 1}, 1]}},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Scalars on local path and top-level null in foreign",
        localRecords: docs,
        localField: "a.b.c",
        foreignRecord: {_id: 0, b: null},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
    });
    runTest_SingleLocalRecord({
        testDescription: "Top-level null in local and scalars on path in foreign",
        localRecord: {_id: 0, b: null},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.b.c",
        idsExpectedToMatch: [0, 1, 3, 4, 6, 7]
    });
})();

(function testMatchingEmptyArrayInTopLevelField() {
    const docs = [
        // For these docs "a" resolves to a (logical) set that contains empty array as a value.
        {_id: 0, a: [[]]},
        {_id: 1, a: [[], 1]},
        {_id: 2, a: [[], null]},

        // For these docs "a" resolves to a (logical) set that contains empty array as a value in
        // foreign collection only.
        {_id: 3, a: []},

        // For these docs "a" key is either missing or contains null.
        {_id: 10, no_a: 1},
        {_id: 11, a: null},
        {_id: 12, a: [null]},
        {_id: 13, a: [null, 1]},

        // "a" doesn't contain neither empty array nor null.
        {_id: 20, a: 1},
        {_id: 21, a: [[[], 1], 2]},
        {_id: 22, a: [1, 2]},
        {_id: 23, a: [[null, 1], 2]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Empty top-level array in foreign, top field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: []},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2]
    });
    runTest_SingleLocalRecord({
        testDescription: "Empty top-level array in local, top field in foreign",
        localRecord: {_id: 0, b: []},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [2, 10, 11, 12, 13]
    });

    runTest_SingleForeignRecord({
        testDescription: "Empty nested array in foreign, top field in local",
        localRecords: docs,
        localField: "a",
        foreignRecord: {_id: 0, b: [[], 42]},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2]
    });
    runTest_SingleLocalRecord({
        testDescription: "Empty nested array in local, top field in foreign",
        localRecord: {_id: 0, b: [[], 42]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        idsExpectedToMatch: [0, 1, 2, 3]
    });
})();

(function testMatchingDeepPathWithEmptyArrays() {
    const docs = [
        {_id: 0, a: {b: {c: []}}},
        {_id: 1, a: {b: [{c: []}, {c: []}]}},
        {_id: 2, a: {b: [{no_c: 42}, {c: []}, {c: []}]}},
        {_id: 3, a: [{b: {c: []}}, {b: {c: []}}]},
        {_id: 4, a: [{b: {no_c: 42}}, {b: {c: []}}, {b: {c: []}}]},

        {_id: 5, a: {b: {c: [[]]}}},
        {_id: 6, a: {b: [{c: []}, {c: [[]]}]}},
        {_id: 7, a: {b: [{no_c: 42}, {c: []}, {c: [[]]}]}},
        {_id: 8, a: [{b: {c: []}}, {b: {c: [[]]}}]},
        {_id: 9, a: [{b: {no_c: 42}}, {b: {c: []}}, {b: {c: [[]]}}]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Empty arrays and missing on deep path in local",
        localRecords: docs,
        localField: "a.b.c",
        foreignRecord: {_id: 0, b: null},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4]
    });

    runTest_SingleForeignRecord({
        testDescription: "Empty arrays and missing on deep path in local",
        localRecords: docs,
        localField: "a.b.c",
        foreignRecord: {_id: 0, b: []},
        foreignField: "b",
        idsExpectedToMatch: [5, 6, 7, 8, 9]
    });
})();

(function testMatchingEmptyArrayValueOnPath() {
    const docs = [
        // For these docs "a.x" resolves to a (logical) set that contains empty array as a value.
        {_id: 0, a: {x: [[]], y: 1}},
        {_id: 1, a: [{x: [[]]}]},
        {_id: 2, a: [{x: [[]]}, {x: 1}]},
        {_id: 3, a: [{x: [[]]}, {x: null}]},
        {_id: 4, a: [{x: [[]]}, {no_x: 1}]},
        {_id: 5, a: {x: [[], 1]}},

        // For these docs "a.x" resolves to a (logical) set that contains empty array as a value in
        // foreign collection only.
        {_id: 10, a: {x: [], y: 1}},
        {_id: 11, a: [{x: []}, {x: 1}]},
        {_id: 12, a: [{x: []}, {x: null}]},
        {_id: 13, a: [{x: []}, {no_x: 1}]},

        // For these docs "a.x" key is either missing or contains null.
        {_id: 20, no_a: 1},
        {_id: 21, a: {no_x: 1}},
        {_id: 22, a: [{no_x: 1}, {no_x: 2}]},
        {_id: 23, a: [{x: null}, {x: 1}]},

        {_id: 30, a: []},
        {_id: 31, a: [1]},
        {_id: 32, a: [null, 1]},
        {_id: 33, a: [[]]},
        {_id: 34, a: [[1], 2]},

        // "a.x" doesn't contain neither empty array nor null.
        {_id: 40, a: {x: 1}},
        {_id: 41, a: {x: [[[], 1], 2]}},
        {_id: 42, a: {x: [1, 2]}},
        {_id: 43, a: {x: [[null, 1], 2]}},
        {_id: 44, a: [{x: 1}]},
        {_id: 45, a: [{x: [[[], 1], 2]}]},
        {_id: 46, a: [{x: [1, 2]}]},
        {_id: 47, a: [{x: [[null, 1], 2]}]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Empty top-level array in foreign, path in local",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: []},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
    });
    runTest_SingleLocalRecord({
        testDescription: "Empty top-level array in local, path in foreign",
        localRecord: {_id: 0, b: []},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [3, 4, 12, 13, 20, 21, 22, 23]
    });

    runTest_SingleForeignRecord({
        testDescription: "Empty nested array in foreign, path in local",
        localRecords: docs,
        localField: "a.x",
        foreignRecord: {_id: 0, b: [[], 42]},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
    });
    runTest_SingleLocalRecord({
        testDescription: "Empty nested array in local, path in foreign",
        localRecord: {_id: 0, b: [[], 42]},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.x",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 10, 11, 12, 13]
    });
})();

(function testMatchingPathWithNumericComponentToScalar() {
    const docs = [
        // For these docs "a.0.x" resolves to a (logical) set that contains value "1".
        {_id: 0, a: [{x: 1}, {x: 2}]},
        {_id: 1, a: [{x: [2, 3, 1]}]},
        {_id: 2, a: [{x: 1}, {y: 1}]},

        // For these docs "a.0.x" resolves to a (logical) set that does _not_ contain value "1".
        {_id: 10, a: [{x: 2}, {x: 1}]},
        {_id: 11, a: [{x: [2, 3]}]},
        {_id: 12, a: {x: 1}},
        {_id: 13, a: {x: [1, 2]}},
        {_id: 14, a: [{y: 1}, {x: 1}]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Scalar in foreign, path with numeral in local",
        localRecords: docs,
        localField: "a.0.x",
        foreignRecord: {_id: 0, b: 1},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2]
    });
    runTest_SingleLocalRecord({
        testDescription: "Scalar in local, path with numeral in foreign",
        localRecord: {_id: 0, b: 1},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.0.x",
        idsExpectedToMatch: [0, 1, 2]
    });
})();

(function testMatchingPathWithNumericComponentToNull() {
    const docs = [
        // For these docs "a.0.x" resolves to a (logical) set that contains value "null".
        {_id: 0, a: {x: 1}},
        {_id: 1, a: {x: [1, 2]}},
        {_id: 2, a: [{y: 1}, {x: 1}]},
        {_id: 3, a: [{x: null}, {x: 1}]},
        {_id: 4, a: [{x: [1, null]}, {x: 1}]},
        {_id: 5, a: [1, 2]},

        // For these docs "a.0.x" resolves to a (logical) set that does _not_ contain value "null".
        {_id: 10, a: [{x: 1}, {y: 1}]},
        {_id: 11, a: [{x: [1, 2]}]},
    ];

    runTest_SingleForeignRecord({
        testDescription: "Null in foreign, path with numeral in local",
        localRecords: docs,
        localField: "a.0.x",
        foreignRecord: {_id: 0, b: null},
        foreignField: "b",
        idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
    });
    runTest_SingleLocalRecord({
        testDescription: "Null in local, path with numeral in foreign",
        localRecord: {_id: 0, b: null},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a.0.x",
        idsExpectedToMatch: [0, 1, 2, 3, 4, /*SERVER-64221: */ 10, 11]
    });
})();
}());
