/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v5.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-harp.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/test.h"

namespace ns3
{

namespace
{

Ptr<TcpSocketState>
MakeTestState()
{
    Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
    tcb->m_segmentSize = 1000;
    tcb->m_cWnd = 100000;
    tcb->m_cWndInfl = 100000;
    tcb->m_bytesInFlight = 73000;
    return tcb;
}

TcpHeader
MakeAck(bool withSack, bool withEcho, uint16_t epoch = 7, uint32_t sackRight = 151000)
{
    TcpHeader header;
    header.SetFlags(TcpHeader::ACK);
    header.SetAckNumber(SequenceNumber32(1000));

    if (withEcho)
    {
        Ptr<TcpOptionHarp> echo = CreateObject<TcpOptionHarp>();
        echo->SetSignal(TcpOptionHarp::Signal::DATA_ECHO);
        echo->SetEpoch(epoch);
        header.AppendOption(echo);
    }
    if (withSack)
    {
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock({SequenceNumber32(1500), SequenceNumber32(sackRight)});
        header.AppendOption(sack);
    }
    return header;
}

} // namespace

class TcpHarpCubicV5PacketSignalTest : public TestCase
{
  public:
    TcpHarpCubicV5PacketSignalTest()
        : TestCase("HARP-Cubic V5 uses an ACK-carried echo and keeps the V3 safe target")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
        tcb->m_segmentSize = 1000;
        tcb->m_cWnd = 100000;
        tcb->m_cWndInfl = 100000;
        tcb->m_bytesInFlight = 73000;

        TcpHeader header;
        header.SetFlags(TcpHeader::ACK);
        header.SetAckNumber(SequenceNumber32(1000));

        Ptr<TcpOptionHarp> echo = CreateObject<TcpOptionHarp>();
        echo->SetSignal(TcpOptionHarp::Signal::DATA_ECHO);
        echo->SetEpoch(7);
        NS_TEST_ASSERT_MSG_EQ(header.AppendOption(echo), true, "HARP echo must fit");

        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock({SequenceNumber32(2000), SequenceNumber32(151000)});
        NS_TEST_ASSERT_MSG_EQ(header.AppendOption(sack), true, "SACK must fit beside HARP echo");

        // Verify that the signal survives the real TCP-header wire format.
        Ptr<Packet> wirePacket = Create<Packet>();
        wirePacket->AddHeader(header);
        TcpHeader decodedHeader;
        wirePacket->RemoveHeader(decodedHeader);
        Ptr<const TcpOptionHarp> decodedEcho =
            DynamicCast<const TcpOptionHarp>(decodedHeader.GetOption(TcpOption::HARP));
        NS_TEST_ASSERT_MSG_NE(decodedEcho, nullptr, "Serialized ACK must contain HARP echo");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(decodedEcho->GetSignal()),
                              static_cast<uint8_t>(TcpOptionHarp::Signal::DATA_ECHO),
                              "Serialized HARP signal must be preserved");
        NS_TEST_ASSERT_MSG_EQ(decodedEcho->GetEpoch(), 7, "Serialized HARP epoch must be preserved");

        Ptr<TcpHarpCubicV5> v5 = CreateObject<TcpHarpCubicV5>();
        v5->ObserveAck(decodedHeader, tcb->m_segmentSize, tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(v5->GetSsThresh(tcb, tcb->m_bytesInFlight),
                              73000,
                              "Packet-carried DATA_ECHO must select the bounded safe target");

        TcpHeader normalHeader;
        normalHeader.SetFlags(TcpHeader::ACK);
        normalHeader.SetAckNumber(SequenceNumber32(1000));
        normalHeader.AppendOption(sack);
        Ptr<TcpHarpCubicV5> normal = CreateObject<TcpHarpCubicV5>();
        normal->ObserveAck(normalHeader, tcb->m_segmentSize, tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(normal->GetSsThresh(tcb, tcb->m_bytesInFlight),
                              70000,
                              "Without an on-wire signal V5 must fall back to normal CUBIC");
    }
};

class TcpHarpCubicV5EarlySignalTest : public TestCase
{
  public:
    TcpHarpCubicV5EarlySignalTest()
        : TestCase("HARP-Cubic V5 accepts an ECHO received before Recovery")
    {
    }

