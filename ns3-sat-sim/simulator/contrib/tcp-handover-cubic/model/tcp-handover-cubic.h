/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HANDOVER_CUBIC_H
#define TCP_HANDOVER_CUBIC_H

#include "ns3/tcp-cubic.h"

#include <cstdint>

namespace ns3
{

class TcpHarpRecovery;

/**
 * CUBIC with short-lived congestion-window protection for LEO handovers.
 *
 * A recovery is treated as a handover-reordering candidate when it occurs
 * inside DetectionWindow and its ACK-to-FACK range/cwnd ratio reaches
 * AckRangeRatioThreshold.  A confirmed candidate starts ProtectionWindow.
 * Packet loss recovery and fast retransmit are still performed; only CUBIC's
 * multiplicative window decrease is bypassed while protection is active.
 */
class TcpHandoverCubic : public TcpCubic
{
  public:
    static TypeId GetTypeId();

    TcpHandoverCubic();
    TcpHandoverCubic(const TcpHandoverCubic& other);

    std::string GetName() const override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    void ObserveAck(const TcpHeader& tcpHeader,
                    uint32_t segmentSize,
                    uint32_t cwndSegments) override;

    /** Record an exact cross-layer handover notification. */
    static void NotifyHandover();

  private:
    friend class TcpHarpRecovery;

    /**
     * 取出 GetSsThresh() 對「這一次 recovery」所做的保護判定。
     *
     * TcpSocketBase 會先呼叫 GetSsThresh()，接著立刻呼叫 recovery
     * algorithm 的 EnterRecovery()。用 socket state 與模擬時間核對，
     * 可避免把舊判定誤用到下一次 recovery。
     */
    static bool ConsumePreserveCwndDecision(Ptr<const TcpSocketState> tcb);

    bool m_enableHandoverProtection{true};
    Time m_detectionWindow{MilliSeconds(250)};
    Time m_protectionWindow{MilliSeconds(250)};
    double m_ackRangeRatioThreshold{0.015};

    uint32_t m_lastAckRangeSegments{0};
    bool m_lastAckRangeValid{false};
    int64_t m_handoverProtectionUntilNs{-1};

    static int64_t s_lastHandoverTimeNs;

    // 改進處：把 HARP 對本次 recovery 的明確判定交給 TcpHarpRecovery。
    static const TcpSocketState* s_lastDecisionTcb;
    static int64_t s_lastDecisionTimeNs;
    static bool s_lastDecisionPreserveCwnd;
};

} // namespace ns3

#endif // TCP_HANDOVER_CUBIC_H
