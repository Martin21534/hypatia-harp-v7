/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V21_H
#define TCP_HARP_CUBIC_V21_H

#include "ns3/tcp-cubic.h"

#include <cstdint>

namespace ns3
{

/**
 * HARP-Cubic 2.1: conservative DATA-path beta sensitivity variant of V2.
 *
 * V2.1 intentionally keeps the V2 detector, handover window, ACK-path beta,
 * normal-loss beta, and PRR behavior unchanged.  Its only algorithmic change
 * is the default DATA-path beta: 0.80 instead of 0.85.
 */
class TcpHarpCubicV21 : public TcpCubic
{
  public:
    enum class HandoverDirection : uint8_t
    {
        DATA_PATH,
        ACK_PATH
    };

    static TypeId GetTypeId();

    TcpHarpCubicV21();
    TcpHarpCubicV21(const TcpHarpCubicV21& other);

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
    // V2.1 唯一的演算法差異：DATA beta 從 0.85 降為 0.80。
    double m_dataHandoverBeta{0.80};

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

#endif // TCP_HARP_CUBIC_V21_H
