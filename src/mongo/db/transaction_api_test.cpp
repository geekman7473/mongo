/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/config.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/transaction_api.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/is_mongos.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

const BSONObj kOKCommandResponse = BSON("ok" << 1);

const BSONObj kOKInsertResponse = BSON("ok" << 1 << "n" << 1);

const BSONObj kResWithBadValueError = BSON("ok" << 0 << "code" << ErrorCodes::BadValue);

const BSONObj kNoSuchTransactionResponse =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction << kErrorLabelsFieldName
              << BSON_ARRAY(ErrorLabel::kTransientTransaction));

const BSONObj kWriteConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                               << "mock");
const BSONObj kResWithWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kWriteConcernError);

const BSONObj kRetryableWriteConcernError =
    BSON("code" << ErrorCodes::PrimarySteppedDown << "errmsg"
                << "mock");
const BSONObj kResWithRetryableWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kRetryableWriteConcernError);

class MockResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) {
        _timesYielded++;

        if (_skipNTimes > 0) {
            _skipNTimes--;
            return;
        }

        auto error = _yieldError.load();
        if (error != ErrorCodes::OK) {
            uasserted(error, "Simulated yield error");
        }
    }

    void unyield(OperationContext*) {
        _timesUnyielded++;

        if (_skipNTimes > 0) {
            _skipNTimes--;
            return;
        }

        auto error = _unyieldError.load();
        if (error != ErrorCodes::OK) {
            uasserted(error, "Simulated unyield error");
        }
    }

    void skipNTimes(int skip) {
        _skipNTimes = skip;
    }

    void throwInYield(ErrorCodes::Error error = ErrorCodes::Interrupted) {
        _yieldError.store(error);
    }

    void throwInUnyield(ErrorCodes::Error error = ErrorCodes::Interrupted) {
        _unyieldError.store(error);
    }

    auto timesYielded() {
        return _timesYielded;
    }

    auto timesUnyielded() {
        return _timesUnyielded;
    }

private:
    int _skipNTimes{0};
    int _timesYielded{0};
    int _timesUnyielded{0};
    AtomicWord<ErrorCodes::Error> _yieldError{ErrorCodes::OK};
    AtomicWord<ErrorCodes::Error> _unyieldError{ErrorCodes::OK};
};

namespace txn_api::details {

class MockTransactionClient : public SEPTransactionClient {
public:
    using SEPTransactionClient::SEPTransactionClient;

    virtual void injectHooks(std::unique_ptr<TxnMetadataHooks> hooks) override {
        _hooks = std::move(hooks);
    }

    virtual SemiFuture<BSONObj> runCommand(StringData dbName, BSONObj cmd) const override {
        auto cmdBob = BSONObjBuilder(cmd);
        invariant(_hooks);
        _hooks->runRequestHook(&cmdBob);
        _lastSentRequest = cmdBob.obj();

        auto nextResponse = _nextResponse;
        if (_secondResponse) {
            _nextResponse = *_secondResponse;
            _secondResponse.reset();
        }
        if (!nextResponse.isOK()) {
            return SemiFuture<BSONObj>::makeReady(nextResponse);
        }
        auto nextResponseRes = nextResponse.getValue();
        _hooks->runReplyHook(nextResponseRes);
        return SemiFuture<BSONObj>::makeReady(nextResponseRes);
    }

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual bool supportsClientTransactionContext() const override {
        return true;
    }

    BSONObj getLastSentRequest() {
        return _lastSentRequest;
    }

    void setNextCommandResponse(StatusWith<BSONObj> res) {
        _nextResponse = res;
    }

    void setSecondCommandResponse(StatusWith<BSONObj> res) {
        _secondResponse = res;
    }

private:
    std::unique_ptr<TxnMetadataHooks> _hooks;
    mutable StatusWith<BSONObj> _nextResponse{BSONObj()};
    mutable boost::optional<StatusWith<BSONObj>> _secondResponse;
    mutable BSONObj _lastSentRequest;
};

}  // namespace txn_api::details

