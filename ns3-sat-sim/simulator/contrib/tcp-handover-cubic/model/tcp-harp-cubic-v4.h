/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V4_H
#define TCP_HARP_CUBIC_V4_H

#include "ns3/tcp-cubic.h"

#include <cstdint>

namespace ns3
{

/**
 * HARP-Cubic 4.0: V3's safe target driven by a live packet-path handover event.
 *
 * Detector, beta values, handover window and bounded safe target are identical
 * to V3.  The only experimental change is the notification source: V4 is
 * notified when a TCP packet first uses a different source-side next hop.
 */
class TcpHarpCubicV4 : public TcpCubic
{
  public:
    enum class HandoverDirection : uint8_t
    {
        DATA_PATH,
        ACK_PATH
    };

    static TypeId GetTypeId();

    TcpHarpCubicV4();
    TcpHarpCubicV4(const TcpHarpCubicV4& other);

    std::string GetName() const override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    void ObserveAck(const TcpHeader& tcpHeader,
                    uint32_t segmentSize,
                    uint32_t cwndSegments) override;

    /** Record when an active TCP packet first uses a changed next hop. */
    static void NotifyHandover(HandoverDirection direction);

  private:
    uint32_t GetNormalTarget(const TcpSocketState& tcb) const;

    bool m_enableAdaptiveBeta{true};
    Time m_handoverWindow{MilliSeconds(250)};
    Time m_directionMergeWindow{MilliSeconds(1)};
    double m_ackRangeRatioThreshold{0.015};
    double m_normalBeta{0.70};
    double m_ackHandoverBeta{0.75};
    double m_dataHandoverBeta{0.85};

    uint32_t m_lastAckRangeSegments{0};
    bool m_lastAckRangeValid{false};

    int64_t m_activeHandoverTimeNs{-1};
    int64_t m_activeHandoverUntilNs{-1};
    uint32_t m_activeTargetCwnd{0};
    double m_activeBeta{0.70};

    static int64_t s_lastDataHandoverTimeNs;
    static int64_t s_lastAckHandoverTimeNs;
};

} // namespace ns3

#endif // TCP_HARP_CUBIC_V4_H
