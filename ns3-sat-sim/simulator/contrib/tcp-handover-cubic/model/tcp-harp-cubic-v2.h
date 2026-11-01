/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V2_H
#define TCP_HARP_CUBIC_V2_H

#include "ns3/tcp-cubic.h"

#include <cstdint>

namespace ns3
{

/**
 * HARP-Cubic 2.0: direction-aware CUBIC with an adaptive multiplicative decrease.
 *
 * Unlike TcpHandoverCubic, this version does not completely preserve cwnd.
 * A recovery near a handover selects a fixed, direction-aware beta after the
 * ACK-range ratio reaches the detection threshold.  Normal loss continues to
 * use NormalBeta.
 */
class TcpHarpCubicV2 : public TcpCubic
{
  public:
    enum class HandoverDirection : uint8_t
    {
        DATA_PATH,
        ACK_PATH
    };

    static TypeId GetTypeId();

    TcpHarpCubicV2();
    TcpHarpCubicV2(const TcpHarpCubicV2& other);

    std::string GetName() const override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    void ObserveAck(const TcpHeader& tcpHeader,
                    uint32_t segmentSize,
                    uint32_t cwndSegments) override;

    /** Record the simulation time and direction of a route handover. */
    static void NotifyHandover(HandoverDirection direction);

  private:
    bool m_enableAdaptiveBeta{true};
    Time m_handoverWindow{MilliSeconds(250)};
    double m_ackRangeRatioThreshold{0.015};
    double m_normalBeta{0.70};
    double m_ackHandoverBeta{0.75};
    double m_dataHandoverBeta{0.85};

    uint32_t m_lastAckRangeSegments{0};
    bool m_lastAckRangeValid{false};

    // 同一次換手只建立一次降窗目標，避免後續 Recovery 重複乘上 beta。
    int64_t m_activeHandoverTimeNs{-1};
    int64_t m_activeHandoverUntilNs{-1};
    uint32_t m_activeTargetCwnd{0};
    double m_activeBeta{0.70};

    static int64_t s_lastDataHandoverTimeNs;
    static int64_t s_lastAckHandoverTimeNs;
};

} // namespace ns3

#endif // TCP_HARP_CUBIC_V2_H
