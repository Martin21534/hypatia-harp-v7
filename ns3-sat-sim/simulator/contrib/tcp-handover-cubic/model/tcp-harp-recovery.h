/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_RECOVERY_H
#define TCP_HARP_RECOVERY_H

#include "ns3/tcp-recovery-ops.h"

namespace ns3
{

/**
 * PRR recovery with an optional HARP congestion-window preservation path.
 *
 * Without an explicit decision from TcpHandoverCubic, this class follows the
 * same PRR calculations as TcpPrrRecovery.  During a confirmed HARP protection
 * interval, loss bookkeeping and retransmission remain active while the actual
 * congestion window is kept at its pre-recovery value.
 */
class TcpHarpRecovery : public TcpClassicRecovery
{
  public:
    static TypeId GetTypeId();

    TcpHarpRecovery();
    TcpHarpRecovery(const TcpHarpRecovery& other);
    ~TcpHarpRecovery() override;

    std::string GetName() const override;

    void EnterRecovery(Ptr<TcpSocketState> tcb,
                       uint32_t dupAckCount,
                       uint32_t unAckDataCount,
                       uint32_t deliveredBytes) override;

    void DoRecovery(Ptr<TcpSocketState> tcb,
                    uint32_t deliveredBytes,
                    bool isDupAck) override;

    void ExitRecovery(Ptr<TcpSocketState> tcb) override;
    void UpdateBytesSent(uint32_t bytesSent) override;
    Ptr<TcpRecoveryOps> Fork() override;

  private:
    uint32_t m_prrDelivered{0};
    uint32_t m_prrOut{0};
    uint32_t m_recoveryFlightSize{0};

    // 改進處：只在 HARP 明確確認的 recovery 期間維持進入前的 cwnd。
    bool m_preserveCwnd{false};
    uint32_t m_preservedCwnd{0};
};

} // namespace ns3

#endif // TCP_HARP_RECOVERY_H
