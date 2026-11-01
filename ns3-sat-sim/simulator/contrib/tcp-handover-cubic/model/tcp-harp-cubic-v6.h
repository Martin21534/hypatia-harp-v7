/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V6_H
#define TCP_HARP_CUBIC_V6_H

#include "ns3/tcp-cubic.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace ns3
{

/**
 * HARP-Cubic 6.0: conservative packet-signal detection with an adaptive gap gate.
 *
 * A packet-carried HARP signal is necessary, but it is not sufficient, for
 * protection.  Every Recovery must also have an ACK-range gap at or above a
 * robust threshold learned from confirmed non-handover Recoveries.  DATA and
 * ACK signals use the same decision and the same bounded target.
 */
class TcpHarpCubicV6 : public TcpCubic
{
  public:
    enum class HandoverDirection : uint8_t
    {
        DATA_PATH,
        ACK_PATH
    };

    static TypeId GetTypeId();

    TcpHarpCubicV6();
    TcpHarpCubicV6(const TcpHarpCubicV6& other);

    std::string GetName() const override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    void ObserveAck(const TcpHeader& tcpHeader,
                    uint32_t segmentSize,
                    uint32_t cwndSegments) override;
    bool GetRecoveryTargetUpdate(Ptr<const TcpSocketState> tcb, uint32_t& target) override;

  protected:
    /**
     * Select the beta used only for a Recovery classified as normal by V6.
     *
     * Qualified handover Recoveries, active-episode followups, and late-signal
     * updates bypass this hook so subclasses cannot override the V6 safe target.
     */
    virtual double SelectNormalLossBeta(Ptr<const TcpSocketState> tcb,
                                        uint32_t bytesInFlight);

  private:
    struct PendingGapSample
    {
        int64_t recoveryTimeNs{-1};
        int64_t confirmAfterNs{-1};
        uint32_t gapSegments{0};
    };

    struct SignalSelection
    {
        bool valid{false};
        bool merged{false};
        HandoverDirection direction{HandoverDirection::ACK_PATH};
        uint16_t epoch{0};
        int64_t timeNs{-1};
    };

    uint32_t GetNormalTarget(const TcpSocketState& tcb) const;
    uint32_t GetSnapshotTarget(double beta) const;
    uint32_t GetDynamicGapThreshold(double* median = nullptr, double* mad = nullptr) const;
    SignalSelection SelectRecentSignal(int64_t nowNs) const;
    void AddConfirmedNormalGap(uint32_t gapSegments);
    void QueuePotentialNormalGap(int64_t nowNs, uint32_t gapSegments);
    void DiscardUnconfirmedGapsForSignal(int64_t signalTimeNs);
    void ExpireTimedState(int64_t nowNs);
    void ClearPendingRecovery();
    static double Median(std::vector<double> values);

    bool m_enableAdaptiveBeta{true};
    Time m_handoverWindow{MilliSeconds(250)};
    Time m_directionMergeWindow{MilliSeconds(1)};
    double m_normalBeta{0.70};
    double m_handoverBeta{0.85};

    uint32_t m_gapThresholdFloor{7};
    uint32_t m_gapMargin{3};
    double m_gapMadMultiplier{3.0};
    uint32_t m_gapBaselineWindow{9};
    uint32_t m_gapBaselineMinSamples{3};

    uint32_t m_lastAckRangeSegments{0};
    bool m_lastAckRangeValid{false};
    int64_t m_lastAckObservationTimeNs{-1};

    bool m_lastDataSignalValid{false};
    bool m_lastAckSignalValid{false};
    uint16_t m_lastDataEpoch{0};
    uint16_t m_lastAckEpoch{0};
    int64_t m_lastDataSignalTimeNs{-1};
    int64_t m_lastAckSignalTimeNs{-1};

    bool m_activeSignalValid{false};
    // Direction/epoch identify the signal used for logging.  They do not
    // divide protection episodes: DATA and ACK share one fixed deadline and
    // one saved target.
    HandoverDirection m_activeDirection{HandoverDirection::DATA_PATH};
    uint16_t m_activeEpoch{0};
    // Retained after expiry so a mark received during the old episode cannot
    // reopen it from the opposite direction.
    int64_t m_activeUntilNs{-1};
    uint32_t m_activeTargetCwnd{0};

    std::deque<uint32_t> m_confirmedNormalGaps;
    std::deque<PendingGapSample> m_pendingGapSamples;

    bool m_pendingRecoveryValid{false};
    int64_t m_pendingRecoveryTimeNs{-1};
    int64_t m_pendingRecoveryUntilNs{-1};
    uint32_t m_pendingCwnd{0};
    uint32_t m_pendingBytesInFlight{0};
    uint32_t m_pendingNormalTarget{0};
    uint32_t m_pendingSegmentSize{0};
    uint32_t m_pendingGapSegments{0};
    uint32_t m_pendingGapThreshold{0};
    double m_pendingRatio{0.0};

    bool m_lateUpdateReady{false};
    SignalSelection m_lateSignal;
};

} // namespace ns3

#endif // TCP_HARP_CUBIC_V6_H
