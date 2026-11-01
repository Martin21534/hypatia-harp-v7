/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TCP_HARP_CUBIC_V7_H
#define TCP_HARP_CUBIC_V7_H

#include "ns3/event-id.h"
#include "ns3/tcp-harp-cubic-v6.h"
#include "ns3/traced-value.h"

#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace ns3
{

class Packet;
class TcpHeader;
class TcpSocketBase;

/**
 * HARP-Cubic 7.0: HARP 6.0 with conservative v2-A random-loss assistance.
 *
 * HARP 6.0 remains the primary policy. A qualified packet-carried HARP
 * signal, its adaptive ACK-gap gate, the bounded handover target, active
 * episode handling, and late-signal revision are all evaluated by the V6
 * base class. The embedded sender-only v2-A model is consulted only when V6
 * reaches its normal-loss path. A high-confidence random-loss prediction may
 * then replace the normal beta with RandomBeta.
 */
class TcpHarpCubicV7 : public TcpHarpCubicV6
{
  public:
    static TypeId GetTypeId();

    TcpHarpCubicV7();
    TcpHarpCubicV7(const TcpHarpCubicV7& other);
    ~TcpHarpCubicV7() override;

    std::string GetName() const override;
    void Init(Ptr<TcpSocketState> tcb) override;
    void PktsAcked(Ptr<TcpSocketState> tcb,
                   uint32_t segmentsAcked,
                   const Time& rtt) override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    Ptr<TcpCongestionOps> Fork() override;

    uint64_t GetMlLossDecisions() const;
    uint64_t GetMlRandomAssists() const;

  protected:
    double SelectNormalLossBeta(Ptr<const TcpSocketState> tcb,
                                uint32_t bytesInFlight) override;

    /**
     * Return a ready congestion prediction for the current sender features.
     *
     * This narrow virtual seam keeps policy tests deterministic without
     * exposing simulator drop cause to the deployed model.
     */
    virtual bool GetCongestionPrediction(double& probability) const;

  private:
    static constexpr uint32_t FEATURE_COUNT = 7;

    void EnsureStarted(Ptr<TcpSocketState> tcb);
    void ConnectSocketTrace();
    void TxTrace(Ptr<const Packet> packet,
                 const TcpHeader& header,
                 Ptr<const TcpSocketBase> socket);
    void SampleInterval();
    double EstimateBandwidthPps() const;

    Time m_monitorInterval{MilliSeconds(100)};
    uint32_t m_warmupIntervals{5};
    bool m_assistEnabled{true};
    double m_randomThreshold{0.00025};
    double m_randomBeta{0.95};
    Time m_bandwidthWindow{Seconds(5)};
    double m_bandwidthQuantile{0.90};

    Ptr<TcpSocketState> m_tcb;
    Ptr<TcpSocketBase> m_socket;
    EventId m_sampleEvent;
    bool m_started{false};
    bool m_traceConnected{false};
    Time m_intervalStart{Seconds(0)};

    uint64_t m_bytesSent{0};
    uint64_t m_bytesAcked{0};
    uint32_t m_senderLossEvents{0};
    std::vector<double> m_rttSamples;
    double m_minAverageRtt{0.0};
    double m_lossHistory{0.0};
    std::array<float, FEATURE_COUNT> m_latestV2Features{{0.0f}};
    std::deque<std::pair<double, double>> m_deliveryRateHistory;
    double m_estimatedBandwidthPps{0.0};
    double m_estimatedBdpPackets{0.0};
    double m_minRttSeconds{0.0};
    bool m_latestV2FeatureValid{false};

    uint64_t m_mlLossDecisions{0};
    uint64_t m_mlRandomAssists{0};
    TracedValue<double> m_congestionProbability{1.0};
    TracedValue<double> m_selectedNormalBeta{0.0};
    TracedValue<uint32_t> m_randomAssist{0};
};

} // namespace ns3

#endif // TCP_HARP_CUBIC_V7_H