  private:
    void RunRecovery()
    {
        const TcpHeader sackAck = MakeAck(true, false);
        m_v5->ObserveAck(sackAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(m_v5->GetSsThresh(m_tcb, m_tcb->m_bytesInFlight),
                              73000,
                              "An ECHO 100 ms before Recovery must select the safe target");
    }

    void DoRun() override
    {
        m_tcb = MakeTestState();
        m_v5 = CreateObject<TcpHarpCubicV5>();
        const TcpHeader echoAck = MakeAck(false, true);
        m_v5->ObserveAck(echoAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
        Simulator::Schedule(MilliSeconds(100), &TcpHarpCubicV5EarlySignalTest::RunRecovery, this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV5> m_v5;
};

class TcpHarpCubicV5LateSignalTest : public TestCase
{
  public:
    TcpHarpCubicV5LateSignalTest()
        : TestCase("HARP-Cubic V5 revises an active Recovery target for a late ECHO")
    {
    }

  private:
    void ReceiveLateEcho()
    {
        const TcpHeader echoAck = MakeAck(false, true);
        m_v5->ObserveAck(echoAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());

        uint32_t updatedTarget = 0;
        NS_TEST_ASSERT_MSG_EQ(m_v5->GetRecoveryTargetUpdate(m_tcb, updatedTarget),
                              true,
                              "A new ECHO within 250 ms must revise active Recovery");
        NS_TEST_ASSERT_MSG_EQ(updatedTarget, 73000, "Late ECHO must recover the bounded target");

        // TcpSocketBase changes only ssthresh.  cwnd remains under PRR control.
        m_tcb->m_ssThresh = updatedTarget;
        NS_TEST_ASSERT_MSG_EQ(m_tcb->m_cWnd.Get(),
                              70000,
                              "Late ECHO must not raise cwnd directly or bypass PRR");
        NS_TEST_ASSERT_MSG_EQ(m_tcb->m_ssThresh.Get(),
                              73000,
                              "Late ECHO must update the Recovery target");
    }

    void DoRun() override
    {
        m_tcb = MakeTestState();
        m_tcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_v5 = CreateObject<TcpHarpCubicV5>();

        const TcpHeader sackAck = MakeAck(true, false);
        m_v5->ObserveAck(sackAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
        const uint32_t normalTarget = m_v5->GetSsThresh(m_tcb, m_tcb->m_bytesInFlight);
        NS_TEST_ASSERT_MSG_EQ(normalTarget, 70000, "Recovery must initially use normal CUBIC");
        m_tcb->m_ssThresh = normalTarget;
        m_tcb->m_cWnd = normalTarget;

        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV5LateSignalTest::ReceiveLateEcho,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV5> m_v5;
};

class TcpHarpCubicV5LateSignalSafetyTest : public TestCase
{
  public:
    TcpHarpCubicV5LateSignalSafetyTest()
        : TestCase("HARP-Cubic V5 rejects expired or post-Recovery late ECHOs")
    {
    }

  private:
    void ReceiveExpiredEcho()
    {
        const TcpHeader echoAck = MakeAck(false, true, 7);
        m_expiredV5->ObserveAck(echoAck,
                                m_expiredTcb->m_segmentSize,
                                m_expiredTcb->GetCwndInSegments());
        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(m_expiredV5->GetRecoveryTargetUpdate(m_expiredTcb, target),
                              false,
                              "ECHO after 250 ms must not revise Recovery");
    }

    void ReceivePostRecoveryEcho()
    {
        m_postTcb->m_congState = TcpSocketState::CA_OPEN;
        const TcpHeader echoAck = MakeAck(false, true, 8);
        m_postV5->ObserveAck(echoAck,
                             m_postTcb->m_segmentSize,
                             m_postTcb->GetCwndInSegments());
        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(m_postV5->GetRecoveryTargetUpdate(m_postTcb, target),
                              false,
                              "ECHO after Recovery exit must not cause a cwnd burst");
        NS_TEST_ASSERT_MSG_EQ(m_postTcb->m_cWnd.Get(),
                              70000,
                              "Post-Recovery ECHO must not restore cwnd directly");
    }

    void DoRun() override
    {
        m_expiredTcb = MakeTestState();
        m_expiredTcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_expiredV5 = CreateObject<TcpHarpCubicV5>();
        const TcpHeader sackAck = MakeAck(true, false);
        m_expiredV5->ObserveAck(sackAck,
                                m_expiredTcb->m_segmentSize,
                                m_expiredTcb->GetCwndInSegments());
        m_expiredTcb->m_ssThresh =
            m_expiredV5->GetSsThresh(m_expiredTcb, m_expiredTcb->m_bytesInFlight);
        Simulator::Schedule(NanoSeconds(250000001),
                            &TcpHarpCubicV5LateSignalSafetyTest::ReceiveExpiredEcho,
                            this);
        Simulator::Run();
        Simulator::Destroy();

        m_postTcb = MakeTestState();
        m_postTcb->m_congState = TcpSocketState::CA_RECOVERY;
        m_postV5 = CreateObject<TcpHarpCubicV5>();
        m_postV5->ObserveAck(sackAck,
                             m_postTcb->m_segmentSize,
                             m_postTcb->GetCwndInSegments());
        m_postTcb->m_ssThresh = m_postV5->GetSsThresh(m_postTcb, m_postTcb->m_bytesInFlight);
        m_postTcb->m_cWnd = 70000;
        Simulator::Schedule(MilliSeconds(100),
                            &TcpHarpCubicV5LateSignalSafetyTest::ReceivePostRecoveryEcho,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_expiredTcb;
    Ptr<TcpHarpCubicV5> m_expiredV5;
    Ptr<TcpSocketState> m_postTcb;
    Ptr<TcpHarpCubicV5> m_postV5;
};

class TcpHarpCubicV5FixedWindowTest : public TestCase
{
  public:
    TcpHarpCubicV5FixedWindowTest()
        : TestCase("HARP-Cubic V5 duplicate ECHOs do not extend the 250 ms window")
    {
    }

  private:
    void ReceiveDuplicateEcho()
    {
        const TcpHeader echoAck = MakeAck(false, true, 7);
        m_v5->ObserveAck(echoAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
    }

    void RunExpiredRecovery()
    {
        m_tcb->m_congState = TcpSocketState::CA_RECOVERY;
        const TcpHeader sackAck = MakeAck(true, false);
        m_v5->ObserveAck(sackAck, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(m_v5->GetSsThresh(m_tcb, m_tcb->m_bytesInFlight),
                              70000,
                              "A duplicate ECHO must not refresh the first ECHO time");
    }

    void DoRun() override
    {
        m_tcb = MakeTestState();
        m_v5 = CreateObject<TcpHarpCubicV5>();
        const TcpHeader firstEcho = MakeAck(false, true, 7);
        m_v5->ObserveAck(firstEcho, m_tcb->m_segmentSize, m_tcb->GetCwndInSegments());
        Simulator::Schedule(MilliSeconds(200),
                            &TcpHarpCubicV5FixedWindowTest::ReceiveDuplicateEcho,
                            this);
        Simulator::Schedule(NanoSeconds(250000001),
                            &TcpHarpCubicV5FixedWindowTest::RunExpiredRecovery,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpHarpCubicV5> m_v5;
};

class TcpHarpCubicV5LowRatioTest : public TestCase
{
  public:
    TcpHarpCubicV5LowRatioTest()
        : TestCase("HARP-Cubic V5 does not undo a low-ratio Recovery")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpSocketState> tcb = MakeTestState();
        tcb->m_congState = TcpSocketState::CA_RECOVERY;
        Ptr<TcpHarpCubicV5> v5 = CreateObject<TcpHarpCubicV5>();

        // One segment of ACK range over a 100-segment cwnd gives ratio 0.01.
        const TcpHeader smallRangeAck = MakeAck(true, false, 7, 2000);
        v5->ObserveAck(smallRangeAck, tcb->m_segmentSize, tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(v5->GetSsThresh(tcb, tcb->m_bytesInFlight),
                              70000,
                              "Low ratio must use normal CUBIC");

        const TcpHeader echoAck = MakeAck(false, true, 7);
        v5->ObserveAck(echoAck, tcb->m_segmentSize, tcb->GetCwndInSegments());
        uint32_t target = 0;
        NS_TEST_ASSERT_MSG_EQ(v5->GetRecoveryTargetUpdate(tcb, target),
                              false,
                              "ECHO must not undo a Recovery whose saved ratio was below 0.015");
    }
};

class TcpSocketBaseHarpEchoHarness : public TcpSocketBase
{
  public:
    using TcpSocketBase::AddOptionHarp;
    using TcpSocketBase::ProcessOptionHarp;
};

class TcpHarpCubicV5EchoDurationTest : public TestCase
{
  public:
    TcpHarpCubicV5EchoDurationTest()
        : TestCase("HARP V5 receiver ECHO has a fixed non-refreshing 250 ms duration")
    {
    }

  private:
    void RepeatSameMark()
    {
        m_socket->ProcessOptionHarp(m_markHeader);
    }

    void CheckBoundary()
    {
        TcpHeader ack;
        ack.SetFlags(TcpHeader::ACK);
        m_socket->AddOptionHarp(ack);
        NS_TEST_ASSERT_MSG_EQ(ack.HasOption(TcpOption::HARP),
                              true,
                              "ECHO must still be present at exactly 250 ms");
    }

    void CheckExpired()
    {
        TcpHeader ack;
        ack.SetFlags(TcpHeader::ACK);
        m_socket->AddOptionHarp(ack);
        NS_TEST_ASSERT_MSG_EQ(ack.HasOption(TcpOption::HARP),
                              false,
                              "ECHO must stop after 250 ms despite a repeated MARK");
    }

    void DoRun() override
    {
        m_socket = CreateObject<TcpSocketBaseHarpEchoHarness>();
        m_markHeader.SetFlags(TcpHeader::ACK);
        Ptr<TcpOptionHarp> mark = CreateObject<TcpOptionHarp>();
        mark->SetSignal(TcpOptionHarp::Signal::DATA_MARK);
        mark->SetEpoch(11);
        m_markHeader.AppendOption(mark);
        m_socket->ProcessOptionHarp(m_markHeader);

        Simulator::Schedule(MilliSeconds(200),
                            &TcpHarpCubicV5EchoDurationTest::RepeatSameMark,
                            this);
        Simulator::Schedule(MilliSeconds(250),
                            &TcpHarpCubicV5EchoDurationTest::CheckBoundary,
                            this);
        Simulator::Schedule(NanoSeconds(250000001),
                            &TcpHarpCubicV5EchoDurationTest::CheckExpired,
                            this);
        Simulator::Run();
        Simulator::Destroy();
    }

    Ptr<TcpSocketBaseHarpEchoHarness> m_socket;
    TcpHeader m_markHeader;
};

class TcpHarpCubicV5TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV5TestSuite()
        : TestSuite("tcp-harp-cubic-v5", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV5PacketSignalTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5EarlySignalTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5LateSignalTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5LateSignalSafetyTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5FixedWindowTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5LowRatioTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpCubicV5EchoDurationTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV5TestSuite g_tcpHarpCubicV5TestSuite;

} // namespace ns3
