/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <queue>

#include "TestUtil.h"
#include "BackupSelector.h"
#include "Memory.h"
#include "ReplicatedSegment.h"
#include "ShortMacros.h"
#include "TaskQueue.h"
#include "TransportManager.h"

namespace RAMCloud {

/**
 * backups.push_back Backups into this selector and the select methods will
 * replay them.  No bounds checking done, use it carefully.
 */
struct MockBackupSelector : public BaseBackupSelector {
    explicit MockBackupSelector(size_t count)
        : backups()
        , nextIndex(0)
    {
        makeSimpleHostList(count);
    }

    ServerId selectPrimary(uint32_t numBackups, const ServerId backupIds[]) {
        return selectSecondary(numBackups, backupIds);
    }

    ServerId selectSecondary(uint32_t numBackups, const ServerId backupIds[]) {
        assert(backups.size());
        for (uint32_t i = 0; i < numBackups; ++i)
            TEST_LOG("conflicting backupId: %lu", *backupIds[i]);
        ServerId backup = backups[nextIndex++];
        nextIndex %= backups.size();
        return backup;
    }

    void makeSimpleHostList(size_t count) {
        for (uint32_t i = 0; i < count; ++i)
            backups.push_back(ServerId(i, 0));
    }

    std::vector<ServerId> backups;
    size_t nextIndex;
};

struct CountingDeleter : public ReplicatedSegment::Deleter {
    CountingDeleter()
        : count(0) {}

    void destroyAndFreeReplicatedSegment(ReplicatedSegment*
                                            replicatedSegment) {
        ++count;
    }

    uint32_t count;
};

struct ReplicatedSegmentTest : public ::testing::Test {
    enum { DATA_LEN = 100 };
    enum { MAX_BYTES_PER_WRITE = 21 };
    Context context;
    TaskQueue taskQueue;
    ServerList serverList;
    BackupTracker tracker;
    CountingDeleter deleter;
    uint32_t writeRpcsInFlight;
    std::mutex dataMutex;
    const ServerId masterId;
    const uint64_t segmentId;
    MinOpenSegmentId minOpenSegmentId;
    char data[DATA_LEN];
    const uint32_t openLen;
    const uint32_t numReplicas;
    MockBackupSelector backupSelector;
    MockTransport transport;
    TransportManager::MockRegistrar mockRegistrar;
    std::unique_ptr<ReplicatedSegment> segment;
    ServerId backupId1;
    ServerId backupId2;

    ReplicatedSegmentTest()
        : context()
        , taskQueue()
        , serverList(context)
        , tracker(context, serverList, NULL)
        , deleter()
        , writeRpcsInFlight(0)
        , dataMutex()
        , masterId(999, 0)
        , segmentId(888)
        , minOpenSegmentId(context, &taskQueue, &masterId)
        , data()
        , openLen(10)
        , numReplicas(2)
        , backupSelector(numReplicas)
        , transport(context)
        , mockRegistrar(context, transport)
        , segment(NULL)
        , backupId1(0, 0)
        , backupId2(1, 0)
    {
        Logger::get().setLogLevels(SILENT_LOG_LEVEL);
        context.serverList = &serverList;

        segment = newSegment(NULL, segmentId);
        serverList.add(backupId1, "mock:host=backup1",
                       {BACKUP_SERVICE}, 100);
        serverList.add(backupId2, "mock:host=backup2",
                       {BACKUP_SERVICE}, 100);
        ServerDetails server;
        ServerChangeEvent event;
        tracker.getChange(server, event);
        tracker.getChange(server, event);

        const char* msg = "abcedfghijklmnopqrstuvwxyz";
        size_t msgLen = strlen(msg);
        for (int i = 0; i < DATA_LEN; ++i)
            data[i] = msg[i % msgLen];
    }


    std::unique_ptr<ReplicatedSegment>
    newSegment(ReplicatedSegment* precedingSegment, uint64_t segmentId) {
        void* segMem = operator new(ReplicatedSegment::sizeOf(numReplicas));
        auto newHead = std::unique_ptr<ReplicatedSegment>(
                new(segMem) ReplicatedSegment(context, taskQueue, tracker,
                                              backupSelector,
                                              deleter, writeRpcsInFlight,
                                              minOpenSegmentId,
                                              dataMutex, true,
                                              masterId, segmentId,
                                              data, openLen, numReplicas,
                                              MAX_BYTES_PER_WRITE));
        // Set up ordering constraints between this new segment and the prior
        // one in the log.
        if (precedingSegment) {
            precedingSegment->followingSegment = newHead.get();
            newHead->precedingSegmentCloseAcked =
                precedingSegment->getAcked().close;
            newHead->precedingSegmentOpenAcked =
                precedingSegment->getAcked().open;
        }
        return newHead;
    }

    void reset() {
        taskQueue.tasks.pop();
        segment->scheduled = false;
    }