namespace {

LogicalSessionId getLsid(BSONObj obj) {
    auto osi = OperationSessionInfo::parse("assertSessionIdMetadata"_sd, obj);
    ASSERT(osi.getSessionId());
    return *osi.getSessionId();
}

// TODO SERVER-64017: Make these consistent with the unified terms for internal sessions.
enum class LsidAssertion {
    kStandalone,
    kNonRetryableChild,
    kRetryableChild,
};
void assertSessionIdMetadata(BSONObj obj,
                             LsidAssertion lsidAssert,
                             boost::optional<LogicalSessionId> parentLsid = boost::none,
                             boost::optional<TxnNumber> parentTxnNumber = boost::none) {
    auto lsid = getLsid(obj);

    switch (lsidAssert) {
        case LsidAssertion::kStandalone:
            ASSERT(!getParentSessionId(lsid));
            break;
        case LsidAssertion::kNonRetryableChild:
            invariant(parentLsid);

            ASSERT(getParentSessionId(lsid));
            ASSERT_EQ(*getParentSessionId(lsid), *parentLsid);
            ASSERT(isInternalSessionForNonRetryableWrite(lsid));
            break;
        case LsidAssertion::kRetryableChild:
            invariant(parentTxnNumber);

            ASSERT(getParentSessionId(lsid));
            ASSERT_EQ(*getParentSessionId(lsid), *parentLsid);
            ASSERT(isInternalSessionForRetryableWrite(lsid));
            ASSERT(lsid.getTxnNumber());
            ASSERT_EQ(*lsid.getTxnNumber(), *parentTxnNumber);
            break;
    }
}

void assertTxnMetadata(BSONObj obj,
                       TxnNumber txnNumber,
                       boost::optional<bool> startTransaction,
                       boost::optional<BSONObj> readConcern = boost::none,
                       boost::optional<BSONObj> writeConcern = boost::none) {
    ASSERT_EQ(obj["lsid"].type(), BSONType::Object);
    ASSERT_EQ(obj["autocommit"].Bool(), false);
    ASSERT_EQ(obj["txnNumber"].Long(), txnNumber);

    if (startTransaction) {
        ASSERT_EQ(obj["startTransaction"].Bool(), *startTransaction);
    } else {
        ASSERT(obj["startTransaction"].eoo());
    }

    if (readConcern) {
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), *readConcern);
    } else if (startTransaction) {
        // If we didn't expect an explicit read concern, the startTransaction request should still
        // send the implicit default read concern.
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), repl::ReadConcernArgs::kImplicitDefault);
    } else {
        ASSERT(obj["readConcern"].eoo());
    }

    if (writeConcern) {
        ASSERT_BSONOBJ_EQ(obj["writeConcern"].Obj(), *writeConcern);
    } else {
        ASSERT(obj["writeConcern"].eoo());
    }
}

class TxnAPITest : public ServiceContextTest {
protected:
    void setUp() final {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();

        ThreadPool::Options options;
        options.poolName = "TxnAPITest";
        options.minThreads = 1;
        options.maxThreads = 1;

        _threadPool = std::make_shared<ThreadPool>(std::move(options));
        _threadPool->startup();

        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>(
            opCtx(), _threadPool, nullptr);
        _mockClient = mockClient.get();
        _txnWithRetries = std::make_unique<txn_api::TransactionWithRetries>(
            opCtx(), _threadPool, std::move(mockClient), nullptr /* resourceYielder */);
    }

    void tearDown() override {
        _threadPool->shutdown();
        _threadPool->join();
        _threadPool.reset();
    }

