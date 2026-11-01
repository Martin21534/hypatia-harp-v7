/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-handover-cubic.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/nstime.h"

#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHandoverCubic");
NS_OBJECT_ENSURE_REGISTERED(TcpHandoverCubic);

int64_t TcpHandoverCubic::s_lastHandoverTimeNs = -1;
const TcpSocketState* TcpHandoverCubic::s_lastDecisionTcb = nullptr;
int64_t TcpHandoverCubic::s_lastDecisionTimeNs = -1;
bool TcpHandoverCubic::s_lastDecisionPreserveCwnd = false;

TypeId
TcpHandoverCubic::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHandoverCubic")
            .SetParent<TcpCubic>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHandoverCubic>()
            .AddAttribute("EnableHandoverProtection",
                          "Enable ACK-range-based handover protection",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHandoverCubic::m_enableHandoverProtection),
                          MakeBooleanChecker())
            .AddAttribute("DetectionWindow",
                          "Time after handover in which reordering can be detected",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHandoverCubic::m_detectionWindow),
                          MakeTimeChecker())
            .AddAttribute("ProtectionWindow",
                          "Protection duration after a handover-reordering detection",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHandoverCubic::m_protectionWindow),
                          MakeTimeChecker())
            .AddAttribute("AckRangeRatioThreshold",
                          "Minimum ACK-to-FACK range/cwnd ratio for detection",
                          DoubleValue(0.015),
                          MakeDoubleAccessor(&TcpHandoverCubic::m_ackRangeRatioThreshold),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

TcpHandoverCubic::TcpHandoverCubic()
    : TcpCubic()
{
}

TcpHandoverCubic::TcpHandoverCubic(const TcpHandoverCubic& other)
    : TcpCubic(other),
      m_enableHandoverProtection(other.m_enableHandoverProtection),
      m_detectionWindow(other.m_detectionWindow),
      m_protectionWindow(other.m_protectionWindow),
      m_ackRangeRatioThreshold(other.m_ackRangeRatioThreshold),
      m_lastAckRangeSegments(other.m_lastAckRangeSegments),
      m_lastAckRangeValid(other.m_lastAckRangeValid),
      m_handoverProtectionUntilNs(other.m_handoverProtectionUntilNs)
{
}

std::string
TcpHandoverCubic::GetName() const
{
    return "TcpHandoverCubic";
}

void
TcpHandoverCubic::NotifyHandover()
{
    s_lastHandoverTimeNs = Simulator::Now().GetNanoSeconds();
    std::cout << "TCP_HANDOVER_NOTIFICATION"
              << " time_ns=" << s_lastHandoverTimeNs << std::endl;
}

void
TcpHandoverCubic::ObserveAck(const TcpHeader& tcpHeader,
                             uint32_t segmentSize,
                             uint32_t cwndSegments)
{
    m_lastAckRangeValid = false;
    if (segmentSize == 0 || !tcpHeader.HasOption(TcpOption::SACK))
    {
        return;
    }

    auto sack = DynamicCast<const TcpOptionSack>(tcpHeader.GetOption(TcpOption::SACK));
    if (!sack)
    {
        return;
    }

    SequenceNumber32 ackNumber = tcpHeader.GetAckNumber();
    SequenceNumber32 fack = ackNumber;
    for (const auto& block : sack->GetSackList())
    {
        if (block.second > fack)
        {
            fack = block.second;
        }
    }

    m_lastAckRangeSegments = static_cast<uint32_t>(fack - ackNumber) / segmentSize;
    m_lastAckRangeValid = true;

    NS_LOG_INFO("ACK=" << ackNumber << " FACK=" << fack
                        << " ACK_range_segments=" << m_lastAckRangeSegments
                        << " cwnd_segments=" << cwndSegments);
}

uint32_t
TcpHandoverCubic::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    double sinceHandoverMs = -1.0;
    bool recentHandover = false;

    if (s_lastHandoverTimeNs >= 0)
    {
        const int64_t diffNs = nowNs - s_lastHandoverTimeNs;
        sinceHandoverMs = static_cast<double>(diffNs) / 1000000.0;
        recentHandover = diffNs >= 0 && diffNs <= m_detectionWindow.GetNanoSeconds();
    }

    const uint32_t cwndSegments = tcb->GetCwndInSegments();
    double ratio = 0.0;
    if (m_lastAckRangeValid && cwndSegments > 0)
    {
        ratio = static_cast<double>(m_lastAckRangeSegments) /
                static_cast<double>(cwndSegments);
    }

    const bool handoverReorderingCandidate =
        m_lastAckRangeValid && recentHandover && ratio >= m_ackRangeRatioThreshold;

    if (handoverReorderingCandidate)
    {
        m_handoverProtectionUntilNs = nowNs + m_protectionWindow.GetNanoSeconds();
    }

    const bool handoverProtectionActive =
        m_handoverProtectionUntilNs >= 0 && nowNs <= m_handoverProtectionUntilNs;

    NS_LOG_INFO("BeforeRecovery"
                << " ACK_range_segments=" << m_lastAckRangeSegments
                << " ACK_range_valid=" << m_lastAckRangeValid
                << " cwnd_segments=" << cwndSegments << " ratio=" << ratio);
    NS_LOG_INFO("TimeSinceHandover_ms=" << sinceHandoverMs
                                         << " recentHandover=" << recentHandover);

    if (handoverReorderingCandidate)
    {
        NS_LOG_INFO("Classification=HANDOVER_REORDERING");
    }
    else if (handoverProtectionActive)
    {
        NS_LOG_INFO("Classification=HANDOVER_PROTECTED_FOLLOWUP");
    }
    else
    {
        NS_LOG_INFO("Classification=NORMAL_LOSS");
    }

    const bool preserveCwnd = m_enableHandoverProtection && handoverProtectionActive;

    // 改進處：TcpHarpRecovery 只接受這一次、同一個 socket 的明確保護判定。
    s_lastDecisionTcb = PeekPointer(tcb);
    s_lastDecisionTimeNs = nowNs;
    s_lastDecisionPreserveCwnd = preserveCwnd;

    if (preserveCwnd)
    {
        NS_LOG_INFO("PreserveCwnd"
                    << " cwnd=" << tcb->m_cWnd << " ssthresh=" << tcb->m_cWnd);
        return tcb->m_cWnd;
    }

    return TcpCubic::GetSsThresh(tcb, bytesInFlight);
}

bool
TcpHandoverCubic::ConsumePreserveCwndDecision(Ptr<const TcpSocketState> tcb)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    const bool preserveCwnd = s_lastDecisionTcb == PeekPointer(tcb) &&
                              s_lastDecisionTimeNs == nowNs &&
                              s_lastDecisionPreserveCwnd;

    // 判定只能使用一次，避免之後的普通 loss 沿用舊狀態。
    s_lastDecisionTcb = nullptr;
    s_lastDecisionTimeNs = -1;
    s_lastDecisionPreserveCwnd = false;
    return preserveCwnd;
}

Ptr<TcpCongestionOps>
TcpHandoverCubic::Fork()
{
    return CopyObject<TcpHandoverCubic>(this);
}

} // namespace ns3