    DISALLOW_COPY_AND_ASSIGN(ReplicatedSegmentTest);
};

TEST_F(ReplicatedSegmentTest, varLenArrayAtEnd) {
    // replicas[0] must be the last member of ReplicatedSegment
    EXPECT_EQ(static_cast<void*>(segment.get() + 1),
              static_cast<void*>(&segment.get()->replicas[0].isActive));
    reset();
}

TEST_F(ReplicatedSegmentTest, constructor) {
    EXPECT_EQ(openLen, segment->queued.bytes);
    EXPECT_TRUE(segment->queued.open);
    ASSERT_FALSE(taskQueue.isIdle());
    EXPECT_EQ(segment.get(), taskQueue.tasks.front());
    reset();
}

TEST_F(ReplicatedSegmentTest, free) {
    segment->write(openLen);
    segment->close();
    segment->free();
    EXPECT_TRUE(segment->freeQueued);
    EXPECT_TRUE(segment->isScheduled());
    ASSERT_FALSE(taskQueue.isIdle());
    EXPECT_EQ(segment.get(), taskQueue.tasks.front());
    reset();
}

TEST_F(ReplicatedSegmentTest, freeWriteRpcInProgress) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    segment->write(openLen);
    segment->close();
    taskQueue.performTask(); // writeRpc created
    segment->free();

    // make sure the backup "free" opcode was not sent
    EXPECT_TRUE(TestUtil::doesNotMatchPosixRegex("0x1001c",
                                                 transport.outputLog));
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc); // ensure the write completed
    EXPECT_FALSE(segment->replicas[0].freeRpc);
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask();
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].freeRpc); // ensure free gets sent
    EXPECT_TRUE(segment->isScheduled());

    reset();
}

TEST_F(ReplicatedSegmentTest, close) {
    reset();
    segment->close();
    EXPECT_TRUE(segment->isScheduled());
    ASSERT_FALSE(taskQueue.isIdle());
    EXPECT_EQ(segment.get(), taskQueue.tasks.front());
    EXPECT_TRUE(segment->queued.close);
    reset();
}

TEST_F(ReplicatedSegmentTest, handleBackupFailureWhileOpen) {
    EXPECT_FALSE(segment->handleBackupFailure({0, 0}));
    foreach (auto& replica, segment->replicas)
        EXPECT_FALSE(replica.replicateAtomically);

    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    segment->write(openLen);
    // Not active still, next performTask chooses backups and sends opens.
    EXPECT_FALSE(segment->handleBackupFailure({0, 0}));
    foreach (auto& replica, segment->replicas)
        EXPECT_FALSE(replica.replicateAtomically);

    taskQueue.performTask();
    // Replicas are now active with an outstanding open rpc.
    EXPECT_FALSE(segment->handleBackupFailure({88888, 0}));
    foreach (auto& replica, segment->replicas)
        EXPECT_FALSE(replica.replicateAtomically);

    EXPECT_TRUE(segment->handleBackupFailure({0, 0}));
    // The failed replica must restart replication in atomic mode.
    EXPECT_TRUE(segment->replicas[0].replicateAtomically);
    // The other open replica is in normal (non-atomic) mode still.
    EXPECT_FALSE(segment->replicas[1].replicateAtomically);

    // Failure of the second replica.
    EXPECT_FALSE(segment->handleBackupFailure({1, 0}));
    EXPECT_TRUE(segment->replicas[0].replicateAtomically);
    EXPECT_TRUE(segment->replicas[1].replicateAtomically);

    reset();
}

TEST_F(ReplicatedSegmentTest, handleBackupFailureWhileHandlingFailure) {
    transport.setInput("0 0"); // open
    transport.setInput("0 0"); // open
    segment->write(openLen);
    taskQueue.performTask(); // send opens
    taskQueue.performTask(); // reap opens

    transport.setInput("0 0"); // close
    transport.setInput("0 0"); // close
    segment->close();
    taskQueue.performTask(); // send closes
    taskQueue.performTask(); // reap closes

    // Handle failure while closed.  This should technically
    // "reopen" the segment, though only in atomic replication mode.
    EXPECT_FALSE(segment->handleBackupFailure({0, 0}));
    EXPECT_TRUE(segment->replicas[0].replicateAtomically);
    EXPECT_FALSE(segment->replicas[1].replicateAtomically);

    // Check to make sure that atomic replications aren't counted as
    // failures while open.  They can't threaten the integrity of the log.
    EXPECT_FALSE(segment->handleBackupFailure({0, 0}));
    EXPECT_TRUE(segment->replicas[0].replicateAtomically);
    EXPECT_FALSE(segment->replicas[1].replicateAtomically);

    reset();
}

