/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v7.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
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
MakeV7TestState(uint32_t bytesInFlight = 90000)
{
    Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
    tcb->m_segmentSize = 1000;
    tcb->m_cWnd = 100000;
    tcb->m_cWndInfl = 100000;
    tcb->m_bytesInFlight = bytesInFlight;
    return tcb;
}

TcpHeader
MakeV7Ack(uint32_t gapSegments,
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
RunV7Recovery(Ptr<TcpHarpCubicV7> v7,
              Ptr<TcpSocketState> tcb,
              uint32_t gapSegments,
              bool withSignal = false,
              TcpOptionHarp::Signal signal = TcpOptionHarp::Signal::DATA_ECHO,
              uint16_t epoch = 7)
{
    const TcpHeader ack = MakeV7Ack(gapSegments, true, withSignal, signal, epoch);
    v7->ObserveAck(ack, tcb->m_segmentSize, tcb->GetCwndInSegments());
    return v7->GetSsThresh(tcb, tcb->m_bytesInFlight);
}

class ForcedRandomTcpHarpCubicV7 : public TcpHarpCubicV7
{
  protected:
    bool GetCongestionPrediction(double& probability) const override
    {
        probability = 0.0;
        return true;
    }
};

class ForcedCongestedTcpHarpCubicV7 : public TcpHarpCubicV7
{
  protected:
    bool GetCongestionPrediction(double& probability) const override
    {
        probability = 1.0;
        return true;
    }
};

} // namespace

class TcpHarpCubicV7PolicyTest : public TestCase
{
  public:
    TcpHarpCubicV7PolicyTest()
        : TestCase("HARP-Cubic V7 gives V6 precedence over v2-A random-loss assistance")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpHarpCubicV7> preWarmup = CreateObject<TcpHarpCubicV7>();
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(preWarmup, MakeV7TestState(), 150),
                              70000,
                              "Before v2-A warmup V7 must use V6 normal beta 0.70");

        Ptr<ForcedRandomTcpHarpCubicV7> random =
            CreateObject<ForcedRandomTcpHarpCubicV7>();
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(random, MakeV7TestState(), 150),
                              95000,
                              "A ready high-confidence random prediction must use beta 0.95");
        NS_TEST_ASSERT_MSG_EQ(random->GetMlRandomAssists(),
                              1,
                              "The normal random loss must count one ML assist");

        Ptr<ForcedRandomTcpHarpCubicV7> conservativeFloor =
            CreateObject<ForcedRandomTcpHarpCubicV7>();
        conservativeFloor->SetAttribute("RandomBeta", DoubleValue(0.50));
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(conservativeFloor, MakeV7TestState(), 150),
                              70000,
                              "RandomBeta must never reduce below the V6 normal beta");
        NS_TEST_ASSERT_MSG_EQ(conservativeFloor->GetMlRandomAssists(),
                              0,
                              "An unchanged V6 response must not count as an ML assist");

        Ptr<ForcedCongestedTcpHarpCubicV7> congested =
            CreateObject<ForcedCongestedTcpHarpCubicV7>();
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(congested, MakeV7TestState(), 150),
                              70000,
                              "A congestion prediction must retain V6 normal beta 0.70");
        NS_TEST_ASSERT_MSG_EQ(congested->GetMlRandomAssists(),
                              0,
                              "A congestion prediction must not count as random-loss assistance");

        Ptr<ForcedRandomTcpHarpCubicV7> handover =
            CreateObject<ForcedRandomTcpHarpCubicV7>();
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(handover, MakeV7TestState(), 7, true),
                              85000,
                              "A qualified V6 HARP event must retain its bounded beta 0.85 target");
        NS_TEST_ASSERT_MSG_EQ(handover->GetMlLossDecisions(),
                              0,
                              "A qualified HARP branch must bypass the ML normal-loss hook");
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(handover, MakeV7TestState(), 7),
                              85000,
                              "An active V6 episode must keep its saved safe target");
        NS_TEST_ASSERT_MSG_EQ(handover->GetMlLossDecisions(),
                              0,
                              "An active V6 episode must continue to bypass ML");

        Ptr<ForcedRandomTcpHarpCubicV7> shadow =
            CreateObject<ForcedRandomTcpHarpCubicV7>();
        shadow->SetAttribute("AssistEnabled", BooleanValue(false));
        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(shadow, MakeV7TestState(), 150),
                              70000,
                              "Shadow mode must preserve the V6 normal beta 0.70 target");

        Simulator::Destroy();
    }
};

class TcpHarpCubicV7LateSignalTest : public TestCase
{
  public:
    TcpHarpCubicV7LateSignalTest()
        : TestCase("HARP-Cubic V7 safely revises an ML-assisted Recovery for a late HARP signal")
    {
    }

