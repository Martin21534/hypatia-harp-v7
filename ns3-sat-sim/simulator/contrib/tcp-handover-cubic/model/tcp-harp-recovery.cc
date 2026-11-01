/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-recovery.h"

#include "tcp-handover-cubic.h"

#include "ns3/log.h"
#include "ns3/tcp-socket-state.h"

#include <algorithm>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpRecovery");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpRecovery);

TypeId
TcpHarpRecovery::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TcpHarpRecovery")
                            .SetParent<TcpClassicRecovery>()
                            .AddConstructor<TcpHarpRecovery>()
                            .SetGroupName("Internet");
    return tid;
}

TcpHarpRecovery::TcpHarpRecovery()
    : TcpClassicRecovery()
{
    NS_LOG_FUNCTION(this);
}

TcpHarpRecovery::TcpHarpRecovery(const TcpHarpRecovery& other)
    : TcpClassicRecovery(other),
      m_prrDelivered(other.m_prrDelivered),
      m_prrOut(other.m_prrOut),
      m_recoveryFlightSize(other.m_recoveryFlightSize),
      m_preserveCwnd(other.m_preserveCwnd),
      m_preservedCwnd(other.m_preservedCwnd)
{
    NS_LOG_FUNCTION(this);
}

TcpHarpRecovery::~TcpHarpRecovery()
{
    NS_LOG_FUNCTION(this);
}

void
TcpHarpRecovery::EnterRecovery(Ptr<TcpSocketState> tcb,
                               uint32_t dupAckCount [[maybe_unused]],
                               uint32_t unAckDataCount [[maybe_unused]],
                               uint32_t deliveredBytes)
{
    NS_LOG_FUNCTION(this << tcb << dupAckCount << unAckDataCount);

    m_prrOut = 0;
    m_prrDelivered = 0;
    m_recoveryFlightSize = tcb->m_bytesInFlight;

    // 改進處：只接收 GetSsThresh() 剛剛對同一個 socket 做出的判定。
    m_preserveCwnd = TcpHandoverCubic::ConsumePreserveCwndDecision(tcb);
    m_preservedCwnd = tcb->m_cWnd;

    NS_LOG_INFO("EnterHarpRecovery"
                << " preserve_cwnd=" << m_preserveCwnd
                << " cwnd=" << m_preservedCwnd
                << " ssthresh=" << tcb->m_ssThresh);

    DoRecovery(tcb, deliveredBytes, true);
}

void
TcpHarpRecovery::DoRecovery(Ptr<TcpSocketState> tcb,
                            uint32_t deliveredBytes,
                            bool isDupAck)
{
    NS_LOG_FUNCTION(this << tcb << deliveredBytes << isDupAck);

    if (isDupAck && m_prrDelivered < m_recoveryFlightSize)
    {
        deliveredBytes += tcb->m_segmentSize;
    }
    if (deliveredBytes == 0)
    {
        return;
    }

    m_prrDelivered += deliveredBytes;

    if (m_preserveCwnd)
    {
        // 改進處：保留 recovery/SACK/重傳流程，但不要讓 PRR 暫時降低實際 cwnd。
        // 若其他機制已把 cwnd 提高，也不會在這裡反向把它壓回舊值。
        tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), m_preservedCwnd);
        tcb->m_cWndInfl = tcb->m_cWnd;
        return;
    }

    // 未受 HARP 保護時，以下維持 ns-3 TcpPrrRecovery 的原始計算方式。
    int sendCount;
    if (tcb->m_bytesInFlight > tcb->m_ssThresh)
    {
        sendCount =
            std::ceil(m_prrDelivered * tcb->m_ssThresh * 1.0 / m_recoveryFlightSize) - m_prrOut;
    }
    else
    {
        int limit = std::max(m_prrDelivered - m_prrOut, deliveredBytes);
        const bool safeAck = tcb->m_isRetransDataAcked;
        if (safeAck)
        {
            limit += tcb->m_segmentSize;
        }
        sendCount = std::min(limit,
                             static_cast<int>(tcb->m_ssThresh - tcb->m_bytesInFlight));
    }

    sendCount = std::max(sendCount,
                         static_cast<int>(m_prrOut > 0 ? 0 : tcb->m_segmentSize));
    tcb->m_cWnd = tcb->m_bytesInFlight + sendCount;
    tcb->m_cWndInfl = tcb->m_cWnd;
}

void
TcpHarpRecovery::ExitRecovery(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);
    tcb->m_cWndInfl = tcb->m_cWnd;
    m_preserveCwnd = false;
    m_preservedCwnd = 0;
}

void
TcpHarpRecovery::UpdateBytesSent(uint32_t bytesSent)
{
    NS_LOG_FUNCTION(this << bytesSent);
    m_prrOut += bytesSent;
}

Ptr<TcpRecoveryOps>
TcpHarpRecovery::Fork()
{
    return CopyObject<TcpHarpRecovery>(this);
}

std::string
TcpHarpRecovery::GetName() const
{
    return "HarpRecovery";
}

} // namespace ns3