TEST_F(ReplicatedSegmentTest, sync) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    segment->sync(segment->queued.bytes); // first sync sends the opens
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij | "
              "sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij",
               transport.outputLog);
    transport.outputLog = "";
    EXPECT_TRUE(segment->getAcked().open);
    EXPECT_EQ(openLen, segment->getAcked().bytes);

    segment->sync(segment->queued.bytes); // second sync doesn't send anything
    EXPECT_EQ("", transport.outputLog);
    transport.outputLog = "";
    EXPECT_EQ(openLen, segment->getAcked().bytes);

    segment->write(openLen + 10);
    segment->sync(openLen); // doesn't send anything
    EXPECT_EQ("", transport.outputLog);
    transport.outputLog = "";
    segment->sync(openLen + 1); // will wait until after the next send
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 10 10 0 klmnopqrst | "
              "sendRequest: 0x10020 999 0 888 0 10 10 0 klmnopqrst",
               transport.outputLog);
    transport.outputLog = "";
    EXPECT_EQ(openLen + 10, segment->getAcked().bytes);
}

TEST_F(ReplicatedSegmentTest, syncDoubleCheckCrossSegmentOrderingConstraints) {
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    auto newHead = newSegment(segment.get(), segmentId + 1);
    segment->close(); // close queued
    newHead->write(openLen + 10); // write queued

    // Mess up the queue order to simulate reorder due to failure.
    // This means newHead will be going first at 'taking turns'
    // with segment.
    {
        Task* t = taskQueue.tasks.front();
        taskQueue.tasks.pop();
        taskQueue.tasks.push(t);
    }

    // Queued order of ops would be:
    // open newHead
    // open segment
    // write newHead
    // close segment

    // Required by constraints (and checked here):
    // open segment
    // open newHead (open order is enforced to prevent missing replicas)
    // close segment
    // write newHead

    EXPECT_EQ("", transport.outputLog);
    newHead->sync(newHead->queued.bytes);

    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij | "
              "sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij | "
              "sendRequest: 0x10020 999 0 889 0 0 10 5 abcedfghij | "
              "sendRequest: 0x10020 999 0 889 0 0 10 1 abcedfghij | "
              "sendRequest: 0x10020 999 0 888 0 10 0 2 | "
              "sendRequest: 0x10020 999 0 888 0 10 0 2 | "
              "sendRequest: 0x10020 999 0 889 0 10 10 0 klmnopqrst | "
              "sendRequest: 0x10020 999 0 889 0 10 10 0 klmnopqrst",
              transport.outputLog);
}

namespace {
bool filter(string s) {
    return s != "checkStatus";
}
}

TEST_F(ReplicatedSegmentTest, syncRecoveringFromLostOpenReplicas) {
    // Generates a segment, syncs some data to it, and then simulates
    // a crash while that segment is still open.  Checks to make sure
    // that the originally synced data appears unsynced while the
    // recovery is going on and that after recovery has happened the
    // data appears synced again.
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // open
    transport.setInput("0 0"); // write/close
    transport.setInput("0 0"); // setOpenMinSegmentId

    segment->sync(segment->queued.bytes); // first sync sends the opens
    transport.outputLog = "";
    segment->sync(segment->queued.bytes); // second sync sends nothing
    EXPECT_EQ("", transport.outputLog);
    transport.outputLog = "";
    EXPECT_EQ(openLen, segment->getAcked().bytes);
    EXPECT_EQ(backupId1, segment->replicas[0].backupId);
    EXPECT_EQ(backupId2, segment->replicas[1].backupId);
    EXPECT_FALSE(segment->getAcked().close);

    // Now the open segment encounters a failure.
    IGNORE_RESULT(segment->handleBackupFailure({0, 0}));
    EXPECT_TRUE(segment->recoveringFromLostOpenReplicas);
    EXPECT_FALSE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].replicateAtomically);
    EXPECT_TRUE(segment->replicas[1].isActive);
    EXPECT_FALSE(segment->replicas[1].replicateAtomically);
    EXPECT_EQ(backupId2, segment->replicas[1].backupId);
    // Failure handling code needs to roll over to a new log head.
    // Usually the close would need to wait on the open of the next
    // log segment, but skip that here, pretend it already happened.
    segment->close();

    EXPECT_EQ(0lu, minOpenSegmentId.current);
    // Will drive recovery (open a replica elsewhere, set minOpenSegmentId).
    EXPECT_FALSE(segment->getAcked().close);
    // Notice: weird and special case that only happens during testing:
    // The replica gets recreated right back on the backup with id 0
    // because it isn't removed from the ServerList that the
    // BackupSelector is working of off.
    TestLog::Enable _(filter);
    segment->sync(segment->queued.bytes);
    EXPECT_TRUE(segment->getAcked().close);
    // Fragile test log check, but left here because the output is pretty
    // reassuring to a human reader that the test does what one expects.
    EXPECT_EQ("sync: syncing | "
              "selectSecondary: conflicting backupId: 999 | "
              "selectSecondary: conflicting backupId: 1 | "
              "performWrite: Starting replication of segment 888 replica "
                  "slot 0 on backup 0 | "
              "performWrite: Sending open to backup 0 | "
              "performWrite: Sending write to backup 1 | "
              "performWrite: Sending write to backup 0 | "
              "performTask: Updating minOpenSegmentId on coordinator to "
                  "ensure lost replicas of segment 888 will not be reused | "
              "updateToAtLeast: request update to minOpenSegmentId for "
                  "999 to 889 | "
              "performTask: minOpenSegmentId ok, lost open replica recovery "
              "complete on segment 888",
              TestLog::get());
    // Three rpcs are sent out to rereplicate the lost replica.
    // Notice weird "atomic" write flags two of them (high-order bits
    // glommed together with the flags field).
    EXPECT_EQ(// Opening primary write, marked atomic.
              "sendRequest: 0x10020 999 0 888 0 0 10 261 abcedfghij | "
              // Closing write to non-crashed replica, no atomic marker.
              "sendRequest: 0x10020 999 0 888 0 10 0 2 | "
              // Closing write, marked atomic.
              "sendRequest: 0x10020 999 0 888 0 10 0 258",
              transport.outputLog);
    EXPECT_TRUE(segment->getAcked().close);
    EXPECT_EQ(889lu, minOpenSegmentId.current);
    transport.outputLog = "";

    segment->sync(segment->queued.bytes); // doesn't send anything
    EXPECT_EQ("", transport.outputLog);
}