    void waitForAllEarlierTasksToComplete() {
        _threadPool->waitForIdle();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    txn_api::details::MockTransactionClient* mockClient() {
        return _mockClient;
    }

    MockResourceYielder* resourceYielder() {
        return _resourceYielder;
    }

    txn_api::TransactionWithRetries& txnWithRetries() {
        return *_txnWithRetries;
    }

    void resetTxnWithRetries(std::unique_ptr<MockResourceYielder> resourceYielder = nullptr) {
        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>(
            opCtx(), _threadPool, nullptr);
        _mockClient = mockClient.get();
        if (resourceYielder) {
            _resourceYielder = resourceYielder.get();
        }

        // Reset _txnWithRetries so it returns and reacquires the same session from the session
        // pool. This ensures that we can predictably monitor txnNumber's value.
        _txnWithRetries = nullptr;
        _txnWithRetries = std::make_unique<txn_api::TransactionWithRetries>(
            opCtx(), _threadPool, std::move(mockClient), std::move(resourceYielder));
    }

    void expectSentAbort(TxnNumber txnNumber, BSONObj writeConcern) {
        auto lastRequest = mockClient()->getLastSentRequest();
        assertTxnMetadata(lastRequest,
                          txnNumber,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::shared_ptr<ThreadPool> _threadPool;
    txn_api::details::MockTransactionClient* _mockClient{nullptr};
    MockResourceYielder* _resourceYielder{nullptr};
    std::unique_ptr<txn_api::TransactionWithRetries> _txnWithRetries;
};

TEST_F(TxnAPITest, OwnSession_AttachesTxnMetadata) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes = txnClient
                            .runCommand("user"_sd,
                                        BSON("insert"
                                             << "foo"
                                             << "documents" << BSON_ARRAY(BSON("x" << 1))))
                            .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnCommit) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}},
        WriteConcernOptions{
            "majority", WriteConcernOptions::SyncMode::FSYNC, WriteConcernOptions::kNoTimeout},
        WriteConcernOptions{
            2, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoWaiting}};

    auto txnNumber{0};
    for (const auto& writeConcern : writeConcernOptions) {

        opCtx()->setWriteConcern(writeConcern);

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        int attempt = -1;
        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;
                // No write concern on requests prior to commit/abort.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt releases and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  true /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);


                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand("user"_sd,
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  boost::none /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Throw a transient error to verify the retries behavior.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();

        // Each attempt returns and reacquires the session from the session pool, incrementing the
        // txnNumber.
        assertTxnMetadata(lastRequest,
                          attempt + txnNumber /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern.toBSON());
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        txnNumber += attempt;
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnAbort) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}},
        WriteConcernOptions{
            "majority", WriteConcernOptions::SyncMode::FSYNC, WriteConcernOptions::kNoTimeout},
        WriteConcernOptions{
            2, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoWaiting}};

    auto txnNumber{0};
    for (const auto& writeConcern : writeConcernOptions) {
        opCtx()->setWriteConcern(writeConcern);

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                mockClient()->setSecondCommandResponse(
                    kOKCommandResponse);  // Best effort abort response.

                uasserted(ErrorCodes::InternalError, "Mock error");
                return SemiFuture<void>::makeReady();
            });
        ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

        expectSentAbort(txnNumber /* txnNumber */, writeConcern.toBSON());
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesReadConcernOnStartTransaction) {
    const std::vector<repl::ReadConcernLevel> readConcernLevels = {
        repl::ReadConcernLevel::kLocalReadConcern,
        repl::ReadConcernLevel::kMajorityReadConcern,
        repl::ReadConcernLevel::kSnapshotReadConcern};

    auto txnNumber{0};
    for (const auto& readConcernLevel : readConcernLevels) {
        auto readConcern = repl::ReadConcernArgs(readConcernLevel);
        repl::ReadConcernArgs::get(opCtx()) = readConcern;

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        int attempt = -1;
        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  true /* startTransaction */,
                                  readConcern.toBSONInner());
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Subsequent requests shouldn't have a read concern.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand("user"_sd,
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  boost::none /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Throw a transient error to verify the retry will still use the read concern.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();

        // Each attempt returns and reacquires the session from the session pool, incrementing the
        // txnNumber.
        assertTxnMetadata(lastRequest,
                          attempt + txnNumber /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          WriteConcernOptions().toBSON() /* writeConcern */);
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        txnNumber += attempt;
    }
}

TEST_F(TxnAPITest, OwnSession_AbortsOnError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            // The best effort abort response, the client should ignore this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            uasserted(ErrorCodes::InternalError, "Mock error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    expectSentAbort(0 /* txnNumber */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_SkipsCommitIfNoCommandsWereRun) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            // The commit response, the client should not receive this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            uasserted(ErrorCodes::InternalError, "Mock error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT(lastRequest.isEmpty());
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */, WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(attempt == 0 ? kNoSuchTransactionResponse
                                                              : kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            // The commit or implicit abort response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            if (attempt == 0) {
                uassertStatusOK(getStatusFromWriteCommandReply(insertRes));
            } else {
                ASSERT_OK(getStatusFromWriteCommandReply(insertRes));
            }

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientClientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */, WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit or implicit abort response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_CommitError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InternalError));

            // The best effort abort response, the client should ignore this.
            mockClient()->setSecondCommandResponse(kResWithBadValueError);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT_EQ(swResult.getValue().cmdStatus, ErrorCodes::InternalError);
    ASSERT(swResult.getValue().wcError.toStatus().isOK());
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::InternalError);

    expectSentAbort(0 /* txnNumber */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_TransientCommitError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */, WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // Set commit and best effort abort response, if necessary.
            if (attempt == 0) {
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
                mockClient()->setSecondCommandResponse(kOKCommandResponse);
            } else {
                mockClient()->setNextCommandResponse(kOKCommandResponse);
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InterruptedDueToReplStateChange));
            mockClient()->setSecondCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_NonRetryableCommitWCError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(kResWithWriteConcernError);
            mockClient()->setSecondCommandResponse(
                kOKCommandResponse);  // Best effort abort response.
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().cmdStatus.isOK());
    ASSERT_EQ(swResult.getValue().wcError.toStatus(), ErrorCodes::WriteConcernFailed);
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::WriteConcernFailed);

    expectSentAbort(0 /* txnNumber */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitWCError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit responses.
            mockClient()->setNextCommandResponse(kResWithRetryableWriteConcernError);
            mockClient()->setSecondCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, RunSyncNoErrors) {
    txnWithRetries().runSync(opCtx(),
                             [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                                 return SemiFuture<void>::makeReady();
                             });
}

