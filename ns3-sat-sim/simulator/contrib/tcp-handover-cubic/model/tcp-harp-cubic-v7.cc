/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tcp-harp-cubic-v7.h"

#include "tcp-hybrid-cubic.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/object-vector.h"
#include "ns3/packet.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpHarpCubicV7");
NS_OBJECT_ENSURE_REGISTERED(TcpHarpCubicV7);

TypeId
TcpHarpCubicV7::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpHarpCubicV7")
            .SetParent<TcpHarpCubicV6>()
            .SetGroupName("Internet")
            .AddConstructor<TcpHarpCubicV7>()
            .AddAttribute("MonitorInterval",
                          "Sender feature-sampling interval for the v2-A model",
                          TimeValue(MilliSeconds(100)),
                          MakeTimeAccessor(&TcpHarpCubicV7::m_monitorInterval),
                          MakeTimeChecker())
            .AddAttribute("WarmupIntervals",
                          "Valid delivery-rate intervals required before ML assistance",
                          UintegerValue(5),
                          MakeUintegerAccessor(&TcpHarpCubicV7::m_warmupIntervals),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("AssistEnabled",
                          "Allow a ready v2-A prediction to alter V6 normal-loss beta",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpHarpCubicV7::m_assistEnabled),
                          MakeBooleanChecker())
            .AddAttribute("RandomThreshold",
                          "Maximum congestion probability that permits random-loss assistance",
                          DoubleValue(0.00025),
                          MakeDoubleAccessor(&TcpHarpCubicV7::m_randomThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("RandomBeta",
                          "Normal-loss beta selected for a high-confidence random loss",
                          DoubleValue(0.95),
                          MakeDoubleAccessor(&TcpHarpCubicV7::m_randomBeta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("V2BandwidthWindow",
                          "Sender delivery-rate history used by the v2-A estimator",
                          TimeValue(Seconds(5)),
                          MakeTimeAccessor(&TcpHarpCubicV7::m_bandwidthWindow),
                          MakeTimeChecker())
            .AddAttribute("V2BandwidthQuantile",
                          "High delivery-rate quantile used as the v2-A bandwidth estimate",
                          DoubleValue(0.90),
                          MakeDoubleAccessor(&TcpHarpCubicV7::m_bandwidthQuantile),
                          MakeDoubleChecker<double>(0.5, 1.0))
            .AddTraceSource("CongestionProbability",
                            "v2-A probability that a V6-normal loss is congestion-driven",
                            MakeTraceSourceAccessor(&TcpHarpCubicV7::m_congestionProbability),
                            "ns3::TracedValueCallback::Double")
            .AddTraceSource("SelectedNormalBeta",
                            "Beta selected for the current V6-normal loss",
                            MakeTraceSourceAccessor(&TcpHarpCubicV7::m_selectedNormalBeta),
                            "ns3::TracedValueCallback::Double")
            .AddTraceSource("RandomAssist",
                            "One when ML assistance changed a V6-normal loss response",
                            MakeTraceSourceAccessor(&TcpHarpCubicV7::m_randomAssist),
                            "ns3::TracedValueCallback::Uint32");
    return tid;
}

TcpHarpCubicV7::TcpHarpCubicV7()
    : TcpHarpCubicV6()
{
}

TcpHarpCubicV7::TcpHarpCubicV7(const TcpHarpCubicV7& other)
    : TcpHarpCubicV6(other),
      m_monitorInterval(other.m_monitorInterval),
      m_warmupIntervals(other.m_warmupIntervals),
      m_assistEnabled(other.m_assistEnabled),
      m_randomThreshold(other.m_randomThreshold),
      m_randomBeta(other.m_randomBeta),
      m_bandwidthWindow(other.m_bandwidthWindow),
      m_bandwidthQuantile(other.m_bandwidthQuantile)
{
}

TcpHarpCubicV7::~TcpHarpCubicV7()
{
    if (m_sampleEvent.IsPending())
    {
        m_sampleEvent.Cancel();
    }
}

std::string
TcpHarpCubicV7::GetName() const
{
    return "TcpHarpCubicV7";
}

void
TcpHarpCubicV7::Init(Ptr<TcpSocketState> tcb)
{
    m_tcb = tcb;
    TcpHarpCubicV6::Init(tcb);
}

void
TcpHarpCubicV7::PktsAcked(Ptr<TcpSocketState> tcb,
                          uint32_t segmentsAcked,
                          const Time& rtt)
{
    TcpHarpCubicV6::PktsAcked(tcb, segmentsAcked, rtt);
    EnsureStarted(tcb);
    m_bytesAcked += static_cast<uint64_t>(segmentsAcked) * tcb->m_segmentSize;
    if (rtt.IsStrictlyPositive())
    {
        m_rttSamples.push_back(rtt.GetSeconds());
    }
}

uint32_t
TcpHarpCubicV7::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    // Qualified HARP paths bypass SelectNormalLossBeta(), so reset the ML-only
    // traces before the V6 policy determines which branch owns this Recovery.
    m_congestionProbability = 1.0;
    m_selectedNormalBeta = 0.0;
    m_randomAssist = 0;

    const uint32_t target = TcpHarpCubicV6::GetSsThresh(tcb, bytesInFlight);
    ++m_senderLossEvents;
    return target;
}

Ptr<TcpCongestionOps>
TcpHarpCubicV7::Fork()
{
    return CopyObject<TcpHarpCubicV7>(this);
}

uint64_t
TcpHarpCubicV7::GetMlLossDecisions() const
{
    return m_mlLossDecisions;
}

uint64_t
TcpHarpCubicV7::GetMlRandomAssists() const
{
    return m_mlRandomAssists;
}

double
TcpHarpCubicV7::SelectNormalLossBeta(Ptr<const TcpSocketState> tcb,
                                     uint32_t bytesInFlight)
{
    const double fallback =
        TcpHarpCubicV6::SelectNormalLossBeta(tcb, bytesInFlight);
    double probability = 1.0;
    double selected = fallback;
    uint32_t assisted = 0;
    const bool controlReady = GetCongestionPrediction(probability);

    if (controlReady)
    {
        probability = std::clamp(probability, 0.0, 1.0);
        m_congestionProbability = probability;
        if (m_assistEnabled && probability <= m_randomThreshold)
        {
            // Random-loss assistance is deliberately conservative: even a
            // misconfigured RandomBeta must never reduce the V6 normal-loss
            // target more aggressively than the inherited policy.
            selected = std::max(fallback, m_randomBeta);
            assisted = selected > fallback ? 1 : 0;
        }
    }

    m_selectedNormalBeta = selected;
    m_randomAssist = assisted;
    ++m_mlLossDecisions;
    m_mlRandomAssists += assisted;

    NS_LOG_INFO("HarpV7MlDecision"
                << " control_ready=" << (controlReady ? 1 : 0)
                << " congestion_probability=" << probability
                << " threshold=" << m_randomThreshold
                << " assisted=" << assisted
                << " selected_normal_beta=" << selected);
    return selected;
}

bool
TcpHarpCubicV7::GetCongestionPrediction(double& probability) const
{
    if (!m_latestV2FeatureValid)
    {
        return false;
    }

    probability = TcpMlCubic::PredictV2CongestionForFeatures(m_latestV2Features);
    return std::isfinite(probability);
}

void
TcpHarpCubicV7::EnsureStarted(Ptr<TcpSocketState> tcb)
{
    m_tcb = tcb;
    if (m_started)
    {
        return;
    }

    NS_ABORT_MSG_IF(!m_monitorInterval.IsStrictlyPositive(),
                    "TcpHarpCubicV7 MonitorInterval must be positive");
    m_started = true;
    ConnectSocketTrace();
    m_intervalStart = Simulator::Now();
    m_sampleEvent =
        Simulator::Schedule(m_monitorInterval, &TcpHarpCubicV7::SampleInterval, this);
}

void
TcpHarpCubicV7::ConnectSocketTrace()
{
    for (NodeList::Iterator nodeIt = NodeList::Begin();
         nodeIt != NodeList::End() && !m_traceConnected;
         ++nodeIt)
    {
        Ptr<TcpL4Protocol> tcp = (*nodeIt)->GetObject<TcpL4Protocol>();
        if (!tcp)
        {
            continue;
        }

        ObjectVectorValue sockets;
        tcp->GetAttribute("SocketList", sockets);
        for (uint32_t i = 0; i < sockets.GetN(); ++i)
        {
            Ptr<TcpSocketBase> socket = DynamicCast<TcpSocketBase>(sockets.Get(i));
            if (!socket)
            {
                continue;
            }

            PointerValue congestionOps;
            socket->GetAttribute("CongestionOps", congestionOps);
            Ptr<TcpHarpCubicV7> v7 =
                DynamicCast<TcpHarpCubicV7>(congestionOps.GetObject());
            if (PeekPointer(v7) != this)
            {
                continue;
            }

            m_socket = socket;
            m_traceConnected = socket->TraceConnectWithoutContext(
                "Tx",
                MakeCallback(&TcpHarpCubicV7::TxTrace, this));
            break;
        }
    }

    NS_ABORT_MSG_IF(!m_traceConnected,
                    "TcpHarpCubicV7 could not connect to its TCP Tx trace");
}

void
TcpHarpCubicV7::TxTrace(Ptr<const Packet> packet,
                        const TcpHeader& header,
                        Ptr<const TcpSocketBase> socket)
{
    (void)header;
    (void)socket;
    m_bytesSent += packet->GetSize();
}

void
TcpHarpCubicV7::SampleInterval()
{
    if (!m_tcb)
    {
        return;
    }

    const Time now = Simulator::Now();
    const double duration = (now - m_intervalStart).GetSeconds();
    const double segmentSize = static_cast<double>(m_tcb->m_segmentSize);
    const double sentPackets = segmentSize > 0.0 ? m_bytesSent / segmentSize : 0.0;
    const double ackedPackets = segmentSize > 0.0 ? m_bytesAcked / segmentSize : 0.0;
    const double sendRatePps = duration > 0.0 ? sentPackets / duration : 0.0;
    const double throughputPps = duration > 0.0 ? ackedPackets / duration : 0.0;
    const double sendingRatio =
        throughputPps > 0.0 && sendRatePps < 1000.0 * throughputPps
            ? sendRatePps / throughputPps
            : 1.0;

    double averageRtt = 0.0;
    double latencyGradient = 0.0;
    if (!m_rttSamples.empty())
    {
        averageRtt =
            std::accumulate(m_rttSamples.begin(), m_rttSamples.end(), 0.0) /
            m_rttSamples.size();
        const std::size_t half = m_rttSamples.size() / 2;
        if (half > 0)
        {
            const double firstMean =
                std::accumulate(m_rttSamples.begin(), m_rttSamples.begin() + half, 0.0) /
                half;
            const std::size_t secondCount = m_rttSamples.size() - half;
            const double secondMean =
                std::accumulate(m_rttSamples.begin() + half, m_rttSamples.end(), 0.0) /
                secondCount;
            latencyGradient =
                duration > 0.0 ? (secondMean - firstMean) / duration : 0.0;
        }
        if (m_minAverageRtt == 0.0 || averageRtt < m_minAverageRtt)
        {
            m_minAverageRtt = averageRtt;
        }
    }

    const double latencyRatio = m_minAverageRtt > 0.0 && averageRtt > 0.0
                                    ? averageRtt / m_minAverageRtt
                                    : 1.0;
    const double currentLossRate =
        sentPackets > 0.0 ? static_cast<double>(m_senderLossEvents) / sentPackets : 0.0;
    const bool validInterval =
        sentPackets > 0.0 && throughputPps > 0.0 && !m_rttSamples.empty();

    if (!m_rttSamples.empty())
    {
        const double intervalMinRtt =
            *std::min_element(m_rttSamples.begin(), m_rttSamples.end());
        if (m_minRttSeconds == 0.0 || intervalMinRtt < m_minRttSeconds)
        {
            m_minRttSeconds = intervalMinRtt;
        }
    }
    if (validInterval)
    {
        m_deliveryRateHistory.emplace_back(now.GetSeconds(), throughputPps);
    }
    const double historyStart = now.GetSeconds() - m_bandwidthWindow.GetSeconds();
    while (!m_deliveryRateHistory.empty() &&
           m_deliveryRateHistory.front().first < historyStart)
    {
        m_deliveryRateHistory.pop_front();
    }

    m_estimatedBandwidthPps = EstimateBandwidthPps();
    const bool estimatorReady = m_deliveryRateHistory.size() >= m_warmupIntervals &&
                                m_estimatedBandwidthPps > 0.0 && m_minRttSeconds > 0.0;
    m_latestV2FeatureValid = validInterval && estimatorReady;

    const double cwndBytes = m_tcb->m_cWnd;
    const double inFlightBytes = m_tcb->m_bytesInFlight;
    const double cwndPackets = segmentSize > 0.0 ? cwndBytes / segmentSize : 0.0;
    const double inFlightPackets =
        segmentSize > 0.0 ? inFlightBytes / segmentSize : 0.0;
    m_estimatedBdpPackets = m_estimatedBandwidthPps * m_minRttSeconds;
    const double normalizedThroughput =
        m_estimatedBandwidthPps > 0.0 ? throughputPps / m_estimatedBandwidthPps : 0.0;
    const double cwndBdpRatio = cwndPackets / std::max(m_estimatedBdpPackets, 1.0);
    const double inFlightCwndRatio =
        inFlightPackets / std::max(cwndPackets, 1.0);

    m_latestV2Features = {{static_cast<float>(sendingRatio),
                           static_cast<float>(normalizedThroughput),
                           static_cast<float>(latencyGradient),
                           static_cast<float>(latencyRatio),
                           static_cast<float>(m_lossHistory),
                           static_cast<float>(cwndBdpRatio),
                           static_cast<float>(inFlightCwndRatio)}};

    m_lossHistory = 0.8 * m_lossHistory + 0.2 * currentLossRate;
    m_intervalStart = now;
    m_bytesSent = 0;
    m_bytesAcked = 0;
    m_senderLossEvents = 0;
    m_rttSamples.clear();
    m_sampleEvent =
        Simulator::Schedule(m_monitorInterval, &TcpHarpCubicV7::SampleInterval, this);
}

double
TcpHarpCubicV7::EstimateBandwidthPps() const
{
    if (m_deliveryRateHistory.empty())
    {
        return 0.0;
    }

    std::vector<double> rates;
    rates.reserve(m_deliveryRateHistory.size());
    for (const auto& sample : m_deliveryRateHistory)
    {
        rates.push_back(sample.second);
    }
    std::sort(rates.begin(), rates.end());
    const std::size_t index =
        std::min(rates.size() - 1,
                 static_cast<std::size_t>(
                     std::ceil(m_bandwidthQuantile * rates.size())) -
                     1);
    return rates[index];
}

} // namespace ns3