TEST_F(ReplicatedSegmentTest, write) {
    segment->write(openLen + 10);
    EXPECT_EQ(openLen + 10, segment->queued.bytes);
    segment->write(openLen + 9);
    EXPECT_EQ(openLen + 10, segment->queued.bytes);
    segment->write(openLen + 11);
    EXPECT_EQ(openLen + 11, segment->queued.bytes);
}

TEST_F(ReplicatedSegmentTest, performTaskFreeNothingToDo) {
    segment->write(openLen + 10);
    segment->close();
    segment->free();
    taskQueue.performTask();
    EXPECT_FALSE(segment->isScheduled());
    EXPECT_EQ(1u, deleter.count);
}

TEST_F(ReplicatedSegmentTest, performTaskFreeOneReplicaToFree) {
    segment->write(openLen);
    segment->close();
    segment->free();

    Transport::SessionRef session = transport.getSession();
    segment->replicas[0].start(backupId1, session);
    taskQueue.performTask();
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performTaskWrite) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    taskQueue.performTask();
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performTaskRecoveringFromLostOpenReplicas) {
    transport.setInput("0 0"); // open/write
    transport.setInput("0 0"); // open/write
    transport.setInput("0 0"); // open/write for replication
    transport.setInput("0 0"); // close/write
    transport.setInput("0 0"); // close/write
    transport.setInput("0 0"); // setMinOpenSegmentId
    taskQueue.performTask(); // send open/writes
    EXPECT_TRUE(segment->handleBackupFailure({0, 0}));
    EXPECT_TRUE(segment->recoveringFromLostOpenReplicas);
    transport.outputLog = "";

    // Reap the remaining outstanding write, send the new atomic open on the
    // originally failed replica (original outstanding rpc should be canceled).
    taskQueue.performTask();
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 0 10 261 abcedfghij",
              transport.outputLog);
    transport.outputLog = "";

    taskQueue.performTask(); // should be a no-op, stuck waiting for close
    EXPECT_EQ("", transport.outputLog);

    segment->close();

    taskQueue.performTask(); // send closes
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 10 0 258 | "
                                                         // send atomic close
              "sendRequest: 0x10020 999 0 888 0 10 0 2", // send normal close
              transport.outputLog);
    transport.outputLog = "";

    TestLog::Enable _(filter);
    taskQueue.performTask(); // reap closes, update min open segment id
    EXPECT_EQ("performTask: Updating minOpenSegmentId on coordinator to ensure "
                  "lost replicas of segment 888 will not be reused | "
              "updateToAtLeast: request update to minOpenSegmentId for 999 to "
              "889",
              TestLog::get());
    EXPECT_EQ("", transport.outputLog);
}

TEST_F(ReplicatedSegmentTest, performTaskFreeWhileRecoveringOpen) {
}

TEST_F(ReplicatedSegmentTest, performFreeNothingToDo) {
    segment->freeQueued = true;
    segment->performFree(segment->replicas[0]);
    EXPECT_FALSE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->isScheduled());
    reset();
}

TEST_F(ReplicatedSegmentTest, performFreeRpcIsReady) {
    reset();

    transport.setInput("0");     // freeSegment response

    Transport::SessionRef session = transport.getSession();

    segment->freeQueued = true;
    segment->replicas[0].start(backupId1, session);
    segment->replicas[0].freeRpc.construct(context, backupId1,
                                           masterId, segmentId);
    EXPECT_STREQ("sendRequest: 0x1001c 999 0 888 0",
                 transport.outputLog.c_str());
    segment->performFree(segment->replicas[0]);
    EXPECT_FALSE(segment->replicas[0].isActive);
}

