/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v21.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"

namespace ns3
{

class TcpHarpCubicV21AdaptiveBetaTest : public TestCase
{
  public:
    TcpHarpCubicV21AdaptiveBetaTest()
        : TestCase("HARP-Cubic V2.1 uses conservative DATA beta without repeated backoff")
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

        Ptr<TcpSocketState> normalTcb = MakeTcb();
        Ptr<TcpHarpCubicV21> normal = CreateObject<TcpHarpCubicV21>();
        normal->ObserveAck(largeRangeAck,
                           normalTcb->m_segmentSize,
                           normalTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(normal->GetSsThresh(normalTcb, normalTcb->m_bytesInFlight),
                              70000,
                              "Normal loss must keep CUBIC beta=0.7");

        Ptr<TcpSocketState> ackTcb = MakeTcb();
        Ptr<TcpHarpCubicV21> ackPath = CreateObject<TcpHarpCubicV21>();
        TcpHarpCubicV21::NotifyHandover(TcpHarpCubicV21::HandoverDirection::ACK_PATH);
        ackPath->ObserveAck(largeRangeAck,
                            ackTcb->m_segmentSize,
                            ackTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(ackPath->GetSsThresh(ackTcb, ackTcb->m_bytesInFlight),
                              75000,
                              "ACK-path handover should keep beta=0.75");

        // V2.1 的唯一差異：DATA-path 使用 beta=0.80，而不是 V2 的 0.85。
        Ptr<TcpSocketState> dataTcb = MakeTcb();
        Ptr<TcpHarpCubicV21> dataPath = CreateObject<TcpHarpCubicV21>();
        TcpHarpCubicV21::NotifyHandover(TcpHarpCubicV21::HandoverDirection::DATA_PATH);
        dataPath->ObserveAck(largeRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              80000,
                              "DATA-path handover should use beta=0.80");

        dataTcb->m_cWnd = 80000;
        dataTcb->m_bytesInFlight = 80000;
        const TcpHeader smallRangeAck = MakeSackAck(1);
        dataPath->ObserveAck(smallRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              80000,
                              "A follow-up recovery must reuse the first target cwnd");
    }
};

class TcpHarpCubicV21TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV21TestSuite()
        : TestSuite("tcp-harp-cubic-v21", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV21AdaptiveBetaTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV21TestSuite g_tcpHarpCubicV21TestSuite;

} // namespace ns3
