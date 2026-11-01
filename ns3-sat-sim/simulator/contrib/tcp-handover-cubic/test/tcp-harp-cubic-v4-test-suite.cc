/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/tcp-harp-cubic-v4.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"
#include "ns3/simulator.h"

namespace ns3
{

class TcpHarpCubicV4SafeTargetTest : public TestCase
{
  public:
    TcpHarpCubicV4SafeTargetTest()
        : TestCase("HARP-Cubic V4 keeps V3 target logic with live notification")
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

        Ptr<TcpSocketState> normalTcb = MakeTcb(60000);
        Ptr<TcpHarpCubicV4> normal = CreateObject<TcpHarpCubicV4>();
        normal->ObserveAck(largeRangeAck,
                           normalTcb->m_segmentSize,
                           normalTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(normal->GetSsThresh(normalTcb, normalTcb->m_bytesInFlight),
                              70000,
                              "Normal loss must remain identical to V3/CUBIC beta=0.7");

        Ptr<TcpSocketState> dataTcb = MakeTcb(73000);
        Ptr<TcpHarpCubicV4> dataPath = CreateObject<TcpHarpCubicV4>();
        TcpHarpCubicV4::NotifyHandover(TcpHarpCubicV4::HandoverDirection::DATA_PATH);
        dataPath->ObserveAck(largeRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              73000,
                              "V4 safe target must equal V3 for the same notification");

        dataTcb->m_cWnd = 73000;
        dataTcb->m_bytesInFlight = 60000;
        const TcpHeader smallRangeAck = MakeSackAck(1);
        dataPath->ObserveAck(smallRangeAck,
                             dataTcb->m_segmentSize,
                             dataTcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(dataPath->GetSsThresh(dataTcb, dataTcb->m_bytesInFlight),
                              73000,
                              "V4 follow-up must reuse the first safe target");

        // DATA 與 ACK 在 1ms 內先後使用新路徑時，必須合併成同一次事件，
        // 並採 DATA beta。若錯誤地讓較晚 ACK 覆蓋 DATA，結果會是 75000。
        uint32_t mergedTarget = 0;
        Simulator::Schedule(MicroSeconds(500),
                            &TcpHarpCubicV4::NotifyHandover,
                            TcpHarpCubicV4::HandoverDirection::ACK_PATH);
        Simulator::Schedule(MilliSeconds(1),
                            [&largeRangeAck, &mergedTarget]() {
                                Ptr<TcpSocketState> mergedTcb = MakeTcb(80000);
                                Ptr<TcpHarpCubicV4> merged = CreateObject<TcpHarpCubicV4>();
                                merged->ObserveAck(largeRangeAck,
                                                   mergedTcb->m_segmentSize,
                                                   mergedTcb->GetCwndInSegments());
                                mergedTarget =
                                    merged->GetSsThresh(mergedTcb, mergedTcb->m_bytesInFlight);
                            });
        Simulator::Run();
        Simulator::Destroy();
        NS_TEST_ASSERT_MSG_EQ(mergedTarget,
                              80000,
                              "Near-simultaneous DATA+ACK must keep DATA beta priority");

        // 超過合併範圍的 ACK 是另一個事件，仍應使用 ACK beta=0.75。
        uint32_t separateAckTarget = 0;
        Simulator::Schedule(MilliSeconds(2),
                            &TcpHarpCubicV4::NotifyHandover,
                            TcpHarpCubicV4::HandoverDirection::ACK_PATH);
        Simulator::Schedule(MilliSeconds(3),
                            [&largeRangeAck, &separateAckTarget]() {
                                Ptr<TcpSocketState> ackTcb = MakeTcb(80000);
                                Ptr<TcpHarpCubicV4> ack = CreateObject<TcpHarpCubicV4>();
                                ack->ObserveAck(largeRangeAck,
                                                ackTcb->m_segmentSize,
                                                ackTcb->GetCwndInSegments());
                                separateAckTarget =
                                    ack->GetSsThresh(ackTcb, ackTcb->m_bytesInFlight);
                            });
        Simulator::Run();
        Simulator::Destroy();
        NS_TEST_ASSERT_MSG_EQ(separateAckTarget,
                              75000,
                              "ACK outside the merge window must retain ACK beta");
    }
};

class TcpHarpCubicV4TestSuite : public TestSuite
{
  public:
    TcpHarpCubicV4TestSuite()
        : TestSuite("tcp-harp-cubic-v4", Type::UNIT)
    {
        AddTestCase(new TcpHarpCubicV4SafeTargetTest(), TestCase::Duration::QUICK);
    }
};

static TcpHarpCubicV4TestSuite g_tcpHarpCubicV4TestSuite;

} // namespace ns3