TEST_F(TxnAPITest, RunSyncThrowsOnBodyError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               uasserted(ErrorCodes::InternalError, "Mock error");
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunSyncThrowsOnCommitCmdError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               mockClient()->setNextCommandResponse(kOKInsertResponse);
                               auto insertRes = txnClient
                                                    .runCommand("user"_sd,
                                                                BSON("insert"
                                                                     << "foo"
                                                                     << "documents"
                                                                     << BSON_ARRAY(BSON("x" << 1))))
                                                    .get();

                               // The commit response.
                               mockClient()->setNextCommandResponse(
                                   BSON("ok" << 0 << "code" << ErrorCodes::InternalError));
                               mockClient()->setSecondCommandResponse(
                                   kOKCommandResponse);  // Best effort abort response.
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunSyncThrowsOnCommitWCError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               mockClient()->setNextCommandResponse(kOKInsertResponse);
                               auto insertRes = txnClient
                                                    .runCommand("user"_sd,
                                                                BSON("insert"
                                                                     << "foo"
                                                                     << "documents"
                                                                     << BSON_ARRAY(BSON("x" << 1))))
                                                    .get();

                               // The commit response.
                               mockClient()->setNextCommandResponse(kResWithWriteConcernError);
                               mockClient()->setSecondCommandResponse(
                                   kOKCommandResponse);  // Best effort abort response.
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::WriteConcernFailed);
}

TEST_F(TxnAPITest, HandlesExceptionWhileYieldingDuringBody) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());
    resourceYielder()->throwInYield();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // 2 yields in body and abort but no unyields because both yields throw.
    ASSERT_EQ(resourceYielder()->timesYielded(), 2);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 0);
}

TEST_F(TxnAPITest, HandlesExceptionWhileUnyieldingDuringBody) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());
    resourceYielder()->throwInUnyield();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // 2 yields in body and abort and both corresponding unyields.
    ASSERT_EQ(resourceYielder()->timesYielded(), 2);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 2);
}

TEST_F(TxnAPITest, HandlesExceptionWhileYieldingDuringCommit) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->throwInYield();

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // 3 yields in body, commit, and best effort abort. Only unyield in body because the other
    // yields throw.
    ASSERT_EQ(resourceYielder()->timesYielded(), 3);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 1);
}

TEST_F(TxnAPITest, HandlesExceptionWhileUnyieldingDuringCommit) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->skipNTimes(1);  // Skip the unyield when the body is finished.
            resourceYielder()->throwInUnyield();

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // 3 yields in body, commit, and best effort abort and all of their corresponding unyields.
    ASSERT_EQ(resourceYielder()->timesYielded(), 3);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 3);
}

TEST_F(TxnAPITest, HandlesExceptionWhileYieldingDuringAbort) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->throwInYield(ErrorCodes::Interrupted);

            // No abort response necessary because the yielder throws before sending the command.
            uasserted(ErrorCodes::InternalError, "Simulated body error");
            return SemiFuture<void>::makeReady();
        });

    // The transaction should fail with the original error instead of the ResourceYielder error.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);
    // 2 yields in body and best effort abort. Only unyield in body because the other yield throws.
    ASSERT_EQ(resourceYielder()->timesYielded(), 2);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 1);
}

