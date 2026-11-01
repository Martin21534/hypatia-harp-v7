/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v6.h"
#include "ns3/boolean.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-harp.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

namespace ns3
{

namespace
{

Ptr<TcpSocketState>
MakeV6TestState(uint32_t bytesInFlight = 90000)
{
    Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
    tcb->m_segmentSize = 1000;
    tcb->m_cWnd = 100000;
    tcb->m_cWndInfl = 100000;
    tcb->m_bytesInFlight = bytesInFlight;
    return tcb;
}

TcpHeader
MakeV6Ack(uint32_t gapSegments,
          bool withSack = true,
          bool withSignal = false,
          TcpOptionHarp::Signal signal = TcpOptionHarp::Signal::DATA_ECHO,
          uint16_t epoch = 7)
{
    TcpHeader header;
    header.SetFlags(TcpHeader::ACK);
    header.SetAckNumber(SequenceNumber32(1000));

    if (withSignal)
    {
        Ptr<TcpOptionHarp> harp = CreateObject<TcpOptionHarp>();
        harp->SetSignal(signal);
        harp->SetEpoch(epoch);
        header.AppendOption(harp);
    }
    if (withSack)
    {
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock(
            {SequenceNumber32(1500), SequenceNumber32(1000 + gapSegments * 1000)});
        header.AppendOption(sack);
    }
    return header;
}

uint32_t
RunV6Recovery(Ptr<TcpHarpCubicV6> v6,
              Ptr<TcpSocketState> tcb,
              uint32_t gapSegments,
              bool withSignal = false,
              TcpOptionHarp::Signal signal = TcpOptionHarp::Signal::DATA_ECHO,
              uint16_t epoch = 7)
{
    const TcpHeader ack = MakeV6Ack(gapSegments, true, withSignal, signal, epoch);
    v6->ObserveAck(ack, tcb->m_segmentSize, tcb->GetCwndInSegments());
    return v6->GetSsThresh(tcb, tcb->m_bytesInFlight);
}

} // namespace

class TcpHarpCubicV6ConservativeGateTest : public TestCase
{
  public:
    TcpHarpCubicV6ConservativeGateTest()
        : TestCase("HARP-Cubic V6 requires both a packet signal and gap seven")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpSocketState> tcb = MakeV6TestState();

        Ptr<TcpHarpCubicV6> noSignal = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(noSignal, tcb, 150),
                              70000,
                              "A large gap without a signal must remain normal CUBIC");

        Ptr<TcpHarpCubicV6> smallGap = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(smallGap, tcb, 6, true),
                              70000,
                              "A signaled gap below seven must remain normal CUBIC");

        Ptr<TcpHarpCubicV6> qualified = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(qualified, tcb, 7, true),
                              85000,
                              "A signaled gap of seven must select the bounded target");

        Ptr<TcpHarpCubicV6> disabled = CreateObject<TcpHarpCubicV6>();
        disabled->SetAttribute("EnableAdaptiveBeta", BooleanValue(false));
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(disabled, tcb, 150, true),
                              70000,
                              "Disabling V6 must always restore normal CUBIC");
    }
};

class TcpHarpCubicV6UnifiedDirectionTest : public TestCase
{
  public:
    TcpHarpCubicV6UnifiedDirectionTest()
        : TestCase("HARP-Cubic V6 applies the same detector and beta to DATA and ACK signals")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpSocketState> tcb = MakeV6TestState();
        Ptr<TcpHarpCubicV6> data = CreateObject<TcpHarpCubicV6>();
        Ptr<TcpHarpCubicV6> ack = CreateObject<TcpHarpCubicV6>();

        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(data,
                                            tcb,
                                            7,
                                            true,
                                            TcpOptionHarp::Signal::DATA_ECHO),
                              85000,
                              "DATA signal must use the common bounded target");
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(ack,
                                            tcb,
                                            7,
                                            true,
                                            TcpOptionHarp::Signal::ACK_MARK),
                              85000,
                              "ACK signal must use the same bounded target");
    }
};

class TcpHarpCubicV6FollowupGateTest : public TestCase
{
  public:
    TcpHarpCubicV6FollowupGateTest()
        : TestCase("HARP-Cubic V6 rechecks the gap on every follow-up Recovery")
    {
    }