TEST_F(ReplicatedSegmentTest, performFreeRpcFailed) {
    transport.clearInput();

    reset();
    Transport::SessionRef session = transport.getSession();

    segment->freeQueued = true;
    segment->replicas[0].start({99, 99}, session);
    segment->replicas[0].freeRpc.construct(context, ServerId(99, 99),
                                           masterId, segmentId);
    TestLog::Enable _;
    segment->performFree(segment->replicas[0]);
    EXPECT_EQ("performFree: ServerDoesntExistException thrown", TestLog::get());
    EXPECT_TRUE(segment->freeQueued);
    EXPECT_FALSE(segment->isScheduled());
    ASSERT_FALSE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].freeRpc);

    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performFreeWriteRpcInProgress) {
    // It should be impossible to get into this situation now that free()
    // synchronously finishes outstanding write rpcs before starting the
    // free, but its worth keeping the code since it is more robust if
    // the code knows how to deal with queued frees while there are
    // outstanding writes in progress.
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // free
    transport.setInput("0 0"); // free

    segment->write(openLen);
    segment->close();
    taskQueue.performTask(); // writeRpc created
    segment->freeQueued = true;
    segment->schedule();

    // make sure the backup "free" opcode was not sent
    EXPECT_TRUE(TestUtil::doesNotMatchPosixRegex("0x1001c",
                                                 transport.outputLog));
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_FALSE(segment->replicas[0].freeRpc);
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // performFree reaps the write, remains scheduled
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_FALSE(segment->replicas[0].freeRpc);
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // now it schedules the free
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].freeRpc);
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // free is reaped and the replica is destroyed
    EXPECT_FALSE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->isScheduled());
    EXPECT_EQ(1u, deleter.count);
}

TEST_F(ReplicatedSegmentTest, performWriteCannotGetSession) {
    context.transportManager->skipServerIdCheck = false;
    transport.setInput("0 0 0");  // succeed server id check
    transport.setInput("0 10 0"); // fail server id check
    transport.setInput("0 99 0"); // fail server id check

    segment->write(openLen);
    taskQueue.performTask(); // fail to send writes

    EXPECT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[1].isActive);
}

TEST_F(ReplicatedSegmentTest, performWriteTooManyInFlight) {
    transport.setInput("0 0"); // open/write
    transport.setInput("0 0"); // open/write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    segment->write(openLen);
    taskQueue.performTask(); // send writes
    taskQueue.performTask(); // reap writes
    transport.outputLog = "";

    writeRpcsInFlight = ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT;
    segment->write(openLen + 10);
    taskQueue.performTask(); // try to send writes, shouldn't be able to.
    EXPECT_EQ("", transport.outputLog);

    EXPECT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);

    writeRpcsInFlight = ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT - 1;
    taskQueue.performTask(); // retry writes since a slot freed up
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 10 10 0 klmnopqrst",
                 transport.outputLog.c_str());
    transport.outputLog = "";
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_EQ(openLen + 10, segment->replicas[0].sent.bytes);
    EXPECT_TRUE(segment->replicas[1].isActive);
    EXPECT_EQ(openLen, segment->replicas[1].sent.bytes);
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // reap write and send the second replica's rpc
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 10 10 0 klmnopqrst",
              transport.outputLog);
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_TRUE(segment->replicas[1].writeRpc);
    EXPECT_EQ(openLen + 10, segment->replicas[1].sent.bytes);
    EXPECT_FALSE(segment->replicas[0].writeRpc); // make sure one was started
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // reap write
    EXPECT_FALSE(segment->replicas[1].writeRpc);
    EXPECT_EQ(uint32_t(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT - 1),
              writeRpcsInFlight);
    EXPECT_FALSE(segment->isScheduled());
    EXPECT_EQ(0u, deleter.count);
}

TEST_F(ReplicatedSegmentTest, performWriteOpen) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    segment->write(openLen);
    segment->close();
    {
        TestLog::Enable _(filter);
        taskQueue.performTask();
        EXPECT_EQ("selectSecondary: conflicting backupId: 999 | "
                  "performWrite: Starting replication of segment 888 replica "
                      "slot 0 on backup 0 | "
                  "performWrite: Sending open to backup 0 | "
                  "selectSecondary: conflicting backupId: 999 | "
                  "selectSecondary: conflicting backupId: 0 | "
                  "performWrite: Starting replication of segment 888 replica "
                      "slot 1 on backup 1 | "
                  "performWrite: Sending open to backup 1",
                  TestLog::get());
    }

    // "10 5" is length 10 (OPEN | PRIMARY), "10 1" is length 10 OPEN
    EXPECT_EQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij | "
              "sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij",
              transport.outputLog);

    EXPECT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].sent.open);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteOpenTooManyInFlight) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    writeRpcsInFlight = ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT;
    segment->write(openLen);
    taskQueue.performTask(); // try to send writes, shouldn't be able to.

    EXPECT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].sent.open);
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);

    writeRpcsInFlight = ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT - 1;
    taskQueue.performTask(); // retry writes since a slot freed up
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij",
                 transport.outputLog.c_str());
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].sent.open);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_TRUE(segment->replicas[1].isActive);
    EXPECT_FALSE(segment->replicas[1].sent.open); // ensure only one was started
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // reap write and send the second replica's rpc
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij | "
                 "sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij",
                 transport.outputLog.c_str());
    EXPECT_EQ(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT, writeRpcsInFlight);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_TRUE(segment->replicas[1].writeRpc);
    EXPECT_TRUE(segment->replicas[1].sent.open);
    EXPECT_EQ(openLen, segment->replicas[1].sent.bytes);
    EXPECT_FALSE(segment->replicas[0].writeRpc); // make sure one was started
    EXPECT_TRUE(segment->isScheduled());

    taskQueue.performTask(); // reap write
    EXPECT_FALSE(segment->replicas[1].writeRpc);
    EXPECT_EQ(uint32_t(ReplicatedSegment::MAX_WRITE_RPCS_IN_FLIGHT - 1),
              writeRpcsInFlight);
    EXPECT_FALSE(segment->isScheduled());
    EXPECT_EQ(0u, deleter.count);
}

