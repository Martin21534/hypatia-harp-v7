/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-cubic-v6.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-harp.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpCubicV6");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpCubicV6);

TypeId
TcpHarpCubicV6::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHarpCubicV6")
            .SetParent<TcpCubic>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHarpCubicV6>()
            .AddAttribute("EnableAdaptiveBeta",
                          "Enable conservative adaptive beta using packet-carried HARP signals",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHarpCubicV6::m_enableAdaptiveBeta),
                          MakeBooleanChecker())
            .AddAttribute("HandoverWindow",
                          "Signal lifetime and delayed normal-sample confirmation interval",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&TcpHarpCubicV6::m_handoverWindow),
                          MakeTimeChecker())
            .AddAttribute("DirectionMergeWindow",
                          "Merge near-simultaneous DATA and ACK packet signals for logging",
                          TimeValue(MilliSeconds(1)),
                          MakeTimeAccessor(&TcpHarpCubicV6::m_directionMergeWindow),
                          MakeTimeChecker())
            .AddAttribute("NormalBeta",
                          "CUBIC beta used outside a qualified handover Recovery",
                          DoubleValue(0.70),
                          MakeDoubleAccessor(&TcpHarpCubicV6::m_normalBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("HandoverBeta",
                          "Common DATA/ACK beta used after both signal and gap gates pass",
                          DoubleValue(0.85),
                          MakeDoubleAccessor(&TcpHarpCubicV6::m_handoverBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("GapThresholdFloor",
                          "Cold-start and minimum ACK-range gap threshold in segments",
                          UintegerValue(7),
                          MakeUintegerAccessor(&TcpHarpCubicV6::m_gapThresholdFloor),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("GapMargin",
                          "Minimum number of segments added above the learned median",
                          UintegerValue(3),
                          MakeUintegerAccessor(&TcpHarpCubicV6::m_gapMargin),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("GapMadMultiplier",
                          "Multiplier applied to the median absolute deviation",
                          DoubleValue(3.0),
                          MakeDoubleAccessor(&TcpHarpCubicV6::m_gapMadMultiplier),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("GapBaselineWindow",
                          "Number of confirmed normal Recovery gaps retained per flow",
                          UintegerValue(9),
                          MakeUintegerAccessor(&TcpHarpCubicV6::m_gapBaselineWindow),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("GapBaselineMinSamples",
                          "Confirmed normal samples required before adapting the threshold",
                          UintegerValue(3),
                          MakeUintegerAccessor(&TcpHarpCubicV6::m_gapBaselineMinSamples),
                          MakeUintegerChecker<uint32_t>(1));
    return tid;
}

TcpHarpCubicV6::TcpHarpCubicV6()
    : TcpCubic()
{
}

TcpHarpCubicV6::TcpHarpCubicV6(const TcpHarpCubicV6& other)
    : TcpCubic(other),
      m_enableAdaptiveBeta(other.m_enableAdaptiveBeta),
      m_handoverWindow(other.m_handoverWindow),
      m_directionMergeWindow(other.m_directionMergeWindow),
      m_normalBeta(other.m_normalBeta),
      m_handoverBeta(other.m_handoverBeta),
      m_gapThresholdFloor(other.m_gapThresholdFloor),
      m_gapMargin(other.m_gapMargin),
      m_gapMadMultiplier(other.m_gapMadMultiplier),
      m_gapBaselineWindow(other.m_gapBaselineWindow),
      m_gapBaselineMinSamples(other.m_gapBaselineMinSamples),
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
      m_confirmedNormalGaps(other.m_confirmedNormalGaps),
      m_pendingGapSamples(other.m_pendingGapSamples),
      m_pendingRecoveryValid(other.m_pendingRecoveryValid),
      m_pendingRecoveryTimeNs(other.m_pendingRecoveryTimeNs),
      m_pendingRecoveryUntilNs(other.m_pendingRecoveryUntilNs),
      m_pendingCwnd(other.m_pendingCwnd),
      m_pendingBytesInFlight(other.m_pendingBytesInFlight),
      m_pendingNormalTarget(other.m_pendingNormalTarget),
      m_pendingSegmentSize(other.m_pendingSegmentSize),
      m_pendingGapSegments(other.m_pendingGapSegments),
      m_pendingGapThreshold(other.m_pendingGapThreshold),
      m_pendingRatio(other.m_pendingRatio),
      m_lateUpdateReady(other.m_lateUpdateReady),
      m_lateSignal(other.m_lateSignal)
{
}

std::string
TcpHarpCubicV6::GetName() const
{
    return "TcpHarpCubicV6";
}

double
TcpHarpCubicV6::Median(std::vector<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1)
    {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2.0;
}

uint32_t
TcpHarpCubicV6::GetDynamicGapThreshold(double* median, double* mad) const
{
    double localMedian = 0.0;
    double localMad = 0.0;

    if (!m_confirmedNormalGaps.empty())
    {
        std::vector<double> gaps;
        gaps.reserve(m_confirmedNormalGaps.size());
        for (const uint32_t gap : m_confirmedNormalGaps)
        {
            gaps.push_back(static_cast<double>(gap));
        }
        localMedian = Median(gaps);

        std::vector<double> deviations;
        deviations.reserve(gaps.size());
        for (const double gap : gaps)
        {
            deviations.push_back(std::abs(gap - localMedian));
        }
        localMad = Median(deviations);
    }

    if (median)
    {
        *median = localMedian;
    }
    if (mad)
    {
        *mad = localMad;
    }

    if (m_confirmedNormalGaps.size() < m_gapBaselineMinSamples)
    {
        return m_gapThresholdFloor;
    }

    const double adaptiveMargin =
        std::max(static_cast<double>(m_gapMargin), m_gapMadMultiplier * localMad);
    const double rawThreshold = std::ceil(localMedian + adaptiveMargin);
    const double boundedThreshold =
        std::min(rawThreshold, static_cast<double>(std::numeric_limits<uint32_t>::max()));
    return std::max(m_gapThresholdFloor, static_cast<uint32_t>(boundedThreshold));
}

TcpHarpCubicV6::SignalSelection
TcpHarpCubicV6::SelectRecentSignal(int64_t nowNs) const
{
    SignalSelection selected;
    const int64_t windowNs = m_handoverWindow.GetNanoSeconds();
    if (windowNs <= 0)
    {
        return selected;
    }

    const bool dataRecent =
        m_lastDataSignalValid && nowNs >= m_lastDataSignalTimeNs &&
        nowNs - m_lastDataSignalTimeNs <= windowNs;
    const bool ackRecent =
        m_lastAckSignalValid && nowNs >= m_lastAckSignalTimeNs &&
        nowNs - m_lastAckSignalTimeNs <= windowNs;

    if (dataRecent && ackRecent &&
        std::abs(m_lastDataSignalTimeNs - m_lastAckSignalTimeNs) <=
            m_directionMergeWindow.GetNanoSeconds())
    {
        selected.valid = true;
        selected.merged = true;
        selected.direction = HandoverDirection::DATA_PATH;
        selected.epoch = m_lastDataEpoch;
        selected.timeNs = std::max(m_lastDataSignalTimeNs, m_lastAckSignalTimeNs);
    }
    else if (dataRecent && (!ackRecent || m_lastDataSignalTimeNs >= m_lastAckSignalTimeNs))
    {
        selected.valid = true;
        selected.direction = HandoverDirection::DATA_PATH;
        selected.epoch = m_lastDataEpoch;
        selected.timeNs = m_lastDataSignalTimeNs;
    }
    else if (ackRecent)
    {
        selected.valid = true;
        selected.direction = HandoverDirection::ACK_PATH;
        selected.epoch = m_lastAckEpoch;
        selected.timeNs = m_lastAckSignalTimeNs;
    }

    return selected;
}

void
TcpHarpCubicV6::AddConfirmedNormalGap(uint32_t gapSegments)
{
    m_confirmedNormalGaps.push_back(gapSegments);
    while (m_confirmedNormalGaps.size() > m_gapBaselineWindow)
    {
        m_confirmedNormalGaps.pop_front();
    }

    double median = 0.0;
    double mad = 0.0;
    const uint32_t threshold = GetDynamicGapThreshold(&median, &mad);
    NS_LOG_INFO("HarpV6Baseline"
                << " event=CONFIRMED_NORMAL_SAMPLE"
                << " gap_segments=" << gapSegments
                << " sample_count=" << m_confirmedNormalGaps.size()
                << " median=" << median
                << " mad=" << mad
                << " gap_threshold_segments=" << threshold);
}

void
TcpHarpCubicV6::QueuePotentialNormalGap(int64_t nowNs, uint32_t gapSegments)
{
    m_pendingGapSamples.push_back(
        {nowNs, nowNs + m_handoverWindow.GetNanoSeconds(), gapSegments});
}

void
TcpHarpCubicV6::DiscardUnconfirmedGapsForSignal(int64_t signalTimeNs)
{
    auto sample = m_pendingGapSamples.begin();
    while (sample != m_pendingGapSamples.end())
    {
        if (signalTimeNs >= sample->recoveryTimeNs && signalTimeNs <= sample->confirmAfterNs)
        {
            NS_LOG_INFO("HarpV6Baseline"
                        << " event=DISCARDED_BY_LATE_SIGNAL"
                        << " recovery_time_ns=" << sample->recoveryTimeNs
                        << " signal_time_ns=" << signalTimeNs
                        << " gap_segments=" << sample->gapSegments);
            sample = m_pendingGapSamples.erase(sample);
        }
        else
        {
            ++sample;
        }
    }
}

void
TcpHarpCubicV6::ClearPendingRecovery()
{
    m_pendingRecoveryValid = false;
    m_pendingRecoveryTimeNs = -1;
    m_pendingRecoveryUntilNs = -1;
    m_pendingCwnd = 0;
    m_pendingBytesInFlight = 0;
    m_pendingNormalTarget = 0;
    m_pendingSegmentSize = 0;
    m_pendingGapSegments = 0;
    m_pendingGapThreshold = 0;
    m_pendingRatio = 0.0;
    m_lateUpdateReady = false;
    m_lateSignal = {};
}

void
TcpHarpCubicV6::ExpireTimedState(int64_t nowNs)
{
    if (m_activeSignalValid && nowNs > m_activeUntilNs)
    {
        NS_LOG_INFO("HarpV6Decision"
                    << " classification=PROTECTION_EXPIRED"
                    << " time_ns=" << nowNs
                    << " epoch=" << m_activeEpoch);
        m_activeSignalValid = false;
        m_activeTargetCwnd = 0;
    }

    while (!m_pendingGapSamples.empty() &&
           nowNs > m_pendingGapSamples.front().confirmAfterNs)
    {
        AddConfirmedNormalGap(m_pendingGapSamples.front().gapSegments);
        m_pendingGapSamples.pop_front();
    }

    if (m_pendingRecoveryValid && nowNs > m_pendingRecoveryUntilNs)
    {
        NS_LOG_INFO("HarpV6Decision"
                    << " classification=LATE_SIGNAL_EXPIRED"
                    << " time_ns=" << nowNs
                    << " recovery_time_ns=" << m_pendingRecoveryTimeNs
                    << " gap_segments=" << m_pendingGapSegments
                    << " gap_threshold_segments=" << m_pendingGapThreshold);
        ClearPendingRecovery();
    }
}

void
TcpHarpCubicV6::ObserveAck(const TcpHeader& tcpHeader,
                           uint32_t segmentSize,
                           uint32_t cwndSegments [[maybe_unused]])
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
            NS_LOG_INFO("HarpV6Signal"
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
            NS_LOG_INFO("HarpV6Signal"
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

    if (receivedNewDataSignal || receivedNewAckSignal)
    {
        const SignalSelection signal = SelectRecentSignal(nowNs);
        DiscardUnconfirmedGapsForSignal(nowNs);

        if (m_enableAdaptiveBeta && signal.valid && m_pendingRecoveryValid &&
            nowNs >= m_pendingRecoveryTimeNs && nowNs <= m_pendingRecoveryUntilNs)
        {
            m_lateSignal = signal;
            m_lateUpdateReady = true;
            NS_LOG_INFO("HarpV6Decision"
                        << " classification=LATE_SIGNAL_READY"
                        << " direction="
                        << (signal.merged
                                ? "DATA+ACK"
                                : (signal.direction == HandoverDirection::DATA_PATH ? "DATA"
                                                                                    : "ACK"))
                        << " epoch=" << signal.epoch
                        << " time_since_recovery_ms="
                        << static_cast<double>(nowNs - m_pendingRecoveryTimeNs) / 1000000.0
                        << " gap_segments=" << m_pendingGapSegments
                        << " gap_threshold_segments=" << m_pendingGapThreshold
                        << " ratio_at_recovery=" << m_pendingRatio);
        }
    }
}

uint32_t
TcpHarpCubicV6::GetNormalTarget(const TcpSocketState& tcb) const
{
    const uint32_t normalSegments =
        std::max(static_cast<uint32_t>(tcb.GetCwndInSegments() * m_normalBeta), 2U);
    return normalSegments * tcb.m_segmentSize;
}

uint32_t
TcpHarpCubicV6::GetSnapshotTarget(double beta) const
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

double
TcpHarpCubicV6::SelectNormalLossBeta(Ptr<const TcpSocketState> tcb,
                                     uint32_t bytesInFlight)
{
    (void)tcb;
    (void)bytesInFlight;
    return m_normalBeta;
}

bool
TcpHarpCubicV6::GetRecoveryTargetUpdate(Ptr<const TcpSocketState> tcb, uint32_t& target)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    ExpireTimedState(nowNs);

    if (!m_lateUpdateReady || !m_pendingRecoveryValid)
    {
        return false;
    }

    if (tcb->m_congState != TcpSocketState::CA_RECOVERY)
    {
        NS_LOG_INFO("HarpV6Decision"
                    << " classification=LATE_SIGNAL_AFTER_RECOVERY"
                    << " time_ns=" << nowNs
                    << " epoch=" << m_lateSignal.epoch
                    << " gap_segments=" << m_pendingGapSegments);
        ClearPendingRecovery();
        return false;
    }

    const double effectiveBeta = std::clamp(m_handoverBeta, m_normalBeta, 1.0);
    const uint32_t adaptiveTarget = GetSnapshotTarget(effectiveBeta);
    const uint32_t safeTarget =
        std::clamp(m_pendingBytesInFlight, m_pendingNormalTarget, adaptiveTarget);

    target = safeTarget;
    m_activeSignalValid = true;
    m_activeDirection = m_lateSignal.direction;
    m_activeEpoch = m_lateSignal.epoch;
    m_activeUntilNs = m_lateSignal.timeNs + m_handoverWindow.GetNanoSeconds();
    m_activeTargetCwnd = safeTarget;

    NS_LOG_INFO("HarpV6Decision"
                << " classification=LATE_SIGNAL_SAFE_UPDATE"
                << " direction="
                << (m_lateSignal.merged
                        ? "DATA+ACK"
                        : (m_lateSignal.direction == HandoverDirection::DATA_PATH ? "DATA"
                                                                                 : "ACK"))
                << " epoch=" << m_lateSignal.epoch
                << " time_since_recovery_ms="
                << static_cast<double>(nowNs - m_pendingRecoveryTimeNs) / 1000000.0
                << " ack_range_segments=" << m_pendingGapSegments
                << " gap_threshold_segments=" << m_pendingGapThreshold
                << " gap_ge_threshold=1"
                << " ratio=" << m_pendingRatio
                << " configured_beta=" << effectiveBeta
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
TcpHarpCubicV6::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    const int64_t windowNs = m_handoverWindow.GetNanoSeconds();
    const uint32_t currentCwnd = tcb->m_cWnd;
    const uint32_t cwndSegments = tcb->GetCwndInSegments();
    const bool ackObservedNow = m_lastAckObservationTimeNs == nowNs;

    ExpireTimedState(nowNs);
    ClearPendingRecovery();

    double baselineMedian = 0.0;
    double baselineMad = 0.0;
    const uint32_t gapThreshold =
        GetDynamicGapThreshold(&baselineMedian, &baselineMad);
    const SignalSelection signal = SelectRecentSignal(nowNs);
    const bool recentSignal = signal.valid;

    double ratio = 0.0;
    if (m_lastAckRangeValid && cwndSegments > 0)
    {
        ratio = static_cast<double>(m_lastAckRangeSegments) /
                static_cast<double>(cwndSegments);
    }

    const bool gapQualified = ackObservedNow && m_lastAckRangeValid &&
                              m_lastAckRangeSegments >= gapThreshold;
    const bool candidate = m_enableAdaptiveBeta && recentSignal && gapQualified;
    // DATA and ACK marks are two observations of the same protection episode.
    // Once an episode is active, direction and per-direction epoch changes are
    // logging information only: every Recovery still has to pass the gap gate,
    // but a direction switch must not replace the saved target or extend the
    // original deadline.
    const bool activeEpisode =
        candidate && m_activeSignalValid && nowNs <= m_activeUntilNs;

    // Keep the expired deadline so that a later-arriving DATA/ACK mark which
    // was received inside the old episode cannot start a second, overlapping
    // 250 ms episode after the first deadline.  A genuinely new signal has a
    // timestamp after this deadline and may establish the next episode.
    const bool signalCoveredByPreviousEpisode =
        candidate && !m_activeSignalValid && m_activeUntilNs >= 0 &&
        signal.timeNs <= m_activeUntilNs;

    if (activeEpisode)
    {
        const uint32_t targetCwnd = std::min(currentCwnd, m_activeTargetCwnd);
        NS_LOG_INFO("HarpV6Decision"
                    << " classification=HANDOVER_GAP_SAFE_FOLLOWUP"
                    << " direction="
                    << (signal.merged
                            ? "DATA+ACK"
                            : (signal.direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"))
                    << " epoch=" << signal.epoch
                    << " ack_range_segments=" << m_lastAckRangeSegments
                    << " gap_threshold_segments=" << gapThreshold
                    << " gap_ge_threshold=1"
                    << " ratio=" << ratio
                    << " configured_beta=" << m_handoverBeta
                    << " cwnd_segments=" << cwndSegments
                    << " safe_target_segments=" << targetCwnd / tcb->m_segmentSize);
        return targetCwnd;
    }

    if (candidate && !signalCoveredByPreviousEpisode)
    {
        const double effectiveBeta = std::clamp(m_handoverBeta, m_normalBeta, 1.0);
        SetAttribute("Beta", DoubleValue(effectiveBeta));
        const uint32_t adaptiveTarget = TcpCubic::GetSsThresh(tcb, bytesInFlight);
        SetAttribute("Beta", DoubleValue(m_normalBeta));
        const uint32_t normalTarget = GetNormalTarget(*tcb);
        const uint32_t safeTarget = std::clamp(bytesInFlight, normalTarget, adaptiveTarget);

        m_activeSignalValid = true;
        m_activeDirection = signal.direction;
        m_activeEpoch = signal.epoch;
        m_activeUntilNs = signal.timeNs + windowNs;
        m_activeTargetCwnd = safeTarget;

        const double appliedBeta =
            currentCwnd > 0 ? static_cast<double>(safeTarget) / currentCwnd : 0.0;
        NS_LOG_INFO("HarpV6Decision"
                    << " classification=HANDOVER_GAP_SAFE"
                    << " direction="
                    << (signal.merged
                            ? "DATA+ACK"
                            : (signal.direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"))
                    << " epoch=" << signal.epoch
                    << " time_since_signal_ms="
                    << static_cast<double>(nowNs - signal.timeNs) / 1000000.0
                    << " ack_range_segments=" << m_lastAckRangeSegments
                    << " gap_threshold_segments=" << gapThreshold
                    << " gap_ge_threshold=1"
                    << " baseline_samples=" << m_confirmedNormalGaps.size()
                    << " baseline_median=" << baselineMedian
                    << " baseline_mad=" << baselineMad
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

    const double requestedNormalBeta = SelectNormalLossBeta(tcb, bytesInFlight);
    const double selectedNormalBeta =
        std::isfinite(requestedNormalBeta)
            ? std::clamp(requestedNormalBeta, 0.0, 1.0)
            : m_normalBeta;
    SetAttribute("Beta", DoubleValue(selectedNormalBeta));
    const uint32_t selectedNormalTarget = TcpCubic::GetSsThresh(tcb, bytesInFlight);
    SetAttribute("Beta", DoubleValue(m_normalBeta));
    const uint32_t baselineNormalTarget = GetNormalTarget(*tcb);

    bool waitingForLateSignal = false;
    if (ackObservedNow && m_lastAckRangeValid && !recentSignal)
    {
        QueuePotentialNormalGap(nowNs, m_lastAckRangeSegments);
        if (m_enableAdaptiveBeta && windowNs > 0 && gapQualified)
        {
            m_pendingRecoveryValid = true;
            m_pendingRecoveryTimeNs = nowNs;
            m_pendingRecoveryUntilNs = nowNs + windowNs;
            m_pendingCwnd = currentCwnd;
            m_pendingBytesInFlight = bytesInFlight;
            // A late qualified HARP signal must retain V6's original 0.70
            // lower bound even when a subclass initially selected a larger
            // normal-loss target (for example ML random-loss beta 0.95).
            m_pendingNormalTarget = baselineNormalTarget;
            m_pendingSegmentSize = tcb->m_segmentSize;
            m_pendingGapSegments = m_lastAckRangeSegments;
            m_pendingGapThreshold = gapThreshold;
            m_pendingRatio = ratio;
            waitingForLateSignal = true;

            NS_LOG_INFO("HarpV6Decision"
                        << " classification=LATE_SIGNAL_WAIT"
                        << " recovery_time_ns=" << nowNs
                        << " expires_ns=" << m_pendingRecoveryUntilNs
                        << " ack_range_segments=" << m_pendingGapSegments
                        << " gap_threshold_segments=" << m_pendingGapThreshold
                        << " gap_ge_threshold=1"
                        << " ratio=" << ratio
                        << " cwnd_before_segments=" << cwndSegments
                        << " bytes_in_flight_at_recovery_segments="
                        << bytesInFlight / tcb->m_segmentSize
                        << " normal_target_segments="
                        << selectedNormalTarget / tcb->m_segmentSize
                        << " baseline_normal_target_segments="
                        << baselineNormalTarget / tcb->m_segmentSize);
        }
    }

    const char* classification =
        signalCoveredByPreviousEpisode
            ? "SIGNAL_PREVIOUS_EPISODE_NORMAL"
            : (recentSignal
                   ? (m_lastAckRangeValid ? "SIGNAL_SMALL_GAP_NORMAL" : "SIGNAL_NO_GAP_NORMAL")
                   : "NORMAL_LOSS");
    const char* direction =
        !recentSignal
            ? "NONE"
            : (signal.merged
                   ? "DATA+ACK"
                   : (signal.direction == HandoverDirection::DATA_PATH ? "DATA" : "ACK"));
    NS_LOG_INFO("HarpV6Decision"
                << " classification=" << classification
                << " direction=" << direction
                << " signal_available=" << (recentSignal ? 1 : 0)
                << " late_signal_wait=" << (waitingForLateSignal ? 1 : 0)
                << " ack_range_segments="
                << (m_lastAckRangeValid ? static_cast<int64_t>(m_lastAckRangeSegments) : -1)
                << " gap_threshold_segments=" << gapThreshold
                << " gap_ge_threshold=" << (gapQualified ? 1 : 0)
                << " baseline_samples=" << m_confirmedNormalGaps.size()
                << " baseline_median=" << baselineMedian
                << " baseline_mad=" << baselineMad
                << " ratio=" << ratio
                << " effective_beta=" << selectedNormalBeta
                << " cwnd_segments=" << cwndSegments);
    return selectedNormalTarget;
}

Ptr<TcpCongestionOps>
TcpHarpCubicV6::Fork()
{
    return CopyObject<TcpHarpCubicV6>(this);
}

} // namespace ns3