  private:
    void RunSmallFollowup()
    {
        m_tcb->m_cWnd = 80000;
        m_tcb->m_bytesInFlight = 75000;
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 6),
                              56000,
                              "A small-gap follow-up must use normal beta 0.70");
    }

    void RunQualifiedFollowup()
    {
        m_tcb->m_cWnd = 80000;
        m_tcb->m_bytesInFlight = 75000;
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 7),
                              80000,
                              "A qualified follow-up may retain at most the saved safe target");
    }

    void DoRun() override
    {
        m_tcb = MakeV6TestState();
        m_v6 = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 7, true),
                              85000,
                              "The first qualified Recovery must establish a safe target");

        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV6FollowupGateTest::RunSmallFollowup,
                            this);
        Simulator::Schedule(MilliSeconds(200),
                            &TcpHarpCubicV6FollowupGateTest::RunQualifiedFollowup,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV6> m_v6;
};

class TcpHarpCubicV6UnifiedEpisodeTest : public TestCase
{
  public:
    TcpHarpCubicV6UnifiedEpisodeTest()
        : TestCase("HARP-Cubic V6 does not restart an episode when direction changes")
    {
    }

  private:
    void AckDirectionFollowup()
    {
        m_tcb->m_cWnd = 80000;
        m_tcb->m_bytesInFlight = 75000;
        NS_TEST_ASSERT_MSG_EQ(
            RunV6Recovery(m_v6,
                          m_tcb,
                          7,
                          true,
                          TcpOptionHarp::Signal::ACK_MARK,
                          42),
            80000,
            "An ACK-direction mark in the active DATA episode must reuse its safe target");
    }

    void StaleAckCannotReopenEpisode()
    {
        m_tcb->m_cWnd = 80000;
        m_tcb->m_bytesInFlight = 75000;
        NS_TEST_ASSERT_MSG_EQ(
            RunV6Recovery(m_v6, m_tcb, 7),
            56000,
            "An ACK mark received inside the expired DATA episode must not reopen protection");
    }

    void NewSignalStartsNewEpisode()
    {
        m_tcb->m_cWnd = 80000;
        m_tcb->m_bytesInFlight = 75000;
        NS_TEST_ASSERT_MSG_EQ(
            RunV6Recovery(m_v6,
                          m_tcb,
                          7,
                          true,
                          TcpOptionHarp::Signal::DATA_ECHO,
                          8),
            68000,
            "A signal received after the old deadline must establish a new episode");
    }

    void DoRun() override
    {
        m_tcb = MakeV6TestState();
        m_v6 = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6,
                                            m_tcb,
                                            7,
                                            true,
                                            TcpOptionHarp::Signal::DATA_ECHO,
                                            7),
                              85000,
                              "The DATA signal must establish the original episode");

        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV6UnifiedEpisodeTest::AckDirectionFollowup,
                            this);
        Simulator::Schedule(MilliSeconds(300),
                            &TcpHarpCubicV6UnifiedEpisodeTest::StaleAckCannotReopenEpisode,
                            this);
        Simulator::Schedule(MilliSeconds(310),
                            &TcpHarpCubicV6UnifiedEpisodeTest::NewSignalStartsNewEpisode,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV6> m_v6;
};

class TcpHarpCubicV6LateSignalTest : public TestCase
{
  public:
    TcpHarpCubicV6LateSignalTest()
        : TestCase("HARP-Cubic V6 revises only a qualified active Recovery for a late signal")
    {
    }

  private:
    void ReceiveQualifiedSignal()
    {
        const TcpHeader signalAck = MakeV6Ack(0, false, true);
        m_qualified->ObserveAck(signalAck,
                                m_qualifiedTcb->m_segmentSize,
                                m_qualifiedTcb->GetCwndInSegments());
        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(m_qualified->GetRecoveryTargetUpdate(m_qualifiedTcb, target),
                              true,
                              "A late signal must update a saved gap-seven Recovery");
        NS_TEST_ASSERT_MSG_EQ(target, 85000, "Late update must retain the bounded target");
        NS_TEST_ASSERT_MSG_EQ(m_qualified->GetRecoveryTargetUpdate(m_qualifiedTcb, target),
                              false,
                              "A late update must be consumed exactly once");
    }

    void ReceiveSmallGapSignal()
    {
        const TcpHeader signalAck = MakeV6Ack(0, false, true, TcpOptionHarp::Signal::DATA_ECHO, 8);
        m_small->ObserveAck(signalAck,
                            m_smallTcb->m_segmentSize,
                            m_smallTcb->GetCwndInSegments());
        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(m_small->GetRecoveryTargetUpdate(m_smallTcb, target),
                              false,
                              "A late signal must not undo a saved small-gap Recovery");
    }

