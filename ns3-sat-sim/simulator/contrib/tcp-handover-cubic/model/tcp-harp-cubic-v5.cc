/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-cubic-v5.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-harp.h"
#include "ns3/tcp-option-sack.h"

#include <algorithm>
#include <cstdlib>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpCubicV5");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpCubicV5);

TypeId
TcpHarpCubicV5::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHarpCubicV5")
            .SetParent<TcpCubic>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHarpCubicV5>()
            .AddAttribute("EnableAdaptiveBeta",
                          "Enable adaptive beta using packet-carried HARP signals",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHarpCubicV5::m_enableAdaptiveBeta),
                          MakeBooleanChecker())
            .AddAttribute("HandoverWindow",
                          "Protection interval measured from first receipt of a new signal epoch",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHarpCubicV5::m_handoverWindow),
                          MakeTimeChecker())
            .AddAttribute("DirectionMergeWindow",
                          "Merge near-simultaneous DATA and ACK packet signals",
                          TimeValue(MilliSeconds(1)),
                          MakeTimeAccessor(&TcpHarpCubicV5::m_directionMergeWindow),
                          MakeTimeChecker())
            .AddAttribute("AckRangeRatioThreshold",
                          "Minimum ACK-range/cwnd ratio for a handover decision",
                          DoubleValue(0.015),
                          MakeDoubleAccessor(&TcpHarpCubicV5::m_ackRangeRatioThreshold),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("NormalBeta",
                          "CUBIC beta used outside a signaled handover decision",
                          DoubleValue(0.70),
                          MakeDoubleAccessor(&TcpHarpCubicV5::m_normalBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("AckHandoverBeta",
                          "Beta used for an ACK-path packet signal",
                          DoubleValue(0.75),
                          MakeDoubleAccessor(&TcpHarpCubicV5::m_ackHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("DataHandoverBeta",
                          "Beta used for a DATA-path echo before the flight cap",
                          DoubleValue(0.85),
                          MakeDoubleAccessor(&TcpHarpCubicV5::m_dataHandoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

TcpHarpCubicV5::TcpHarpCubicV5()
    : TcpCubic()
{
}

TcpHarpCubicV5::TcpHarpCubicV5(const TcpHarpCubicV5& other)
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
      m_lastAckObservationTimeNs(other.m_lastAckObservationTimeNs),
      m_lastDataSignalValid(other.m_lastDataSignalValid),
      m_lastAckSignalValid(other.m_lastAckSignalValid),
      m_lastDataEpoch(other.m_lastDataEpoch),
      m_lastAckEpoch(other.m_lastAckEpoch),
      m_lastDataSignalTimeNs(other.m_lastDataSignalTimeNs),
      m_lastAckSignalTimeNs(other.m_lastAckSignalTimeNs),
      m_activeSignalValid(other.m_activeSignalValid),
      m_activeDirection(other.m_activeDirection),
      m_activeEpoch(other.m_activeEpoch),
      m_activeUntilNs(other.m_activeUntilNs),
      m_activeTargetCwnd(other.m_activeTargetCwnd),
      m_activeBeta(other.m_activeBeta),
      m_pendingRecoveryValid(other.m_pendingRecoveryValid),
      m_pendingRecoveryTimeNs(other.m_pendingRecoveryTimeNs),
      m_pendingRecoveryUntilNs(other.m_pendingRecoveryUntilNs),
      m_pendingCwnd(other.m_pendingCwnd),
      m_pendingBytesInFlight(other.m_pendingBytesInFlight),
      m_pendingNormalTarget(other.m_pendingNormalTarget),
      m_pendingSegmentSize(other.m_pendingSegmentSize),
      m_pendingRatio(other.m_pendingRatio),
      m_lateUpdateReady(other.m_lateUpdateReady),
      m_lateUpdateDirection(other.m_lateUpdateDirection),
      m_lateUpdateEpoch(other.m_lateUpdateEpoch),
      m_lateSignalTimeNs(other.m_lateSignalTimeNs)
{
}

std::string
TcpHarpCubicV5::GetName() const
{
    return "TcpHarpCubicV5";
}

void
TcpHarpCubicV5::ObserveAck(const TcpHeader& tcpHeader,
                           uint32_t segmentSize,
                           uint32_t cwndSegments)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    ExpireTimedState(nowNs);

    bool receivedNewDataSignal = false;
    bool receivedNewAckSignal = false;
    for (const auto& option : tcpHeader.GetOptionList())
    {
        if (option->GetKind() != TcpOption::HARP)
        {
            continue;
        }

        const auto harp = DynamicCast<const TcpOptionHarp>(option);
        if (!harp)
        {
            continue;
        }

        if (harp->GetSignal() == TcpOptionHarp::Signal::DATA_ECHO)
        {
            const bool newEpoch = !m_lastDataSignalValid || harp->GetEpoch() != m_lastDataEpoch;
            if (newEpoch)
            {
                m_lastDataSignalValid = true;
                m_lastDataEpoch = harp->GetEpoch();
                m_lastDataSignalTimeNs = nowNs;
                receivedNewDataSignal = true;
            }
            NS_LOG_INFO("HarpV5Signal"
                        << " event=ECHO_RX"
                        << " time_ns=" << nowNs
                        << " direction=DATA"
                        << " epoch=" << harp->GetEpoch()
                        << " new_epoch=" << (newEpoch ? 1 : 0));
        }
        else if (harp->GetSignal() == TcpOptionHarp::Signal::ACK_MARK)
        {
            const bool newEpoch = !m_lastAckSignalValid || harp->GetEpoch() != m_lastAckEpoch;
            if (newEpoch)
            {
                m_lastAckSignalValid = true;
                m_lastAckEpoch = harp->GetEpoch();
                m_lastAckSignalTimeNs = nowNs;
                receivedNewAckSignal = true;
            }
            NS_LOG_INFO("HarpV5Signal"
                        << " event=ACK_MARK_RX"
                        << " time_ns=" << nowNs
                        << " direction=ACK"
                        << " epoch=" << harp->GetEpoch()
                        << " new_epoch=" << (newEpoch ? 1 : 0));
        }
    }

    m_lastAckObservationTimeNs = nowNs;
    m_lastAckRangeValid = false;
    if (segmentSize > 0 && tcpHeader.HasOption(TcpOption::SACK))
    {
        const auto sack = DynamicCast<const TcpOptionSack>(tcpHeader.GetOption(TcpOption::SACK));
        if (sack)
        {
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
    }

    // A signal can arrive after an earlier high-ratio ACK already started
    // Recovery.  Arm a one-shot target revision; TcpSocketBase will consume it
    // only while that same Recovery is still active.
    if (m_enableAdaptiveBeta && m_pendingRecoveryValid &&
        (receivedNewDataSignal || receivedNewAckSignal) &&
        nowNs >= m_pendingRecoveryTimeNs && nowNs <= m_pendingRecoveryUntilNs)
    {
        const bool merged = receivedNewDataSignal && receivedNewAckSignal;
        if (merged || receivedNewDataSignal)
        {
            m_lateUpdateDirection = HandoverDirection::DATA_PATH;
            m_lateUpdateEpoch = m_lastDataEpoch;
            m_lateSignalTimeNs = m_lastDataSignalTimeNs;
        }
        else
        {
            m_lateUpdateDirection = HandoverDirection::ACK_PATH;
            m_lateUpdateEpoch = m_lastAckEpoch;
            m_lateSignalTimeNs = m_lastAckSignalTimeNs;
        }
        m_lateUpdateReady = true;

        NS_LOG_INFO("HarpV5Decision"
                    << " classification=LATE_SIGNAL_READY"
                    << " direction="
                    << (merged ? "DATA+ACK"
                               : (m_lateUpdateDirection == HandoverDirection::DATA_PATH ? "DATA"
                                                                                         : "ACK"))
                    << " epoch=" << m_lateUpdateEpoch
                    << " time_since_recovery_ms="
                    << static_cast<double>(nowNs - m_pendingRecoveryTimeNs) / 1000000.0
                    << " ratio_at_recovery=" << m_pendingRatio);
    }
}

uint32_t
TcpHarpCubicV5::GetNormalTarget(const TcpSocketState& tcb) const
{
    const uint32_t normalSegments =
        std::max(static_cast<uint32_t>(tcb.GetCwndInSegments() * m_normalBeta), 2U);
    return normalSegments * tcb.m_segmentSize;
}

uint32_t
TcpHarpCubicV5::GetSnapshotTarget(double beta) const
{
    if (m_pendingSegmentSize == 0)
    {
        return 0;
    }

    const uint32_t cwndSegments = m_pendingCwnd / m_pendingSegmentSize;
    const uint32_t targetSegments =
        std::max(static_cast<uint32_t>(cwndSegments * beta), 2U);
    return targetSegments * m_pendingSegmentSize;
}

void
TcpHarpCubicV5::ClearPendingRecovery()
{
    m_pendingRecoveryValid = false;
    m_pendingRecoveryTimeNs = -1;
    m_pendingRecoveryUntilNs = -1;
    m_pendingCwnd = 0;
    m_pendingBytesInFlight = 0;
    m_pendingNormalTarget = 0;
    m_pendingSegmentSize = 0;
    m_pendingRatio = 0.0;
    m_lateUpdateReady = false;
    m_lateSignalTimeNs = -1;
}

void
TcpHarpCubicV5::ExpireTimedState(int64_t nowNs)
{
    if (m_activeSignalValid && nowNs > m_activeUntilNs)
    {
        NS_LOG_INFO("HarpV5Decision"
                    << " classification=PROTECTION_EXPIRED"
                    << " time_ns=" << nowNs
                    << " epoch=" << m_activeEpoch);
        m_activeSignalValid = false;
        m_activeUntilNs = -1;
        m_activeTargetCwnd = 0;
        m_activeBeta = m_normalBeta;
        SetAttribute("Beta", DoubleValue(m_normalBeta));
    }

    if (m_pendingRecoveryValid && nowNs > m_pendingRecoveryUntilNs)
    {
        NS_LOG_INFO("HarpV5Decision"
                    << " classification=LATE_SIGNAL_EXPIRED"
                    << " time_ns=" << nowNs
                    << " recovery_time_ns=" << m_pendingRecoveryTimeNs
                    << " ratio_at_recovery=" << m_pendingRatio);
        ClearPendingRecovery();
    }
}

bool
TcpHarpCubicV5::GetRecoveryTargetUpdate(Ptr<const TcpSocketState> tcb, uint32_t& target)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    ExpireTimedState(nowNs);

    if (!m_lateUpdateReady || !m_pendingRecoveryValid)
    {
        return false;
    }

    if (tcb->m_congState != TcpSocketState::CA_RECOVERY)
    {
        NS_LOG_INFO("HarpV5Decision"
                    << " classification=LATE_SIGNAL_AFTER_RECOVERY"
                    << " time_ns=" << nowNs
                    << " epoch=" << m_lateUpdateEpoch
                    << " ratio_at_recovery=" << m_pendingRatio);
        ClearPendingRecovery();
        return false;
    }

    const double configuredBeta = m_lateUpdateDirection == HandoverDirection::DATA_PATH
                                      ? m_dataHandoverBeta
                                      : m_ackHandoverBeta;
    const double effectiveBeta = std::clamp(configuredBeta, m_normalBeta, 1.0);
    const uint32_t adaptiveTarget = GetSnapshotTarget(effectiveBeta);
    const uint32_t safeTarget =
        std::clamp(m_pendingBytesInFlight, m_pendingNormalTarget, adaptiveTarget);

    target = safeTarget;
    m_activeSignalValid = true;
    m_activeDirection = m_lateUpdateDirection;
    m_activeEpoch = m_lateUpdateEpoch;
    m_activeUntilNs = m_lateSignalTimeNs + m_handoverWindow.GetNanoSeconds();
    m_activeTargetCwnd = safeTarget;
    m_activeBeta = effectiveBeta;

    NS_LOG_INFO("HarpV5Decision"
                << " classification=LATE_SIGNAL_SAFE_UPDATE"
                << " direction="
                << (m_lateUpdateDirection == HandoverDirection::DATA_PATH ? "DATA" : "ACK")
                << " epoch=" << m_lateUpdateEpoch
                << " time_since_recovery_ms="
                << static_cast<double>(nowNs - m_pendingRecoveryTimeNs) / 1000000.0
                << " ratio_at_recovery=" << m_pendingRatio
                << " cwnd_before_segments=" << m_pendingCwnd / m_pendingSegmentSize
                << " bytes_in_flight_at_recovery_segments="
                << m_pendingBytesInFlight / m_pendingSegmentSize
                << " normal_target_segments=" << m_pendingNormalTarget / m_pendingSegmentSize
                << " adaptive_target_segments=" << adaptiveTarget / m_pendingSegmentSize
                << " safe_target_segments=" << safeTarget / m_pendingSegmentSize);

    ClearPendingRecovery();
    return true;
}

uint32_t
TcpHarpCubicV5::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    const int64_t windowNs = m_handoverWindow.GetNanoSeconds();
    const uint32_t currentCwnd = tcb->m_cWnd;
    const uint32_t cwndSegments = tcb->GetCwndInSegments();
    const bool ackObservedNow = m_lastAckObservationTimeNs == nowNs;

    ExpireTimedState(nowNs);

    const bool mergedDirections =
        m_lastDataSignalValid && m_lastAckSignalValid &&
        std::abs(m_lastDataSignalTimeNs - m_lastAckSignalTimeNs) <=
            m_directionMergeWindow.GetNanoSeconds();

    int64_t signalTimeNs = -1;
    uint16_t signalEpoch = 0;
    HandoverDirection direction = HandoverDirection::ACK_PATH;
    if (mergedDirections ||
        (m_lastDataSignalValid &&
         (!m_lastAckSignalValid || m_lastDataSignalTimeNs >= m_lastAckSignalTimeNs)))
    {
        signalTimeNs = m_lastDataSignalTimeNs;
        signalEpoch = m_lastDataEpoch;
        direction = HandoverDirection::DATA_PATH;
    }
    else if (m_lastAckSignalValid)
    {
        signalTimeNs = m_lastAckSignalTimeNs;
        signalEpoch = m_lastAckEpoch;
    }

    const int64_t diffNs = signalTimeNs >= 0 ? nowNs - signalTimeNs : -1;
    const bool recentSignal = diffNs >= 0 && windowNs > 0 && diffNs <= windowNs;

    double ratio = 0.0;
    if (m_lastAckRangeValid && cwndSegments > 0)
    {
        ratio = static_cast<double>(m_lastAckRangeSegments) /
                static_cast<double>(cwndSegments);
    }

    const bool candidate = m_enableAdaptiveBeta && ackObservedNow && m_lastAckRangeValid &&
                           recentSignal && ratio >= m_ackRangeRatioThreshold;
    const bool newSignalDecision =
        candidate &&
        (!m_activeSignalValid || signalEpoch != m_activeEpoch || direction != m_activeDirection);

    if (newSignalDecision)
    {
        const double configuredBeta = direction == HandoverDirection::DATA_PATH
                                          ? m_dataHandoverBeta
                                          : m_ackHandoverBeta;
        const double effectiveBeta = std::clamp(configuredBeta, m_normalBeta, 1.0);

        SetAttribute("Beta", DoubleValue(effectiveBeta));
        const uint32_t adaptiveTarget = TcpCubic::GetSsThresh(tcb, bytesInFlight);
        // Beta selects this Recovery target only.  Leaving it at the adaptive
        // value would also change CUBIC's later friendliness/growth behavior.
        SetAttribute("Beta", DoubleValue(m_normalBeta));
        const uint32_t normalTarget = GetNormalTarget(*tcb);
        const uint32_t safeTarget = std::clamp(bytesInFlight, normalTarget, adaptiveTarget);

        ClearPendingRecovery();
        m_activeSignalValid = true;
        m_activeDirection = direction;
        m_activeEpoch = signalEpoch;
        m_activeUntilNs = signalTimeNs + windowNs;
        m_activeTargetCwnd = safeTarget;
        m_activeBeta = effectiveBeta;

        const double appliedBeta =
            currentCwnd > 0 ? static_cast<double>(safeTarget) / currentCwnd : 0.0;
        NS_LOG_INFO("HarpV5Decision"
                    << " classification=HANDOVER_SIGNAL_SAFE"
                    << " direction="
                    << (mergedDirections
                            ? "DATA+ACK"
                            : (direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"))
                    << " epoch=" << signalEpoch
                    << " time_since_signal_ms=" << static_cast<double>(diffNs) / 1000000.0
                    << " ratio=" << ratio
                    << " configured_beta=" << effectiveBeta
                    << " applied_beta=" << appliedBeta
                    << " cwnd_segments=" << cwndSegments
                    << " bytes_in_flight_segments=" << bytesInFlight / tcb->m_segmentSize
                    << " normal_target_segments=" << normalTarget / tcb->m_segmentSize
                    << " adaptive_target_segments=" << adaptiveTarget / tcb->m_segmentSize
                    << " safe_target_segments=" << safeTarget / tcb->m_segmentSize);
        return safeTarget;
    }

    const bool sameSignalFollowup =
        m_enableAdaptiveBeta && ackObservedNow && m_activeSignalValid &&
        m_activeTargetCwnd > 0 &&
        nowNs <= m_activeUntilNs;
    if (sameSignalFollowup)
    {
        const uint32_t targetCwnd = std::min(currentCwnd, m_activeTargetCwnd);
        NS_LOG_INFO("HarpV5Decision"
                    << " classification=HANDOVER_SIGNAL_SAFE_FOLLOWUP"
                    << " epoch=" << m_activeEpoch
                    << " configured_beta=" << m_activeBeta
                    << " cwnd_segments=" << cwndSegments
                    << " safe_target_segments=" << targetCwnd / tcb->m_segmentSize);
        return targetCwnd;
    }

    SetAttribute("Beta", DoubleValue(m_normalBeta));
    const uint32_t normalTarget = TcpCubic::GetSsThresh(tcb, bytesInFlight);

    bool waitingForLateSignal = false;
    if (ackObservedNow)
    {
        // A newer Recovery supersedes any older unconfirmed snapshot.
        ClearPendingRecovery();
        if (m_enableAdaptiveBeta && windowNs > 0 && m_lastAckRangeValid && !recentSignal &&
            ratio >= m_ackRangeRatioThreshold)
        {
            m_pendingRecoveryValid = true;
            m_pendingRecoveryTimeNs = nowNs;
            m_pendingRecoveryUntilNs = nowNs + windowNs;
            m_pendingCwnd = currentCwnd;
            m_pendingBytesInFlight = bytesInFlight;
            m_pendingNormalTarget = normalTarget;
            m_pendingSegmentSize = tcb->m_segmentSize;
            m_pendingRatio = ratio;
            waitingForLateSignal = true;

            NS_LOG_INFO("HarpV5Decision"
                        << " classification=LATE_SIGNAL_WAIT"
                        << " recovery_time_ns=" << nowNs
                        << " expires_ns=" << m_pendingRecoveryUntilNs
                        << " ratio_at_recovery=" << ratio
                        << " cwnd_before_segments=" << cwndSegments
                        << " bytes_in_flight_at_recovery_segments="
                        << bytesInFlight / tcb->m_segmentSize
                        << " normal_target_segments=" << normalTarget / tcb->m_segmentSize);
        }
    }

    NS_LOG_INFO("HarpV5Decision"
                << " classification=NORMAL_LOSS"
                << " signal_available=" << (recentSignal ? 1 : 0)
                << " late_signal_wait=" << (waitingForLateSignal ? 1 : 0)
                << " ratio=" << ratio
                << " effective_beta=" << m_normalBeta
                << " cwnd_segments=" << cwndSegments);
    return normalTarget;
}

Ptr<TcpCongestionOps>
TcpHarpCubicV5::Fork()
{
    return CopyObject<TcpHarpCubicV5>(this);
}

} // namespace ns3