TEST_F(ReplicatedSegmentTest, performWriteRpcIsReady) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    segment->write(openLen);
    segment->close();

    taskQueue.performTask();
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_EQ(0u, segment->replicas[0].acked.bytes);

    taskQueue.performTask();
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].acked.bytes);
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteRpcFailed) {
    ServerIdRpcWrapper::ConvertExceptionsToDoesntExist _;
    transport.clearInput();
    transport.setInput("0 0"); // ok first replica open
    transport.setInput(NULL); // error second replica open
    transport.setInput("0 0"); // ok second replica reopen
    transport.setInput(NULL); // error first replica close
    transport.setInput("0 0"); // ok second replica close
    transport.setInput("0 0"); // ok first replica reclose

    segment->write(openLen);

    taskQueue.performTask();  // send open requests
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_EQ(0u, segment->replicas[0].acked.bytes);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_EQ(openLen, segment->replicas[1].sent.bytes);
    ServerId backupIdForFirstOpenAttempt = segment->replicas[1].backupId;

    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 0 10 5 abcedfghij | "
                 "sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij",
                 transport.outputLog.c_str());
    transport.outputLog = "";
    {
        TestLog::Enable _;
        taskQueue.performTask();  // reap rpcs, second replica got error
        EXPECT_TRUE(TestUtil::matchesPosixRegex(
            "performWrite: Couldn't write to backup 1; server is down",
            TestLog::get()));
    }
    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].acked.bytes);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_EQ(0u, segment->replicas[1].acked.bytes);
    // Ensure retried open rpc goes to the same backup as the first attempt.
    EXPECT_EQ(backupIdForFirstOpenAttempt, segment->replicas[1].backupId);

    taskQueue.performTask();  // resend second open request
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 0 10 1 abcedfghij",
                 transport.outputLog.c_str());
    transport.outputLog = "";
    taskQueue.performTask();  // reap second open request

    segment->write(openLen + 10);
    segment->close();
    taskQueue.performTask();  // send close requests
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 10 10 2 klmnopqrst | "
                 "sendRequest: 0x10020 999 0 888 0 10 10 2 klmnopqrst",
                 transport.outputLog.c_str());
    transport.outputLog = "";
    {
        TestLog::Enable _;
        taskQueue.performTask();  // reap rpcs, first replica got error
        EXPECT_TRUE(TestUtil::matchesPosixRegex(
            "performWrite: Couldn't write to backup 0; server is down",
            TestLog::get()));
    }
    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].acked.bytes);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_EQ(openLen + 10, segment->replicas[1].acked.bytes);
    EXPECT_EQ(openLen + 10, segment->replicas[1].sent.bytes);
    EXPECT_FALSE(segment->replicas[1].writeRpc);

    taskQueue.performTask();  // resend first close request
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 10 10 2 klmnopqrst",
                 transport.outputLog.c_str());
    transport.outputLog = "";
    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen + 10, segment->replicas[0].sent.bytes);
    EXPECT_TRUE(segment->replicas[0].writeRpc);

    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteMoreToSend) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write

    segment->write(openLen + 20);
    segment->close();
    taskQueue.performTask(); // send open
    EXPECT_TRUE(segment->isScheduled());
    taskQueue.performTask(); // reap opens
    EXPECT_TRUE(segment->isScheduled());
    transport.outputLog = "";
    taskQueue.performTask(); // send second round

    // "20 4" is length 20 (PRIMARY), "20 0" is length 20 NONE
    EXPECT_STREQ(
        "sendRequest: 0x10020 999 0 888 0 10 20 2 klmnopqrstuvwxyzabce | "
        "sendRequest: 0x10020 999 0 888 0 10 20 2 klmnopqrstuvwxyzabce",
        transport.outputLog.c_str());
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_TRUE(segment->replicas[0].writeRpc);

    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteClosedButLongerThanMaxTxLimit) {
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // write
    transport.setInput("0 0"); // close
    transport.setInput("0 0"); // close

    segment->write(segment->maxBytesPerWriteRpc + segment->openLen + 1);
    segment->close();
    taskQueue.performTask(); // send open
    EXPECT_TRUE(segment->isScheduled());
    taskQueue.performTask(); // reap opens
    EXPECT_TRUE(segment->isScheduled());
    transport.outputLog = "";
    taskQueue.performTask(); // send second round

    // "21 0" is length 21 NONE, "21 0" is length 21 NONE
    EXPECT_STREQ(
        "sendRequest: 0x10020 999 0 888 0 10 21 0 klmnopqrstuvwxyzabced | "
        "sendRequest: 0x10020 999 0 888 0 10 21 0 klmnopqrstuvwxyzabced",
        transport.outputLog.c_str());
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    transport.outputLog = "";

    taskQueue.performTask(); // reap second round
    taskQueue.performTask(); // send third (closing) round

    // "1 2" is length 1 CLOSE, "1 2" is length 1 CLOSE
    EXPECT_STREQ("sendRequest: 0x10020 999 0 888 0 31 1 2 f | "
                 "sendRequest: 0x10020 999 0 888 0 31 1 2 f",
                 transport.outputLog.c_str());
    EXPECT_TRUE(segment->isScheduled());
    EXPECT_TRUE(segment->replicas[0].writeRpc);

    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteEnsureNewHeadOpenAckedBeforeClose) {
    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - segment close
    transport.setInput("0 0"); // write - segment close

    taskQueue.performTask(); // send segment open
    taskQueue.performTask();
    taskQueue.performTask(); // reap segment open
    taskQueue.performTask();

    auto newHead = newSegment(segment.get(), segmentId + 1);
    segment->close();

    taskQueue.performTask(); // send newHead open, try segment close but can't
    taskQueue.performTask();

    EXPECT_TRUE(newHead->isScheduled());
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_TRUE(newHead->replicas[0].writeRpc);
    EXPECT_TRUE(newHead->replicas[0].sent.open);
    EXPECT_FALSE(newHead->replicas[0].acked.open);

    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_FALSE(segment->replicas[0].sent.close);

    taskQueue.performTask(); // reap newHead open, try segment close should work
    taskQueue.performTask();

    EXPECT_FALSE(newHead->isScheduled());
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_FALSE(newHead->replicas[0].writeRpc);
    EXPECT_TRUE(newHead->replicas[0].acked.open);

    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].sent.close);

    EXPECT_EQ(0u, deleter.count);
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteEnsureCloseBeforeNewHeadWrittenTo) {
    auto newHead = newSegment(segment.get(), segmentId + 1);

    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - segment open
    taskQueue.performTask(); // send segment open for segment
    taskQueue.performTask(); // cannot send open for newHead
                             // (first open not yet durable)
    taskQueue.performTask(); // reap segment open for segment
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - newHead open
    taskQueue.performTask(); // send segment open for newHead
    taskQueue.performTask(); // segment - nothing to do
    taskQueue.performTask(); // reap segment open for newHead

    segment->close(); // close queued
    newHead->write(openLen + 10); // write queued

    transport.setInput("0 0"); // write - segment close replica 1
    transport.setInput("0 0"); // write - segment close replica 2
    taskQueue.performTask(); // send close rpcs for segment
    transport.setInput("0 0"); // write - newHead
    transport.setInput("0 0"); // write - newHead
    taskQueue.performTask(); // try newHead write but can't

    EXPECT_TRUE(newHead->isScheduled());
    EXPECT_FALSE(newHead->precedingSegmentCloseAcked);
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_FALSE(newHead->replicas[0].writeRpc);
    EXPECT_TRUE(newHead->replicas[0].acked.open);
    EXPECT_EQ(openLen, newHead->replicas[0].sent.bytes);

    EXPECT_TRUE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_TRUE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].sent.close);
    EXPECT_FALSE(segment->replicas[0].acked.close);

    taskQueue.performTask(); // reap close rpcs on replicas

    EXPECT_TRUE(newHead->isScheduled());
    EXPECT_TRUE(newHead->precedingSegmentCloseAcked);
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_FALSE(newHead->replicas[0].writeRpc);
    EXPECT_TRUE(newHead->replicas[0].acked.open);
    EXPECT_EQ(openLen, newHead->replicas[0].sent.bytes);

    EXPECT_FALSE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].sent.close);
    EXPECT_TRUE(segment->replicas[0].acked.close);
    ASSERT_TRUE(segment->replicas[1].isActive);
    EXPECT_FALSE(segment->replicas[1].writeRpc);
    EXPECT_TRUE(segment->replicas[1].sent.close);
    EXPECT_TRUE(segment->replicas[1].acked.close);

    taskQueue.performTask(); // send newHead write

    EXPECT_TRUE(newHead->isScheduled());
    EXPECT_TRUE(newHead->precedingSegmentCloseAcked);
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_TRUE(newHead->replicas[0].writeRpc);
    EXPECT_EQ(openLen + 10, newHead->replicas[0].sent.bytes);

    EXPECT_FALSE(segment->isScheduled());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_FALSE(segment->replicas[0].writeRpc);
    EXPECT_TRUE(segment->replicas[0].acked.close);

    EXPECT_EQ(0u, deleter.count);
    newHead->scheduled = false;
    reset();
}

