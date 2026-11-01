/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-cubic-v4.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"

#include <algorithm>
#include <cstdlib>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpCubicV4");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpCubicV4);

int64_t TcpHarpCubicV4::s_lastDataHandoverTimeNs = -1;
int64_t TcpHarpCubicV4::s_lastAckHandoverTimeNs = -1;

TypeId
TcpHarpCubicV4::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHarpCubicV4")
            .SetParent<TcpCubic>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHarpCubicV4>()
            .AddAttribute("EnableAdaptiveBeta",
                          "Enable V3 adaptive beta and safe target using live route notification",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHarpCubicV4::m_enableAdaptiveBeta),
                          MakeBooleanChecker())
            .AddAttribute("HandoverWindow",
                          "Detection and protection interval measured from the live notification",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHarpCubicV4::m_handoverWindow),
                          MakeTimeChecker())
            .AddAttribute("DirectionMergeWindow",
                          "Merge near-simultaneous DATA and ACK notifications into DATA+ACK",
                          TimeValue(MilliSeconds(1)),
                          MakeTimeAccessor(&TcpHarpCubicV4::m_directionMergeWindow),
                          MakeTimeChecker())
            .AddAttribute("AckRangeRatioThreshold",
                          "Minimum ACK-range/cwnd ratio for a handover decision",
                          DoubleValue(0.015),
                          MakeDoubleAccessor(&TcpHarpCubicV4::m_ackRangeRatioThreshold),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("NormalBeta",
                          "CUBIC beta used outside an adaptive handover decision",
                          DoubleValue(0.70),
                          MakeDoubleAccessor(&TcpHarpCubicV4::m_normalBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AckHandoverBeta",
                          "Beta used for a detected ACK-path handover event",
                          DoubleValue(0.75),
                          MakeDoubleAccessor(&TcpHarpCubicV4::m_ackHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("DataHandoverBeta",
                          "Beta used for a detected DATA-path handover event before the flight cap",
                          DoubleValue(0.85),
                          MakeDoubleAccessor(&TcpHarpCubicV4::m_dataHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

TcpHarpCubicV4::TcpHarpCubicV4()
    : TcpCubic()
{
}

TcpHarpCubicV4::TcpHarpCubicV4(const TcpHarpCubicV4& other)
    : TcpCubic(other),
      m_enableAdaptiveBeta(other.m_enableAdaptiveBeta),
      m_handoverWindow(other.m_handoverWindow),
      m_directionMergeWindow(other.m_directionMergeWindow),
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
TcpHarpCubicV4::GetName() const
{
    return "TcpHarpCubicV4";
}

void
TcpHarpCubicV4::NotifyHandover(HandoverDirection direction)
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

    NS_LOG_INFO("HarpV4Notify"
                << " time_ns=" << nowNs
                << " direction="
                << (direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"));
}

void
TcpHarpCubicV4::ObserveAck(const TcpHeader& tcpHeader,
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
}

uint32_t
TcpHarpCubicV4::GetNormalTarget(const TcpSocketState& tcb) const
{
    const uint32_t normalSegments =
        std::max(static_cast<uint32_t>(tcb.GetCwndInSegments() * m_normalBeta), 2U);
    return normalSegments * tcb.m_segmentSize;
}

uint32_t
TcpHarpCubicV4::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
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

    int64_t handoverTimeNs = -1;
    HandoverDirection direction = HandoverDirection::ACK_PATH;
    const bool mergedDirections =
        s_lastDataHandoverTimeNs >= 0 &&
        s_lastAckHandoverTimeNs >= 0 &&
        std::abs(s_lastDataHandoverTimeNs - s_lastAckHandoverTimeNs) <=
            m_directionMergeWindow.GetNanoSeconds();

    if (mergedDirections)
    {
        // 同一次路由更新可能先被 DATA 封包、再被 ACK 封包觀察到。
        // 合併後以較晚時間確認雙向都已切換，但使用 DATA beta 保護資料傳輸。
        handoverTimeNs = std::max(s_lastDataHandoverTimeNs, s_lastAckHandoverTimeNs);
        direction = HandoverDirection::DATA_PATH;
    }
    else if (s_lastDataHandoverTimeNs >= s_lastAckHandoverTimeNs)
    {
        handoverTimeNs = s_lastDataHandoverTimeNs;
        direction = HandoverDirection::DATA_PATH;
    }
    else
    {
        handoverTimeNs = s_lastAckHandoverTimeNs;
    }

    const int64_t diffNs = handoverTimeNs >= 0 ? nowNs - handoverTimeNs : -1;
    const bool recentHandover = diffNs >= 0 && windowNs > 0 && diffNs <= windowNs;

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
        const double configuredHandoverBeta =
            direction == HandoverDirection::DATA_PATH ? m_dataHandoverBeta
                                                      : m_ackHandoverBeta;
        const double effectiveBeta =
            std::clamp(configuredHandoverBeta, m_normalBeta, 1.0);

        SetAttribute("Beta", DoubleValue(effectiveBeta));
        const uint32_t adaptiveTarget = TcpCubic::GetSsThresh(tcb, bytesInFlight);
        const uint32_t normalTarget = GetNormalTarget(*tcb);
        const uint32_t safeTarget =
            std::clamp(bytesInFlight, normalTarget, adaptiveTarget);

        m_activeHandoverTimeNs = handoverTimeNs;
        m_activeHandoverUntilNs = handoverTimeNs + windowNs;
        m_activeTargetCwnd = safeTarget;
        m_activeBeta = effectiveBeta;

        const double appliedBeta =
            currentCwnd > 0 ? static_cast<double>(safeTarget) / currentCwnd : 0.0;
        NS_LOG_INFO("HarpV4Decision"
                    << " classification=HANDOVER_SAFE"
                    << " direction="
                    << (mergedDirections
                            ? "DATA+ACK"
                            : (direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"))
                    << " time_since_handover_ms="
                    << static_cast<double>(diffNs) / 1000000.0
                    << " ratio=" << ratio << " configured_beta=" << effectiveBeta
                    << " applied_beta=" << appliedBeta
                    << " cwnd_segments=" << cwndSegments
                    << " bytes_in_flight_segments=" << bytesInFlight / tcb->m_segmentSize
                    << " normal_target_segments=" << normalTarget / tcb->m_segmentSize
                    << " adaptive_target_segments=" << adaptiveTarget / tcb->m_segmentSize
                    << " safe_target_segments=" << safeTarget / tcb->m_segmentSize);
        return safeTarget;
    }

    const bool sameHandoverFollowup =
        m_enableAdaptiveBeta && m_activeTargetCwnd > 0 &&
        nowNs <= m_activeHandoverUntilNs;
    if (sameHandoverFollowup)
    {
        const uint32_t targetCwnd = std::min(currentCwnd, m_activeTargetCwnd);
        NS_LOG_INFO("HarpV4Decision"
                    << " classification=HANDOVER_SAFE_FOLLOWUP"
                    << " configured_beta=" << m_activeBeta
                    << " cwnd_segments=" << cwndSegments
                    << " safe_target_segments=" << targetCwnd / tcb->m_segmentSize);
        return targetCwnd;
    }

    SetAttribute("Beta", DoubleValue(m_normalBeta));
    NS_LOG_INFO("HarpV4Decision"
                << " classification=NORMAL_LOSS"
                << " ratio=" << ratio << " effective_beta=" << m_normalBeta
                << " cwnd_segments=" << cwndSegments);
    return TcpCubic::GetSsThresh(tcb, bytesInFlight);
}

Ptr<TcpCongestionOps>
TcpHarpCubicV4::Fork()
{
    return CopyObject<TcpHarpCubicV4>(this);
}

} // namespace ns3