TEST_F(TxnAPITest, UnyieldsAfterCancellation) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    unittest::Barrier txnApiStarted(2);
    unittest::Barrier opCtxKilled(2);

    auto killerThread = stdx::thread([&txnApiStarted, &opCtxKilled, opCtx = opCtx()] {
        txnApiStarted.countDownAndWait();
        opCtx->markKilled();
    });

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->throwInUnyield(ErrorCodes::InternalError);

            txnApiStarted.countDownAndWait();
            opCtxKilled.countDownAndWait();

            return SemiFuture<void>::makeReady();
        });

    // The transaction should fail with an Interrupted error from killing the opCtx using the
    // API instead of the ResourceYielder error from within the API callback.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // 2 yields in body and best effort abort and both of their corresponding unyields.
    ASSERT_EQ(resourceYielder()->timesYielded(), 2);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 2);

    opCtxKilled.countDownAndWait();
    killerThread.join();

    // Wait until the API callback has completed before letting the barriers go out of scope.
    waitForAllEarlierTasksToComplete();
}

TEST_F(TxnAPITest, HandlesExceptionWhileUnyieldingDuringAbort) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->skipNTimes(1);  // Skip the unyield when the body is finished.
            resourceYielder()->throwInUnyield(ErrorCodes::Interrupted);

            // Best effort abort response.
            mockClient()->setSecondCommandResponse(kOKCommandResponse);
            uasserted(ErrorCodes::InternalError, "Simulated body error");
            return SemiFuture<void>::makeReady();
        });

    // The transaction should fail with the original error instead of the ResourceYielder error.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);
    // 2 yields in body and best effort abort and both of their corresponding unyields.
    ASSERT_EQ(resourceYielder()->timesYielded(), 2);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 2);
}

TEST_F(TxnAPITest, ClientSession_UsesNonRetryableInternalSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    resetTxnWithRetries();

    int attempt = -1;
    boost::optional<LogicalSessionId> firstAttemptLsid;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                    LsidAssertion::kNonRetryableChild,
                                    opCtx()->getLogicalSessionId());

            if (attempt == 0) {
                firstAttemptLsid = getLsid(mockClient()->getLastSentRequest());
                // Trigger transient error retry to verify the same session is used by the retry.
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
            } else {
                ASSERT_EQ(*firstAttemptLsid, getLsid(mockClient()->getLastSentRequest()));
                mockClient()->setNextCommandResponse(kOKCommandResponse);  // Commit response.
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                            LsidAssertion::kNonRetryableChild,
                            opCtx()->getLogicalSessionId());
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, ClientRetryableWrite_UsesRetryableInternalSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    resetTxnWithRetries();

    int attempt = -1;
    boost::optional<LogicalSessionId> firstAttemptLsid;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents"
                                                  << BSON_ARRAY(BSON("x" << 1))
                                                  // Retryable transactions must include stmtIds for
                                                  // retryable write commands.
                                                  << "stmtIds" << BSON_ARRAY(1)))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                    LsidAssertion::kRetryableChild,
                                    opCtx()->getLogicalSessionId(),
                                    opCtx()->getTxnNumber());

            // Verify a non-retryable write command does not need to include stmtIds.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            auto findRes = txnClient
                               .runCommand("user"_sd,
                                           BSON("find"
                                                << "foo"))
                               .get();
            ASSERT(findRes["ok"]);  // Verify the mocked response was returned.

            // Verify the alternate format for stmtIds is allowed.
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes =
                txnClient
                    .runCommand("user"_sd,
                                BSON("insert"
                                     << "foo"
                                     << "documents" << BSON_ARRAY(BSON("x" << 1)) << "stmtId" << 1))
                    .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            if (attempt == 0) {
                firstAttemptLsid = getLsid(mockClient()->getLastSentRequest());
                // Trigger transient error retry to verify the same session is used by the retry.
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
            } else {
                ASSERT_EQ(*firstAttemptLsid, getLsid(mockClient()->getLastSentRequest()));
                mockClient()->setNextCommandResponse(kOKCommandResponse);  // Commit response.
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                            LsidAssertion::kRetryableChild,
                            opCtx()->getLogicalSessionId(),
                            opCtx()->getTxnNumber());
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_F(TxnAPITest,
             ClientRetryableWrite_RetryableWriteWithoutStmtIdCrashesOnDebug,
             "In a retryable write transaction every retryable write command should") {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::duplicateCodeForTest(6410500));
}
#endif

