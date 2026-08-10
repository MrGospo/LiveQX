#include <gtest/gtest.h>
#include "output/SrtOutput.h"
#include <chrono>
#include <thread>

TEST(SrtOutput, StartSucceeds) {
    SrtOutput out(9000, 200);
    EXPECT_TRUE(out.start());
    out.stop();
}

TEST(SrtOutput, NotHealthyBeforeStart) {
    SrtOutput out(9000, 200);
    EXPECT_FALSE(out.isHealthy());
}

TEST(SrtOutput, SendBeforeStartDoesNotCrash) {
    SrtOutput out(9000, 200);
    Packet pkt;
    pkt.data = {0x47, 0x00, 0x00}; // 3 bytes of mock TS
    EXPECT_NO_THROW(out.send(pkt));
}

TEST(SrtOutput, StatsZeroByDefault) {
    SrtOutput out(9000, 200);
    auto s = out.getStats();
    EXPECT_EQ(s.packets_sent, 0u);
    EXPECT_EQ(s.bytes_sent,   0u);
    EXPECT_DOUBLE_EQ(s.rtt_ms, 0.0);
}

TEST(SrtOutput, StopWithoutStartDoesNotCrash) {
    SrtOutput out(9000, 200);
    EXPECT_NO_THROW(out.stop());
}

TEST(SrtOutput, DoubleStartReturnsFalse) {
    SrtOutput out(9000, 200);
    EXPECT_TRUE(out.start());
    EXPECT_FALSE(out.start()); // already running
    out.stop();
}

TEST(SrtOutput, SendLargePacketDoesNotCrash) {
    SrtOutput out(9000, 200);
    Packet pkt;
    pkt.data.resize(1316 * 4, 0x47); // 4 SRT chunks worth
    EXPECT_NO_THROW(out.send(pkt));
}

TEST(SrtOutput, DestructorStopsGracefully) {
    // Verify RAII: destructor calls stop() without deadlock.
    {
        SrtOutput out(9001, 120);
        out.start(); // will fail — that's OK
    } // destructor here
    SUCCEED();
}

TEST(SrtOutput, IsHealthyFalseAfterStop) {
    SrtOutput out(9000, 200);
    out.stop();
    EXPECT_FALSE(out.isHealthy());
}

TEST(SrtOutput, QueueFillDoesNotBlock) {
    // Push 128 packets (> queue capacity 64) — should drop with LOG_WARN, not block.
    SrtOutput out(9000, 200);
    Packet pkt;
    pkt.data.resize(188, 0x47);
    for (int i = 0; i < 128; ++i)
        EXPECT_NO_THROW(out.send(pkt));
}
