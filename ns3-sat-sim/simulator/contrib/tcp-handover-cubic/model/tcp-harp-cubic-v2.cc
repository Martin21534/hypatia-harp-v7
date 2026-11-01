/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-cubic-v2.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"

#include <algorithm>
#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpCubicV2");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpCubicV2);

int64_t TcpHarpCubicV2::s_lastDataHandoverTimeNs = -1;
int64_t TcpHarpCubicV2::s_lastAckHandoverTimeNs = -1;

TypeId
TcpHarpCubicV2::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHarpCubicV2")
            .SetParent<TcpCubic>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHarpCubicV2>()
            .AddAttribute("EnableAdaptiveBeta",
                          "Enable direction-aware adaptive beta after a handover",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHarpCubicV2::m_enableAdaptiveBeta),
                          MakeBooleanChecker())
            .AddAttribute("HandoverWindow",
                          "Detection and protection interval measured from the handover time",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHarpCubicV2::m_handoverWindow),
                          MakeTimeChecker())
            .AddAttribute("AckRangeRatioThreshold",
                          "Minimum ACK-range/cwnd ratio for a handover decision",
                          DoubleValue(0.015),
                          MakeDoubleAccessor(&TcpHarpCubicV2::m_ackRangeRatioThreshold),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("NormalBeta",
                          "CUBIC beta used outside an adaptive handover decision",
                          DoubleValue(0.70),
                          MakeDoubleAccessor(&TcpHarpCubicV2::m_normalBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AckHandoverBeta",
                          "Beta used for a detected ACK-path handover event",
                          DoubleValue(0.75),
                          MakeDoubleAccessor(&TcpHarpCubicV2::m_ackHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("DataHandoverBeta",
                          "Beta used for a detected DATA-path handover event",
                          DoubleValue(0.85),
                          MakeDoubleAccessor(&TcpHarpCubicV2::m_dataHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

TcpHarpCubicV2::TcpHarpCubicV2()
    : TcpCubic()
{
}

TcpHarpCubicV2::TcpHarpCubicV2(const TcpHarpCubicV2& other)
    : TcpCubic(other),
      m_enableAdaptiveBeta(other.m_enableAdaptiveBeta),
      m_handoverWindow(other.m_handoverWindow),
      m_ackRangeRatioThreshold(other.m_ackRangeRatioThreshold),
      m_normalBeta(other.m_normalBeta),
      m_ackHandoverBeta(other.m_ackHandoverBeta),
      m_dataHandoverBeta(other.m_dataHandoverBeta),
      m_lastAckRangeSegments(other.m_lastAckRangeSegments),
      m_lastAckRangeValid(other.m_lastAckRangeValid),
      m_activeHandoverTimeNs(other.m_activeHandoverTimeNs),
      m_activeHandoverUntilNs(other.m_activeHandoverUntilNs),
      m_activeTargetCwnd(other.m_activeTargetCwnd),
      m_activeBeta(other.m_activeBeta)
{
}

std::string
TcpHarpCubicV2::GetName() const
{
    return "TcpHarpCubicV2";
}

void
TcpHarpCubicV2::NotifyHandover(HandoverDirection direction)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    if (direction == HandoverDirection::DATA_PATH)
    {
        s_lastDataHandoverTimeNs = nowNs;
    }
    else
    {
        s_lastAckHandoverTimeNs = nowNs;
    }

    std::cout << "TCP_HARP_V2_HANDOVER_NOTIFICATION"
              << " time_ns=" << nowNs
              << " direction="
              << (direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK") << std::endl;
}

void
TcpHarpCubicV2::ObserveAck(const TcpHeader& tcpHeader,
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

    const SequenceNumber32 ackNumber = tcpHeader.GetAckNumber();
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

    // 每個 SACK ACK 的明細只放在 debug，正式實驗的 info log 僅記錄 Recovery 決策。
    NS_LOG_DEBUG("HARP_V2_ACK=" << ackNumber << " FACK=" << fack
                                 << " ACK_range_segments=" << m_lastAckRangeSegments
                                 << " cwnd_segments=" << cwndSegments);
}

uint32_t
TcpHarpCubicV2::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    const int64_t windowNs = m_handoverWindow.GetNanoSeconds();
    const uint32_t currentCwnd = tcb->m_cWnd;
    const uint32_t cwndSegments = tcb->GetCwndInSegments();

    if (m_activeHandoverUntilNs >= 0 && nowNs > m_activeHandoverUntilNs)
    {
        m_activeHandoverTimeNs = -1;
        m_activeHandoverUntilNs = -1;
        m_activeTargetCwnd = 0;
        m_activeBeta = m_normalBeta;
    }

    // 同一時刻兩個方向都換手時，DATA path 對資料亂序的證據較強，因此優先採用。
    int64_t handoverTimeNs = -1;
    HandoverDirection direction = HandoverDirection::ACK_PATH;
    if (s_lastDataHandoverTimeNs >= s_lastAckHandoverTimeNs)
    {
        handoverTimeNs = s_lastDataHandoverTimeNs;
        direction = HandoverDirection::DATA_PATH;
    }
    else
    {
        handoverTimeNs = s_lastAckHandoverTimeNs;
    }

    const int64_t diffNs = handoverTimeNs >= 0 ? nowNs - handoverTimeNs : -1;
    const bool recentHandover =
        diffNs >= 0 && windowNs > 0 && diffNs <= windowNs;

    double ratio = 0.0;
    if (m_lastAckRangeValid && cwndSegments > 0)
    {
        ratio = static_cast<double>(m_lastAckRangeSegments) /
                static_cast<double>(cwndSegments);
    }

    const bool candidate = m_enableAdaptiveBeta && m_lastAckRangeValid && recentHandover &&
                           ratio >= m_ackRangeRatioThreshold;
    const bool newHandoverDecision =
        candidate && handoverTimeNs != m_activeHandoverTimeNs;

    if (newHandoverDecision)
    {
        // V2 第一版採用容易驗證的三級 beta，不假設 ratio 越大就一定越像亂序。
        const double configuredHandoverBeta =
            direction == HandoverDirection::DATA_PATH ? m_dataHandoverBeta
                                                      : m_ackHandoverBeta;
        const double effectiveBeta =
            std::clamp(configuredHandoverBeta, m_normalBeta, 1.0);

        // 使用繼承自 TcpCubic 的 Beta attribute，讓 ssthresh、Wmax 與下一個
        // CUBIC epoch 使用同一個 effective beta，不必修改原始 TcpCubic。
        SetAttribute("Beta", DoubleValue(effectiveBeta));
        const uint32_t targetCwnd = TcpCubic::GetSsThresh(tcb, bytesInFlight);

        m_activeHandoverTimeNs = handoverTimeNs;
        // 保護截止時間從實際 handover 計算，不是從辨識成功時間計算。
        m_activeHandoverUntilNs = handoverTimeNs + windowNs;
        m_activeTargetCwnd = targetCwnd;
        m_activeBeta = effectiveBeta;

        NS_LOG_INFO("HarpV2Decision"
                    << " classification=HANDOVER_ADAPTIVE"
                    << " direction="
                    << (direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK")
                    << " time_since_handover_ms="
                    << static_cast<double>(diffNs) / 1000000.0
                    << " ratio=" << ratio << " effective_beta=" << effectiveBeta
                    << " cwnd_segments=" << cwndSegments
                    << " target_segments=" << targetCwnd / tcb->m_segmentSize);
        return targetCwnd;
    }

    const bool sameHandoverFollowup =
        m_enableAdaptiveBeta && m_activeTargetCwnd > 0 &&
        nowNs <= m_activeHandoverUntilNs;
    if (sameHandoverFollowup)
    {
        // 不提高已經降得更低的 cwnd，也不對同一次換手再次乘 beta。
        const uint32_t targetCwnd = std::min(currentCwnd, m_activeTargetCwnd);
        NS_LOG_INFO("HarpV2Decision"
                    << " classification=HANDOVER_FOLLOWUP"
                    << " effective_beta=" << m_activeBeta
                    << " cwnd_segments=" << cwndSegments
                    << " target_segments=" << targetCwnd / tcb->m_segmentSize);
        return targetCwnd;
    }

    SetAttribute("Beta", DoubleValue(m_normalBeta));
    NS_LOG_INFO("HarpV2Decision"
                << " classification=NORMAL_LOSS"
                << " ratio=" << ratio << " effective_beta=" << m_normalBeta
                << " cwnd_segments=" << cwndSegments);
    return TcpCubic::GetSsThresh(tcb, bytesInFlight);
}

Ptr<TcpCongestionOps>
TcpHarpCubicV2::Fork()
{
    return CopyObject<TcpHarpCubicV2>(this);
}

} // namespace ns3