TEST_F(ReplicatedSegmentTest, performWriteBackupRejectedOpen) {
    transport.setInput("0 0"); // write - open
    transport.setInput("13 0"); // write - open rejected
    transport.setInput("12 0"); // write - bad segment id exception

    taskQueue.performTask(); // send

    TestLog::Enable _(filter);
    taskQueue.performTask(); // reap - second replica gets rejected
    EXPECT_EQ(
        "performWrite: Couldn't open replica on backup 1; server may be "
        "overloaded or may already have a replica for this segment which "
        "was found on disk after a crash; will choose another backup",
        TestLog::get());
    ASSERT_TRUE(segment->replicas[0].isActive);
    EXPECT_EQ(openLen, segment->replicas[0].sent.bytes);
    EXPECT_EQ(10u, segment->replicas[0].acked.bytes);

    ASSERT_FALSE(segment->replicas[1].isActive);
    EXPECT_EQ(0u, segment->replicas[1].sent.bytes);
    EXPECT_EQ(0u, segment->replicas[1].acked.bytes);
    EXPECT_FALSE(segment->replicas[1].writeRpc);
    EXPECT_EQ(ServerId(), segment->replicas[1].backupId);

    taskQueue.performTask(); // send
    // Next performTask() should blow up whole server: backup can throw
    // this if the server issues a write to a replica that wasn't created
    // by that backup but that it found on disk instead.
    EXPECT_THROW(taskQueue.performTask(),
                 BackupBadSegmentIdException);

    reset();
}

