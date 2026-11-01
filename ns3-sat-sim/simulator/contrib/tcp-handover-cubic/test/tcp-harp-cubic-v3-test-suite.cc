/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v3.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"

namespace ns3
{

class TcpHarpCubicV3SafeTargetTest : public TestCase
{
  public:
    TcpHarpCubicV3SafeTargetTest()
        : TestCase("HARP-Cubic V3 bounds a handover target using recovery-entry flight")
    {
    }

  private:
    static Ptr<TcpSocketState> MakeTcb(uint32_t bytesInFlight)
    {
        Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
        tcb->m_segmentSize = 1000;
        tcb->m_cWnd = 100000;
        tcb->m_cWndInfl = 100000;
        tcb->m_bytesInFlight = bytesInFlight;
        return tcb;
    }

    static TcpHeader MakeSackAck(uint32_t rangeSegments)
    {
        TcpHeader header;
        header.SetFlags(TcpHeader::ACK);
        header.SetAckNumber(SequenceNumber32(1000));
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock(
            {SequenceNumber32(2000), SequenceNumber32(1000 + rangeSegments * 1000)});
        header.AppendOption(sack);
        return header;
    }

    void DoRun() override
    {
        const TcpHeader largeRangeAck = MakeSackAck(150);

        // 非換手 loss 完全維持原始 CUBIC beta=0.7，不套用 safe cap。
        Ptr<TcpSocketState> normalTcb = MakeTcb(60000);
        Ptr<TcpHarpCubicV3> normal = CreateObject<TcpHarpCubicV3>();
        normal->ObserveAck(largeRangeAck,
                           normalTcb->m_segmentSize,
                           normalTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(normal->GetSsThresh(normalTcb, normalTcb->m_bytesInFlight),
                              70000,
                              "Normal loss must remain identical to CUBIC beta=0.7");

        // DATA handover 的 0.85 target 高於 flight 時，應停在 recovery-entry flight。
        Ptr<TcpSocketState> dataTcb = MakeTcb(73000);
        Ptr<TcpHarpCubicV3> dataPath = CreateObject<TcpHarpCubicV3>();
        TcpHarpCubicV3::NotifyHandover(TcpHarpCubicV3::HandoverDirection::DATA_PATH);
        dataPath->ObserveAck(largeRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              73000,
                              "Safe target must not exceed recovery-entry flight");

        // follow-up 必須維持 V2 邏輯，只重用第一次 target，不再次套用 flight cap。
        dataTcb->m_cWnd = 73000;
        dataTcb->m_bytesInFlight = 60000;
        const TcpHeader smallRangeAck = MakeSackAck(1);
        dataPath->ObserveAck(smallRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              73000,
                              "Follow-up recovery must reuse V2's active target logic");

        // flight 已高於 0.85 target 時，仍使用原本的 adaptive target。
        Ptr<TcpSocketState> highFlightTcb = MakeTcb(90000);
        Ptr<TcpHarpCubicV3> highFlight = CreateObject<TcpHarpCubicV3>();
        TcpHarpCubicV3::NotifyHandover(TcpHarpCubicV3::HandoverDirection::DATA_PATH);
        highFlight->ObserveAck(largeRangeAck,
                               highFlightTcb->m_segmentSize,
                               highFlightTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(highFlight->GetSsThresh(highFlightTcb,
                                                       highFlightTcb->m_bytesInFlight),
                              85000,
                              "Flight cap must not raise the configured adaptive target");

        // flight 低於普通 CUBIC target 時，不可比原始 CUBIC 降得更低。
        Ptr<TcpSocketState> lowFlightTcb = MakeTcb(60000);
        Ptr<TcpHarpCubicV3> lowFlight = CreateObject<TcpHarpCubicV3>();
        TcpHarpCubicV3::NotifyHandover(TcpHarpCubicV3::HandoverDirection::DATA_PATH);
        lowFlight->ObserveAck(largeRangeAck,
                              lowFlightTcb->m_segmentSize,
                              lowFlightTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(lowFlight->GetSsThresh(lowFlightTcb,
                                                     lowFlightTcb->m_bytesInFlight),
                              70000,
                              "Safe target must retain the normal CUBIC floor");
    }
};

class TcpHarpCubicV3TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV3TestSuite()
        : TestSuite("tcp-harp-cubic-v3", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV3SafeTargetTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV3TestSuite g_tcpHarpCubicV3TestSuite;

} // namespace ns3