  private:
    void ReceiveLateSignal()
    {
        const TcpHeader signalOnly = MakeV7Ack(0, false, true);
        m_v7->ObserveAck(signalOnly,
                         m_tcb->m_segmentSize,
                         m_tcb->GetCwndInSegments());

        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(m_v7->GetRecoveryTargetUpdate(m_tcb, target),
                              true,
                              "A qualified late HARP signal must revise the active Recovery");
        NS_TEST_ASSERT_MSG_EQ(target,
                              85000,
                              "Late HARP revision must use the V6 0.70-to-0.85 safe bounds");
        NS_TEST_ASSERT_MSG_EQ(m_v7->GetRecoveryTargetUpdate(m_tcb, target),
                              false,
                              "The late update must remain one-shot");
    }

    void DoRun() override
    {
        m_tcb = MakeV7TestState();
        m_tcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_v7 = CreateObject<ForcedRandomTcpHarpCubicV7>();

        NS_TEST_ASSERT_MSG_EQ(RunV7Recovery(m_v7, m_tcb, 7),
                              95000,
                              "Before a late signal, the normal path may use ML beta 0.95");
        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV7LateSignalTest::ReceiveLateSignal,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<ForcedRandomTcpHarpCubicV7> m_v7;
};

class TcpHarpCubicV7ForkTest : public TestCase
{
  public:
    TcpHarpCubicV7ForkTest()
        : TestCase("HARP-Cubic V7 Fork preserves inherited V6 and v2-A attributes")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpHarpCubicV7> prototype = CreateObject<TcpHarpCubicV7>();
        prototype->SetAttribute("MonitorInterval", TimeValue(MilliSeconds(125)));
        prototype->SetAttribute("WarmupIntervals", UintegerValue(7));
        prototype->SetAttribute("AssistEnabled", BooleanValue(false));
        prototype->SetAttribute("RandomThreshold", DoubleValue(0.0005));
        prototype->SetAttribute("RandomBeta", DoubleValue(0.93));
        prototype->SetAttribute("V2BandwidthWindow", TimeValue(Seconds(3)));
        prototype->SetAttribute("V2BandwidthQuantile", DoubleValue(0.80));
        prototype->SetAttribute("HandoverWindow", TimeValue(MilliSeconds(175)));
        prototype->SetAttribute("HandoverBeta", DoubleValue(0.82));

        Ptr<TcpHarpCubicV7> forked = DynamicCast<TcpHarpCubicV7>(prototype->Fork());
        NS_TEST_ASSERT_MSG_NE(forked, nullptr, "Fork must preserve the V7 concrete type");
        NS_TEST_ASSERT_MSG_EQ(forked->GetName(), "TcpHarpCubicV7", "Fork must report V7");

        TimeValue monitor;
        UintegerValue warmup;
        BooleanValue assist;
        DoubleValue threshold;
        DoubleValue randomBeta;
        TimeValue bandwidthWindow;
        DoubleValue bandwidthQuantile;
        TimeValue handoverWindow;
        DoubleValue handoverBeta;
        forked->GetAttribute("MonitorInterval", monitor);
        forked->GetAttribute("WarmupIntervals", warmup);
        forked->GetAttribute("AssistEnabled", assist);
        forked->GetAttribute("RandomThreshold", threshold);
        forked->GetAttribute("RandomBeta", randomBeta);
        forked->GetAttribute("V2BandwidthWindow", bandwidthWindow);
        forked->GetAttribute("V2BandwidthQuantile", bandwidthQuantile);
        forked->GetAttribute("HandoverWindow", handoverWindow);
        forked->GetAttribute("HandoverBeta", handoverBeta);

        NS_TEST_ASSERT_MSG_EQ(monitor.Get(), MilliSeconds(125), "MonitorInterval must copy");
        NS_TEST_ASSERT_MSG_EQ(warmup.Get(), 7, "WarmupIntervals must copy");
        NS_TEST_ASSERT_MSG_EQ(assist.Get(), false, "AssistEnabled must copy");
        NS_TEST_ASSERT_MSG_EQ_TOL(threshold.Get(), 0.0005, 1e-12, "Threshold must copy");
        NS_TEST_ASSERT_MSG_EQ_TOL(randomBeta.Get(), 0.93, 1e-12, "RandomBeta must copy");
        NS_TEST_ASSERT_MSG_EQ(bandwidthWindow.Get(), Seconds(3), "Bandwidth window must copy");
        NS_TEST_ASSERT_MSG_EQ_TOL(bandwidthQuantile.Get(),
                                  0.80,
                                  1e-12,
                                  "Bandwidth quantile must copy");
        NS_TEST_ASSERT_MSG_EQ(handoverWindow.Get(),
                              MilliSeconds(175),
                              "Inherited HandoverWindow must copy");
        NS_TEST_ASSERT_MSG_EQ_TOL(handoverBeta.Get(),
                                  0.82,
                                  1e-12,
                                  "Inherited HandoverBeta must copy");
        NS_TEST_ASSERT_MSG_EQ(forked->GetMlLossDecisions(),
                              0,
                              "A fork must start with fresh runtime ML state");

        Simulator::Destroy();
    }
};

class TcpHarpCubicV7TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV7TestSuite()
        : TestSuite("tcp-harp-cubic-v7", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV7PolicyTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV7LateSignalTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV7ForkTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV7TestSuite g_tcpHarpCubicV7TestSuite;

} // namespace ns3