namespace {
bool performWriteFilter(string s) {
    return s == "performWrite";
}
}

TEST_F(ReplicatedSegmentTest, performWriteEnsureDurableOpensOrdered) {
    auto newHead = newSegment(segment.get(), segmentId + 1);
    segment->close(); // close queued

    // Mess up the queue order to simulate reorder due to failure.
    // This means newHead will be going first at 'taking turns'
    // with segment.
    {
        Task* t = taskQueue.tasks.front();
        taskQueue.tasks.pop();
        taskQueue.tasks.push(t);
    }

    TestLog::Enable _(performWriteFilter);
    TestLog::reset();
    taskQueue.performTask(); // new head cannot open until first is acked
    EXPECT_EQ(
        "performWrite: Starting replication of segment 889 replica slot 0 "
            "on backup 0 | "
        "performWrite: Cannot open segment 889 until preceding segment "
            "is durably open | "
        "performWrite: Starting replication of segment 889 replica slot 1 "
            "on backup 1 | "
        "performWrite: Cannot open segment 889 until preceding segment "
            "is durably open",
        TestLog::get());
    EXPECT_TRUE(newHead->isScheduled());
    EXPECT_FALSE(newHead->precedingSegmentOpenAcked);
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_FALSE(newHead->replicas[0].writeRpc);
    EXPECT_FALSE(newHead->replicas[0].acked.open);
    EXPECT_EQ(0lu, newHead->replicas[0].sent.bytes);

    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - segment open
    taskQueue.performTask(); // send segment open for first segment
    TestLog::reset();
    taskQueue.performTask(); // new head cannot open until first is acked
    EXPECT_EQ(
        "performWrite: Cannot open segment 889 until preceding segment "
            "is durably open | "
        "performWrite: Cannot open segment 889 until preceding segment "
            "is durably open",
        TestLog::get());
    taskQueue.performTask(); // reap segment open for first segment
    TestLog::reset();
    transport.setInput("0 0"); // write - newHead open
    transport.setInput("0 0"); // write - newHead open
    taskQueue.performTask(); // send segment open for second semgent
    EXPECT_EQ(
        "performWrite: Sending open to backup 0 | "
        "performWrite: Sending open to backup 1",
        TestLog::get());
    EXPECT_TRUE(newHead->isScheduled());
    EXPECT_TRUE(newHead->precedingSegmentOpenAcked);
    ASSERT_TRUE(newHead->replicas[0].isActive);
    EXPECT_TRUE(newHead->replicas[0].writeRpc);
    EXPECT_FALSE(newHead->replicas[0].acked.open);
    EXPECT_EQ(openLen, newHead->replicas[0].sent.bytes);

    taskQueue.performTask();
    taskQueue.performTask(); // reap open rpc on new head
    EXPECT_TRUE(newHead->replicas[0].acked.open);
}

TEST_F(ReplicatedSegmentTest, performWriteEnsureDurableOpensOrderedAlreadyOpen)
{
    transport.setInput("0 0"); // write - segment open
    transport.setInput("0 0"); // write - segment open
    taskQueue.performTask(); // send segment open for first segment
    taskQueue.performTask(); // reap segment open for first segment

    auto newHead = newSegment(segment.get(), segmentId + 1);
    segment->close(); // close queued

    EXPECT_TRUE(newHead->precedingSegmentOpenAcked);
}

} // namespace RAMCloud
