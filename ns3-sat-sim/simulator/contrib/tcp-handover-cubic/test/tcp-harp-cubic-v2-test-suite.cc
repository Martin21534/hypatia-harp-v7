/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v2.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"

namespace ns3
{

class TcpHarpCubicV2AdaptiveBetaTest : public TestCase
{
  public:
    TcpHarpCubicV2AdaptiveBetaTest()
        : TestCase("HARP-Cubic V2 selects direction-aware beta without repeated backoff")
    {
    }

  private:
    static Ptr<TcpSocketState> MakeTcb()
    {
        Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
        tcb->m_segmentSize = 1000;
        tcb->m_cWnd = 100000;
        tcb->m_cWndInfl = 100000;
        tcb->m_bytesInFlight = 100000;
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

        // 沒有換手時，不論 ACK range 多大，都維持原始 CUBIC beta=0.7。
        Ptr<TcpSocketState> normalTcb = MakeTcb();
        Ptr<TcpHarpCubicV2> normal = CreateObject<TcpHarpCubicV2>();
        normal->ObserveAck(largeRangeAck,
                           normalTcb->m_segmentSize,
                           normalTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(normal->GetSsThresh(normalTcb, normalTcb->m_bytesInFlight),
                              70000,
                              "Normal loss must keep CUBIC beta=0.7");

        // ACK-path 通過 ratio 門檻後固定使用 beta=0.75。
        Ptr<TcpSocketState> ackTcb = MakeTcb();
        Ptr<TcpHarpCubicV2> ackPath = CreateObject<TcpHarpCubicV2>();
        TcpHarpCubicV2::NotifyHandover(TcpHarpCubicV2::HandoverDirection::ACK_PATH);
        ackPath->ObserveAck(largeRangeAck,
                            ackTcb->m_segmentSize,
                            ackTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(ackPath->GetSsThresh(ackTcb, ackTcb->m_bytesInFlight),
                              75000,
                              "ACK-path handover should receive only partial protection");

        // DATA-path 通過 ratio 門檻後固定使用 beta=0.85。
        Ptr<TcpSocketState> dataTcb = MakeTcb();
        Ptr<TcpHarpCubicV2> dataPath = CreateObject<TcpHarpCubicV2>();
        TcpHarpCubicV2::NotifyHandover(TcpHarpCubicV2::HandoverDirection::DATA_PATH);
        dataPath->ObserveAck(largeRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              85000,
                              "DATA-path handover should use beta=0.85");

        // 同一次 250ms handover 的後續 Recovery 不可再做 0.85 倍降窗。
        dataTcb->m_cWnd = 85000;
        dataTcb->m_bytesInFlight = 85000;
        const TcpHeader smallRangeAck = MakeSackAck(1);
        dataPath->ObserveAck(smallRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              85000,
                              "A follow-up recovery must reuse the first target cwnd");
    }
};

class TcpHarpCubicV2TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV2TestSuite()
        : TestSuite("tcp-harp-cubic-v2", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV2AdaptiveBetaTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV2TestSuite g_tcpHarpCubicV2TestSuite;

} // namespace ns3
