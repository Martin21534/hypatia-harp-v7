/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V5_H
#define TCP_HARP_CUBIC_V5_H

#include "ns3/tcp-cubic.h"

#include <cstdint>

namespace ns3
{

/**
 * HARP-Cubic 5.0: V3's bounded target driven only by packet-carried signals.
 *
 * Unlike V4, this class has no routing notification entry point and no static
 * handover state.  Each socket learns an event only by parsing a DATA_ECHO or
 * ACK_MARK serialized in an ACK that the socket actually receives.
 */
class TcpHarpCubicV5 : public TcpCubic
{
  public:
    enum class HandoverDirection : uint8_t
    {
        DATA_PATH,
        ACK_PATH
    };

    static TypeId GetTypeId();

    TcpHarpCubicV5();
    TcpHarpCubicV5(const TcpHarpCubicV5& other);

    std::string GetName() const override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    void ObserveAck(const TcpHeader& tcpHeader,
                    uint32_t segmentSize,
                    uint32_t cwndSegments) override;
    bool GetRecoveryTargetUpdate(Ptr<const TcpSocketState> tcb, uint32_t& target) override;

  private:
    uint32_t GetNormalTarget(const TcpSocketState& tcb) const;
    uint32_t GetSnapshotTarget(double beta) const;
    void ExpireTimedState(int64_t nowNs);
    void ClearPendingRecovery();

    bool m_enableAdaptiveBeta{true};
    Time m_handoverWindow{MilliSeconds(250)};
    Time m_directionMergeWindow{MilliSeconds(1)};
    double m_ackRangeRatioThreshold{0.015};
    double m_normalBeta{0.70};
    double m_ackHandoverBeta{0.75};
    double m_dataHandoverBeta{0.85};

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
    HandoverDirection m_activeDirection{HandoverDirection::DATA_PATH};
    uint16_t m_activeEpoch{0};
    int64_t m_activeUntilNs{-1};
    uint32_t m_activeTargetCwnd{0};
    double m_activeBeta{0.70};

    // A high-ratio Recovery that happened before its packet-carried signal
    // arrived.  A new ECHO may revise only this active Recovery's target.
    bool m_pendingRecoveryValid{false};
    int64_t m_pendingRecoveryTimeNs{-1};
    int64_t m_pendingRecoveryUntilNs{-1};
    uint32_t m_pendingCwnd{0};
    uint32_t m_pendingBytesInFlight{0};
    uint32_t m_pendingNormalTarget{0};
    uint32_t m_pendingSegmentSize{0};
    double m_pendingRatio{0.0};

    bool m_lateUpdateReady{false};
    HandoverDirection m_lateUpdateDirection{HandoverDirection::DATA_PATH};
    uint16_t m_lateUpdateEpoch{0};
    int64_t m_lateSignalTimeNs{-1};
};

} // namespace ns3

#endif // TCP_HARP_CUBIC_V5_H