TEST_F(TxnAPITest, ClientTransaction_UsesClientTransactionOptionsAndDoesNotCommitOnSuccess) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    // No commit should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotAppendStartTransactionFields) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();

    auto readConcern = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(opCtx()) = readConcern;

    auto writeConcernOptions =
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}};
    opCtx()->setWriteConcern(writeConcernOptions);

    resetTxnWithRetries();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */,
                              boost::none /* readConcern */,
                              boost::none /* writeConcern */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    // No commit should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotBestEffortAbortOnFailure) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            // Trigger mock error retry to verify the API does not best effort abort.
            uasserted(ErrorCodes::InternalError, "Mock error");

            return SemiFuture<void>::makeReady();
        });
    // The error should have been propagated.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    // No best effort abort should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotRetryOnTransientErrors) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            // Trigger transient error retry to verify the API does not retry.
            uasserted(ErrorCodes::HostUnreachable, "Mock network error");

            return SemiFuture<void>::makeReady();
        });
    // The transient error should have been propagated.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::HostUnreachable);

    // No best effort abort should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, OwnSession_HandleErrorRetryCommitOnNetworkError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                Status(ErrorCodes::HostUnreachable, "Host Unreachable"));
            mockClient()->setSecondCommandResponse(kOKCommandResponse);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, TestExhaustiveFindWithSingleBatch) {
    FindCommandRequest findCommand(NamespaceString("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(true);

    const long long cursorId = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);

    mockClient()->setNextCommandResponse(findResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFind(findCommand).get();

    ASSERT_EQ(exhaustiveFindRes.size(), 1);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[0], firstBatchDoc);

    // Check that we only ran the find command and no follow up getMores.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["find"].String(), "bar");
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["singleBatch"].Bool(), true);
}

TEST_F(TxnAPITest, TestExhaustiveFindWithMultipleBatches) {
    FindCommandRequest findCommand(NamespaceString("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(false);

    const long long cursorId = 1;
    const long long cursorIdNext = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    BSONObj nextBatchDoc = BSON("_id" << 1);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);
    auto getMoreResponse = BSON("cursor" << BSON("id" << cursorIdNext << "ns"
                                                      << "foo.bar"
                                                      << "nextBatch" << BSON_ARRAY(nextBatchDoc))
                                         << "ok" << 1);

    mockClient()->setNextCommandResponse(findResponse);
    mockClient()->setSecondCommandResponse(getMoreResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFind(findCommand).get();

    ASSERT_EQ(exhaustiveFindRes.size(), 2);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[0], firstBatchDoc);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[1], nextBatchDoc);

    // Check that getMore was run.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["getMore"].Long(), 1);
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["collection"].String(), "bar");
}

TEST_F(TxnAPITest, TestExhaustiveFindErrorOnFind) {
    FindCommandRequest findCommand(NamespaceString("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(true);

    const long long cursorId = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);

    auto badFindResponse = BSON("ok" << 0 << "code" << ErrorCodes::HostUnreachable);
    mockClient()->setNextCommandResponse(badFindResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFind(findCommand).getNoThrow();

    ASSERT_EQ(exhaustiveFindRes.getStatus(), ErrorCodes::HostUnreachable);

    // Check that we only ran the find command and no follow up getMores.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["find"].String(), "bar");
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["singleBatch"].Bool(), true);
}

TEST_F(TxnAPITest, TestExhaustiveFindErrorOnGetMore) {
    FindCommandRequest findCommand(NamespaceString("foo.bar"));
    findCommand.setBatchSize(2);
    findCommand.setSingleBatch(false);

    const long long cursorId = 1;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    BSONObj nextBatchDoc = BSON("_id" << 1);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);
    auto badGetMoreResponse = BSON("ok" << 0 << "code" << ErrorCodes::HostUnreachable);

    mockClient()->setNextCommandResponse(findResponse);
    mockClient()->setSecondCommandResponse(badGetMoreResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFind(findCommand).getNoThrow();

    ASSERT_EQ(exhaustiveFindRes.getStatus(), ErrorCodes::HostUnreachable);

    // Check that getMore was run.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["getMore"].Long(), 1);
    ASSERT_EQ(lastRequest["batchSize"].Long(), 2);
    ASSERT_EQ(lastRequest["collection"].String(), "bar");
}
}  // namespace
}  // namespace mongo