    void DoRun() override
    {
        m_qualifiedTcb = MakeV6TestState();
        m_qualifiedTcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_qualified = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_qualified, m_qualifiedTcb, 7),
                              70000,
                              "Before the signal, a qualified gap must initially use normal CUBIC");

        m_smallTcb = MakeV6TestState();
        m_smallTcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_small = CreateObject<TcpHarpCubicV6>();
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_small, m_smallTcb, 6),
                              70000,
                              "A small gap must initially use normal CUBIC");

        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV6LateSignalTest::ReceiveQualifiedSignal,
                            this);
        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV6LateSignalTest::ReceiveSmallGapSignal,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_qualifiedTcb;
    Ptr<TcpHarpCubicV6> m_qualified;
    Ptr<TcpSocketState> m_smallTcb;
    Ptr<TcpHarpCubicV6> m_small;
};

class TcpHarpCubicV6DynamicBaselineTest : public TestCase
{
  public:
    TcpHarpCubicV6DynamicBaselineTest()
        : TestCase("HARP-Cubic V6 raises its gap threshold from confirmed normal Recoveries")
    {
    }

  private:
    void Recovery11()
    {
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 11), 70000, "gap 11 is a normal sample");
    }

    void Recovery12()
    {
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 12), 70000, "gap 12 is a normal sample");
    }

    void SignalGap13()
    {
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 13, true),
                              70000,
                              "median 11 and MAD 1 must raise the threshold to 14");
    }

    void SignalGap14()
    {
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6,
                                            m_tcb,
                                            14,
                                            true,
                                            TcpOptionHarp::Signal::DATA_ECHO,
                                            8),
                              85000,
                              "A gap at the learned threshold must qualify");
    }

    void DoRun() override
    {
        m_tcb = MakeV6TestState();
        m_v6 = CreateObject<TcpHarpCubicV6>();
        m_v6->SetAttribute("HandoverWindow", TimeValue(MilliSeconds(1)));

        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 10), 70000, "gap 10 is a normal sample");
        Simulator::Schedule(MilliSeconds(2),
                            &TcpHarpCubicV6DynamicBaselineTest::Recovery11,
                            this);
        Simulator::Schedule(MilliSeconds(4),
                            &TcpHarpCubicV6DynamicBaselineTest::Recovery12,
                            this);
        Simulator::Schedule(MilliSeconds(6),
                            &TcpHarpCubicV6DynamicBaselineTest::SignalGap13,
                            this);
        Simulator::Schedule(MilliSeconds(8),
                            &TcpHarpCubicV6DynamicBaselineTest::SignalGap14,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV6> m_v6;
};

class TcpHarpCubicV6RobustOutlierTest : public TestCase
{
  public:
    TcpHarpCubicV6RobustOutlierTest()
        : TestCase("HARP-Cubic V6 median and MAD reject one huge normal-gap outlier")
    {
    }

  private:
    void Recovery4Again()
    {
        RunV6Recovery(m_v6, m_tcb, 4);
    }

    void RecoveryOutlier()
    {
        RunV6Recovery(m_v6, m_tcb, 400);
    }

    void SignalGap7()
    {
        NS_TEST_ASSERT_MSG_EQ(RunV6Recovery(m_v6, m_tcb, 7, true),
                              85000,
                              "One outlier must not move the robust threshold above seven");
    }

    void DoRun() override
    {
        m_tcb = MakeV6TestState();
        m_v6 = CreateObject<TcpHarpCubicV6>();
        m_v6->SetAttribute("HandoverWindow", TimeValue(MilliSeconds(1)));

        RunV6Recovery(m_v6, m_tcb, 4);
        Simulator::Schedule(MilliSeconds(2),
                            &TcpHarpCubicV6RobustOutlierTest::Recovery4Again,
                            this);
        Simulator::Schedule(MilliSeconds(4),
                            &TcpHarpCubicV6RobustOutlierTest::RecoveryOutlier,
                            this);
        Simulator::Schedule(MilliSeconds(6),
                            &TcpHarpCubicV6RobustOutlierTest::SignalGap7,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV6> m_v6;
};

class TcpHarpCubicV6TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV6TestSuite()
        : TestSuite("tcp-harp-cubic-v6", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV6ConservativeGateTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6UnifiedDirectionTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6FollowupGateTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6UnifiedEpisodeTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6LateSignalTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6DynamicBaselineTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV6RobustOutlierTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV6TestSuite g_tcpHarpCubicV6TestSuite;

} // namespace ns3
