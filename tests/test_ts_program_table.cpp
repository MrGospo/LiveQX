#include <gtest/gtest.h>

#include "gateway/ts/ProgramTable.h"

using namespace liveqx::gateway::ts;

namespace {

ParsedPat makePat(std::uint16_t tsid,
                  std::vector<std::pair<std::uint16_t, std::uint16_t>> programs,
                  std::uint8_t version = 0) {
    ParsedPat pat;
    pat.transport_stream_id = tsid;
    pat.version_number = version;
    for (auto [pn, pmt] : programs) {
        PatEntry e;
        e.program_number = pn;
        e.pmt_pid = pmt;
        pat.programs.push_back(e);
    }
    return pat;
}

ParsedPmt makePmt(std::uint16_t pn,
                  std::uint16_t pcr,
                  std::vector<std::pair<std::uint8_t, std::uint16_t>> streams,
                  std::uint8_t version = 0) {
    ParsedPmt pmt;
    pmt.program_number = pn;
    pmt.pcr_pid = pcr;
    pmt.version_number = version;
    for (auto [type, pid] : streams) {
        PmtStream s;
        s.stream_type = type;
        s.elementary_pid = pid;
        pmt.streams.push_back(s);
    }
    return pmt;
}

}  // namespace

TEST(ProgramTableTest, EmptyOnConstruction) {
    ProgramTable t;
    auto snap = t.current();
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->programs.size(), 0u);
    EXPECT_EQ(snap->transport_stream_id, 0u);
}

TEST(ProgramTableTest, IngestPatPopulatesPrograms) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}, {2, 0x101}}));
    auto snap = t.current();
    EXPECT_EQ(snap->transport_stream_id, 0x1234u);
    ASSERT_EQ(snap->programs.size(), 2u);
    EXPECT_EQ(snap->programs[0].service_id, 1u);
    EXPECT_EQ(snap->programs[0].pmt_pid, 0x100u);
    EXPECT_FALSE(snap->programs[0].discovered);  // PAT alone doesn't discover
}

TEST(ProgramTableTest, IngestPmtMarksDiscovered) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    t.ingestPmt(makePmt(1, 0x100, {{0x1B, 0x110}, {0x0F, 0x111}}));
    auto snap = t.current();
    ASSERT_EQ(snap->programs.size(), 1u);
    const auto& p = snap->programs[0];
    EXPECT_TRUE(p.discovered);
    EXPECT_EQ(p.pcr_pid, 0x100u);
    ASSERT_EQ(p.streams.size(), 2u);
    EXPECT_EQ(p.streams[0].stream_type, 0x1Bu);
    EXPECT_EQ(p.streams[0].codec_name, "H.264");
    EXPECT_EQ(p.streams[1].codec_name, "AAC");
}

TEST(ProgramTableTest, FindByServiceId) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{7, 0x200}, {9, 0x300}}));
    auto snap = t.current();
    auto* p = snap->find(9);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->pmt_pid, 0x300u);
    EXPECT_EQ(snap->find(99), nullptr);
}

TEST(ProgramTableTest, IdenticalRepublishKeepsSnapshotIdentity) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    auto snap1 = t.current();
    // Ingest the *same* PAT — snapshot pointer must be unchanged.
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    auto snap2 = t.current();
    EXPECT_EQ(snap1.get(), snap2.get());
}

TEST(ProgramTableTest, VersionBumpPublishesNewSnapshot) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}, /*version=*/0));
    auto snap1 = t.current();
    t.ingestPat(makePat(0x1234, {{1, 0x100}}, /*version=*/1));
    auto snap2 = t.current();
    EXPECT_NE(snap1.get(), snap2.get());
    EXPECT_EQ(snap2->pat_version, 1);
}

TEST(ProgramTableTest, DroppedProgramDisappears) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}, {2, 0x101}}));
    t.ingestPat(makePat(0x1234, {{1, 0x100}}, /*version=*/1));
    auto snap = t.current();
    ASSERT_EQ(snap->programs.size(), 1u);
    EXPECT_EQ(snap->programs[0].service_id, 1u);
}

