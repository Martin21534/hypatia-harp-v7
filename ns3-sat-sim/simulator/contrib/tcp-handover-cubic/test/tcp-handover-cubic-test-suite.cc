/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/boolean.h"
#include "ns3/tcp-harp-recovery.h"
#include "ns3/tcp-handover-cubic.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/tcp-prr-recovery.h"
#include "ns3/test.h"

#include <algorithm>

namespace ns3
{

class TcpHandoverCubicProtectionTest : public TestCase
{
  public:
    TcpHandoverCubicProtectionTest()
        : TestCase("A handover candidate preserves cwnd only when protection is enabled")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
        tcb->m_segmentSize = 1000;
        tcb->m_cWnd = 100000;

        TcpHeader header;
        header.SetFlags(TcpHeader::ACK);
        header.SetAckNumber(SequenceNumber32(1000));
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock({SequenceNumber32(2000), SequenceNumber32(52000)});
        header.AppendOption(sack);

        TcpHandoverCubic::NotifyHandover();

        Ptr<TcpHandoverCubic> protectedCubic = CreateObject<TcpHandoverCubic>();
        protectedCubic->ObserveAck(header, tcb->m_segmentSize, tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(protectedCubic->GetSsThresh(tcb, tcb->m_cWnd),
                              100000,
                              "Protection should preserve the current cwnd");

        Ptr<TcpHandoverCubic> baselineCubic = CreateObject<TcpHandoverCubic>();
        baselineCubic->SetAttribute("EnableHandoverProtection", BooleanValue(false));
        baselineCubic->ObserveAck(header, tcb->m_segmentSize, tcb->GetCwndInSegments());
        NS_TEST_ASSERT_MSG_EQ(baselineCubic->GetSsThresh(tcb, tcb->m_cWnd),
                              70000,
                              "Disabled protection should use CUBIC's beta decrease");
    }
};

class TcpHarpRecoveryTest : public TestCase
{
  public:
    TcpHarpRecoveryTest()
        : TestCase("HARP recovery prevents transient cwnd reduction only during protection")
    {
    }

  private:
    void CwndTrace(uint32_t oldCwnd [[maybe_unused]], uint32_t newCwnd)
    {
        m_minObservedCwnd = std::min(m_minObservedCwnd, newCwnd);
    }

    static Ptr<TcpSocketState> MakeTcb()
    {
        Ptr<TcpSocketState> tcb = CreateObject<TcpSocketState>();
        tcb->m_segmentSize = 1000;
        tcb->m_cWnd = 100000;
        tcb->m_cWndInfl = 100000;
        tcb->m_bytesInFlight = 35000;
        return tcb;
    }

    static TcpHeader MakeReorderingAck()
    {
        TcpHeader header;
        header.SetFlags(TcpHeader::ACK);
        header.SetAckNumber(SequenceNumber32(1000));
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock({SequenceNumber32(2000), SequenceNumber32(52000)});
        header.AppendOption(sack);
        return header;
    }

    void DoRun() override
    {
        // 改進處測試：確認受保護 recovery 從頭到尾沒有出現暫時降窗 trace。
        Ptr<TcpSocketState> protectedTcb = MakeTcb();
        m_minObservedCwnd = protectedTcb->m_cWnd;
        protectedTcb->TraceConnectWithoutContext(
            "CongestionWindow",
            MakeCallback(&TcpHarpRecoveryTest::CwndTrace, this));

        Ptr<TcpHandoverCubic> protectedCubic = CreateObject<TcpHandoverCubic>();
        const TcpHeader header = MakeReorderingAck();
        TcpHandoverCubic::NotifyHandover();
        protectedCubic->ObserveAck(header,
                                   protectedTcb->m_segmentSize,
                                   protectedTcb->GetCwndInSegments());
        protectedTcb->m_ssThresh =
            protectedCubic->GetSsThresh(protectedTcb, protectedTcb->m_bytesInFlight);

        Ptr<TcpHarpRecovery> harpRecovery = CreateObject<TcpHarpRecovery>();
        harpRecovery->EnterRecovery(protectedTcb, 3, 100000, 0);
        protectedTcb->m_bytesInFlight = 30000;
        harpRecovery->DoRecovery(protectedTcb, 1000, false);

        NS_TEST_ASSERT_MSG_EQ(protectedTcb->m_cWnd.Get(),
                              100000,
                              "Protected recovery should preserve the pre-recovery cwnd");
        NS_TEST_ASSERT_MSG_EQ(m_minObservedCwnd,
                              100000,
                              "Protected recovery must not emit a temporary cwnd reduction");

        // 普通 loss 測試：TcpHarpRecovery 必須和 ns-3 原始 PRR 得到相同結果。
        Ptr<TcpSocketState> harpNormalTcb = MakeTcb();
        Ptr<TcpSocketState> prrTcb = MakeTcb();
        Ptr<TcpHandoverCubic> disabledCubic = CreateObject<TcpHandoverCubic>();
        disabledCubic->SetAttribute("EnableHandoverProtection", BooleanValue(false));
        disabledCubic->ObserveAck(header,
                                  harpNormalTcb->m_segmentSize,
                                  harpNormalTcb->GetCwndInSegments());
        const uint32_t normalSsThresh =
            disabledCubic->GetSsThresh(harpNormalTcb, harpNormalTcb->m_bytesInFlight);
        harpNormalTcb->m_ssThresh = normalSsThresh;
        prrTcb->m_ssThresh = normalSsThresh;

        Ptr<TcpHarpRecovery> normalHarpRecovery = CreateObject<TcpHarpRecovery>();
        Ptr<TcpPrrRecovery> normalPrrRecovery = CreateObject<TcpPrrRecovery>();
        normalHarpRecovery->EnterRecovery(harpNormalTcb, 3, 100000, 0);
        normalPrrRecovery->EnterRecovery(prrTcb, 3, 100000, 0);

        NS_TEST_ASSERT_MSG_EQ(harpNormalTcb->m_cWnd.Get(),
                              prrTcb->m_cWnd.Get(),
                              "Outside protection HARP recovery should match standard PRR");
    }

    uint32_t m_minObservedCwnd{0};
};

class TcpHandoverCubicTestSuite : public TestSuite
{
  public:
    TcpHandoverCubicTestSuite()
        : TestSuite("tcp-handover-cubic", Type::UNIT)
    {
        AddTestCase(new TcpHandoverCubicProtectionTest(), TestCase::Duration::QUICK);
        AddTestCase(new TcpHarpRecoveryTest(), TestCase::Duration::QUICK);
    }
};

static TcpHandoverCubicTestSuite g_tcpHandoverCubicTestSuite;

} // namespace ns3