TEST(ProgramTableTest, PmtCarriesForwardOnPatRebroadcast) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    t.ingestPmt(makePmt(1, 0x100, {{0x1B, 0x110}}));
    EXPECT_TRUE(t.current()->programs[0].discovered);

    // PAT rebroadcast with the same payload must NOT lose PMT data.
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    EXPECT_TRUE(t.current()->programs[0].discovered);
    EXPECT_EQ(t.current()->programs[0].streams.size(), 1u);
}

TEST(ProgramTableTest, SdtFillsServiceMetadata) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{7, 0x100}}));
    ParsedSdt sdt;
    sdt.transport_stream_id = 0x1234;
    sdt.original_network_id = 0xABCD;
    SdtService svc;
    svc.service_id = 7;
    svc.service_name = "BBC News";
    svc.provider_name = "BBC";
    svc.running_status = 4;
    svc.eit_present_following_flag = true;
    sdt.services.push_back(svc);
    t.ingestSdt(sdt);

    auto snap = t.current();
    EXPECT_EQ(snap->original_network_id, 0xABCD);
    ASSERT_EQ(snap->programs.size(), 1u);
    EXPECT_EQ(snap->programs[0].service_name, "BBC News");
    EXPECT_EQ(snap->programs[0].provider_name, "BBC");
    EXPECT_EQ(snap->programs[0].running_status, 4);
    EXPECT_TRUE(snap->programs[0].eit_present);
}

TEST(ProgramTableTest, SdtForUnknownServiceIgnored) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    ParsedSdt sdt;
    sdt.transport_stream_id = 0x1234;
    SdtService svc; svc.service_id = 99; svc.service_name = "ghost";
    sdt.services.push_back(svc);
    t.ingestSdt(sdt);
    auto snap = t.current();
    EXPECT_TRUE(snap->programs[0].service_name.empty());
}

TEST(ProgramTableTest, ClearResetsState) {
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));
    t.ingestPmt(makePmt(1, 0x100, {{0x1B, 0x110}}));
    EXPECT_FALSE(t.current()->programs.empty());
    t.clear();
    auto snap = t.current();
    EXPECT_TRUE(snap->programs.empty());
    EXPECT_EQ(snap->transport_stream_id, 0u);
}

TEST(ProgramTableTest, SignatureStableAcrossOrdering) {
    ProgramTable a, b;
    a.ingestPat(makePat(0x1234, {{1, 0x100}, {2, 0x101}}));
    b.ingestPat(makePat(0x1234, {{2, 0x101}, {1, 0x100}}));
    EXPECT_EQ(a.signature(), b.signature());
}

TEST(ProgramTableTest, SignatureChangesWithProgramChange) {
    ProgramTable a, b;
    a.ingestPat(makePat(0x1234, {{1, 0x100}}));
    b.ingestPat(makePat(0x1234, {{1, 0x101}}));  // different PMT pid
    EXPECT_NE(a.signature(), b.signature());
}

TEST(ProgramTableTest, PmtBeforePatStillIngests) {
    // Edge case: PMT can arrive before PAT after a rapid version flip.
    ProgramTable t;
    t.ingestPmt(makePmt(1, 0x100, {{0x1B, 0x110}}));
    auto snap = t.current();
    ASSERT_EQ(snap->programs.size(), 1u);
    EXPECT_EQ(snap->programs[0].service_id, 1u);
    EXPECT_TRUE(snap->programs[0].discovered);
}

TEST(ProgramTableTest, ConcurrentReadersSafeWithWriter) {
    // Smoke test: many readers iterate snapshots while writer publishes new
    // versions. ASan/TSan should be clean. Snapshots are shared_ptr-immutable.
    ProgramTable t;
    t.ingestPat(makePat(0x1234, {{1, 0x100}}));

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = t.current();
                ASSERT_NE(snap, nullptr);
                volatile std::size_t s = snap->programs.size();
                (void)s;
            }
        });
    }
    for (int v = 1; v <= 200; ++v) {
        t.ingestPat(makePat(0x1234, {{1, 0x100}, {2, 0x101}},
                            static_cast<std::uint8_t>(v % 32)));
    }
    stop.store(true);
    for (auto& th : readers) th.join();
    EXPECT_EQ(t.current()->programs.size(), 2u);
}
