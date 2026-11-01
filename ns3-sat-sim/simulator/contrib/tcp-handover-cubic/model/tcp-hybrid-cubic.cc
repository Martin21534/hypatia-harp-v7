/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "tcp-hybrid-cubic.h"

#include "hybrid-cubic-model-weights.h"
#include "hybrid-cubic-v2-model-weights.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/object-vector.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/nstime.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TcpHybridCubic");
NS_OBJECT_ENSURE_REGISTERED (TcpMlCubic);
NS_OBJECT_ENSURE_REGISTERED (TcpOracleCubic);
NS_OBJECT_ENSURE_REGISTERED (TcpCubicBeta95);

TypeId
TcpMlCubic::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpMlCubic")
    .SetParent<TcpCubic> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpMlCubic> ()
    .AddAttribute ("MonitorInterval",
                   "Feature sampling interval used by the trained classifier.",
                   TimeValue (MilliSeconds (100)),
                   MakeTimeAccessor (&TcpMlCubic::m_monitorInterval),
                   MakeTimeChecker ())
    .AddAttribute ("WarmupIntervals",
                   "Valid intervals required before ML can alter CUBIC.",
                   UintegerValue (5),
                   MakeUintegerAccessor (&TcpMlCubic::m_warmupIntervals),
                   MakeUintegerChecker<uint32_t> (1))
    .AddAttribute ("AssistEnabled",
                   "Allow classifier decisions to alter the CUBIC loss response.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpMlCubic::m_assistEnabled),
                   MakeBooleanChecker ())
    .AddAttribute ("UseV2Model",
                   "Use the scale-normalized v2-A model for loss decisions.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TcpMlCubic::m_useV2Model),
                   MakeBooleanChecker ())
    .AddAttribute ("GrowthTraceEnabled",
                   "Retain every IncreaseWindow event for diagnostic output.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&TcpMlCubic::m_growthTraceEnabled),
                   MakeBooleanChecker ())
    .AddAttribute ("RandomThreshold",
                   "Maximum congestion probability that permits random-loss assistance.",
                   DoubleValue (0.010),
                   MakeDoubleAccessor (&TcpMlCubic::m_randomThreshold),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("RandomBeta",
                   "Multiplicative decrease factor for confidently random loss.",
                   DoubleValue (0.95),
                   MakeDoubleAccessor (&TcpMlCubic::m_randomBeta),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("V2BandwidthWindow",
                   "Sender-side delivery-rate history used by passive v2 telemetry.",
                   TimeValue (Seconds (5)),
                   MakeTimeAccessor (&TcpMlCubic::m_bandwidthWindow),
                   MakeTimeChecker ())
    .AddAttribute ("V2BandwidthQuantile",
                   "High delivery-rate quantile used as the v2 bandwidth estimate.",
                   DoubleValue (0.90),
                   MakeDoubleAccessor (&TcpMlCubic::m_bandwidthQuantile),
                   MakeDoubleChecker<double> (0.5, 1.0))
    .AddTraceSource ("CongestionProbability",
                     "Classifier probability that the loss is congestion-driven.",
                     MakeTraceSourceAccessor (&TcpMlCubic::m_congestionProbability),
                     "ns3::TracedValueCallback::Double")
    .AddTraceSource ("SelectedBeta",
                     "Reduction factor selected for the current CUBIC loss event.",
                     MakeTraceSourceAccessor (&TcpMlCubic::m_selectedBeta),
                     "ns3::TracedValueCallback::Double")
    .AddTraceSource ("RandomAssist",
                     "One when high-confidence random-loss assistance is active.",
                     MakeTraceSourceAccessor (&TcpMlCubic::m_randomAssist),
                     "ns3::TracedValueCallback::Uint32");
  return tid;
}

TcpMlCubic::TcpMlCubic ()
  : TcpCubic ()
{
}

TcpMlCubic::TcpMlCubic (const TcpMlCubic &other)
  : TcpCubic (other),
    m_monitorInterval (other.m_monitorInterval),
    m_warmupIntervals (other.m_warmupIntervals),
    m_assistEnabled (other.m_assistEnabled),
    m_useV2Model (other.m_useV2Model),
    m_growthTraceEnabled (other.m_growthTraceEnabled),
    m_randomThreshold (other.m_randomThreshold),
    m_randomBeta (other.m_randomBeta),
    m_bandwidthWindow (other.m_bandwidthWindow),
    m_bandwidthQuantile (other.m_bandwidthQuantile)
{
}

TcpMlCubic::~TcpMlCubic ()
{
  if (m_sampleEvent.IsPending ())
    {
      m_sampleEvent.Cancel ();
    }
}

std::string
TcpMlCubic::GetName () const
{
  return m_useV2Model ? "TcpMlCubicV2" : "TcpMlCubic";
}

void
TcpMlCubic::Init (Ptr<TcpSocketState> tcb)
{
  m_tcb = tcb;
  TcpCubic::Init (tcb);
}

void
TcpMlCubic::PktsAcked (Ptr<TcpSocketState> tcb,
                       uint32_t segmentsAcked,
                       const Time &rtt)
{
  TcpCubic::PktsAcked (tcb, segmentsAcked, rtt);
  EnsureStarted (tcb);
  m_bytesAcked += static_cast<uint64_t> (segmentsAcked) * tcb->m_segmentSize;
  if (rtt.IsStrictlyPositive ())
    {
      m_rttSamples.push_back (rtt.GetSeconds ());
    }
}

void
TcpMlCubic::IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
  m_maxSegmentsAcked = std::max (m_maxSegmentsAcked, segmentsAcked);
  m_segmentsAckedGt10 += segmentsAcked > 10;
  m_segmentsAckedGt50 += segmentsAcked > 50;
  m_segmentsAckedGt100 += segmentsAcked > 100;

  if (!m_growthTraceEnabled)
    {
      TcpCubic::IncreaseWindow (tcb, segmentsAcked);
      return;
    }

  const double segmentSize = static_cast<double> (tcb->m_segmentSize);
  HybridCubicGrowthRecord record;
  record.timeSeconds = Simulator::Now ().GetSeconds ();
  record.ackNumber = tcb->m_lastAckedSeq.GetValue ();
  record.segmentsAcked = segmentsAcked;
  record.cwndBeforePackets = tcb->m_cWnd / segmentSize;
  record.ssThreshPackets = tcb->m_ssThresh / segmentSize;
  record.bytesInFlightPackets = tcb->m_bytesInFlight / segmentSize;
  record.congestionState = tcb->m_congState;
  TcpCubic::IncreaseWindow (tcb, segmentsAcked);
  record.cwndAfterPackets = tcb->m_cWnd / segmentSize;
  m_growthRecords.push_back (record);
}

Ptr<TcpCongestionOps>
TcpMlCubic::Fork ()
{
  return CopyObject<TcpMlCubic> (this);
}

uint64_t
TcpMlCubic::GetLossDecisions () const
{
  return m_lossDecisions;
}

uint64_t
TcpMlCubic::GetRandomAssists () const
{
  return m_randomAssists;
}

uint32_t
TcpMlCubic::GetMaxSegmentsAcked () const
{
  return m_maxSegmentsAcked;
}

uint64_t
TcpMlCubic::GetSegmentsAckedGt10Count () const
{
  return m_segmentsAckedGt10;
}

uint64_t
TcpMlCubic::GetSegmentsAckedGt50Count () const
{
  return m_segmentsAckedGt50;
}

uint64_t
TcpMlCubic::GetSegmentsAckedGt100Count () const
{
  return m_segmentsAckedGt100;
}

void
TcpMlCubic::NotifyQueueDrop ()
{
  ++m_queueDrops;
  ++m_intervalQueueDrops;
}

void
TcpMlCubic::NotifyRandomDrop ()
{
  ++m_randomDrops;
  ++m_intervalRandomDrops;
}

const std::vector<HybridCubicDecisionRecord> &
TcpMlCubic::GetDecisionRecords () const
{
  return m_decisionRecords;
}

const std::vector<HybridCubicGrowthRecord> &
TcpMlCubic::GetGrowthRecords () const
{
  return m_growthRecords;
}

const std::vector<HybridCubicV2IntervalRecord> &
TcpMlCubic::GetV2IntervalRecords () const
{
  return m_v2IntervalRecords;
}

void
TcpMlCubic::EnsureStarted (Ptr<TcpSocketState> tcb)
{
  m_tcb = tcb;
  if (m_started)
    {
      return;
    }

  NS_ABORT_MSG_IF (!m_monitorInterval.IsStrictlyPositive (),
                   "TcpMlCubic MonitorInterval must be positive");
  m_started = true;
  ConnectSocketTrace ();
  m_intervalStart = Simulator::Now ();
  m_sampleEvent = Simulator::Schedule (m_monitorInterval,
                                       &TcpMlCubic::SampleInterval,
                                       this);
}

void
TcpMlCubic::ConnectSocketTrace ()
{
  for (NodeList::Iterator nodeIt = NodeList::Begin ();
       nodeIt != NodeList::End () && !m_traceConnected;
       ++nodeIt)
    {
      Ptr<TcpL4Protocol> tcp = (*nodeIt)->GetObject<TcpL4Protocol> ();
      if (!tcp)
        {
          continue;
        }

      ObjectVectorValue sockets;
      tcp->GetAttribute ("SocketList", sockets);
      for (uint32_t i = 0; i < sockets.GetN (); ++i)
        {
          Ptr<TcpSocketBase> socket = DynamicCast<TcpSocketBase> (sockets.Get (i));
          if (!socket)
            {
              continue;
            }

          PointerValue congestionOps;
          socket->GetAttribute ("CongestionOps", congestionOps);
          Ptr<TcpMlCubic> mlCubic = DynamicCast<TcpMlCubic> (congestionOps.GetObject ());
          if (PeekPointer (mlCubic) != this)
            {
              continue;
            }

          m_socket = socket;
          m_traceConnected = socket->TraceConnectWithoutContext (
            "Tx", MakeCallback (&TcpMlCubic::TxTrace, this));
          break;
        }
    }

  NS_ABORT_MSG_IF (!m_traceConnected,
                   "TcpMlCubic could not connect to its TCP Tx trace");
}

void
TcpMlCubic::TxTrace (Ptr<const Packet> packet,
                     const TcpHeader &header,
                     Ptr<const TcpSocketBase> socket)
{
  (void) header;
  (void) socket;
  m_bytesSent += packet->GetSize ();
}

void
TcpMlCubic::SampleInterval ()
{
  if (!m_tcb)
    {
      return;
    }

  const Time now = Simulator::Now ();
  const double duration = (now - m_intervalStart).GetSeconds ();
  const double segmentSize = static_cast<double> (m_tcb->m_segmentSize);
  const double sentPackets = segmentSize > 0.0 ? m_bytesSent / segmentSize : 0.0;
  const double ackedPackets = segmentSize > 0.0 ? m_bytesAcked / segmentSize : 0.0;
  const double sendRatePps = duration > 0.0 ? sentPackets / duration : 0.0;
  const double throughputPps = duration > 0.0 ? ackedPackets / duration : 0.0;
  const double sendingRatio = throughputPps > 0.0 && sendRatePps < 1000.0 * throughputPps
                                ? sendRatePps / throughputPps
                                : 1.0;

  double averageRtt = 0.0;
  double latencyGradient = 0.0;
  if (!m_rttSamples.empty ())
    {
      averageRtt = std::accumulate (m_rttSamples.begin (), m_rttSamples.end (), 0.0)
                   / m_rttSamples.size ();
      const std::size_t half = m_rttSamples.size () / 2;
      if (half > 0)
        {
          const double firstMean = std::accumulate (m_rttSamples.begin (),
                                                    m_rttSamples.begin () + half,
                                                    0.0) / half;
          const std::size_t secondCount = m_rttSamples.size () - half;
          const double secondMean = std::accumulate (m_rttSamples.begin () + half,
                                                     m_rttSamples.end (),
                                                     0.0) / secondCount;
          latencyGradient = duration > 0.0 ? (secondMean - firstMean) / duration : 0.0;
        }
      if (m_minAverageRtt == 0.0 || averageRtt < m_minAverageRtt)
        {
          m_minAverageRtt = averageRtt;
        }
    }

  const double latencyRatio = m_minAverageRtt > 0.0 && averageRtt > 0.0
                                ? averageRtt / m_minAverageRtt
                                : 1.0;
  const double currentLossRate = sentPackets > 0.0
                                   ? static_cast<double> (m_senderLossEvents) / sentPackets
                                   : 0.0;
  const bool validInterval = sentPackets > 0.0 && throughputPps > 0.0
                             && !m_rttSamples.empty ();

  if (!m_rttSamples.empty ())
    {
      const double intervalMinRtt = *std::min_element (m_rttSamples.begin (),
                                                       m_rttSamples.end ());
      if (m_minRttSeconds == 0.0 || intervalMinRtt < m_minRttSeconds)
        {
          m_minRttSeconds = intervalMinRtt;
        }
    }
  if (validInterval)
    {
      m_deliveryRateHistory.emplace_back (now.GetSeconds (), throughputPps);
    }
  const double historyStart = now.GetSeconds () - m_bandwidthWindow.GetSeconds ();
  while (!m_deliveryRateHistory.empty ()
         && m_deliveryRateHistory.front ().first < historyStart)
    {
      m_deliveryRateHistory.pop_front ();
    }
  m_estimatedBandwidthPps = EstimateBandwidthPps ();
  m_v2EstimatorReady = m_deliveryRateHistory.size () >= m_warmupIntervals
                       && m_estimatedBandwidthPps > 0.0 && m_minRttSeconds > 0.0;
  m_latestV2FeatureValid = validInterval && m_v2EstimatorReady;

  const double cwndBytes = m_tcb->m_cWnd;
  const double inFlightBytes = m_tcb->m_bytesInFlight;
  const double cwndPackets = segmentSize > 0.0 ? cwndBytes / segmentSize : 0.0;
  const double inFlightPackets = segmentSize > 0.0
                                   ? inFlightBytes / segmentSize
                                   : 0.0;
  m_estimatedBdpPackets = m_estimatedBandwidthPps * m_minRttSeconds;
  const double normalizedThroughput = m_estimatedBandwidthPps > 0.0
                                        ? throughputPps / m_estimatedBandwidthPps
                                        : 0.0;
  const double cwndBdpRatio = cwndPackets / std::max (m_estimatedBdpPackets, 1.0);
  const double inFlightCwndRatio = inFlightPackets / std::max (cwndPackets, 1.0);

  m_latestFeatures = {{static_cast<float> (sendingRatio),
                       static_cast<float> (throughputPps),
                       static_cast<float> (latencyGradient),
                       static_cast<float> (latencyRatio),
                       static_cast<float> (m_lossHistory),
                       static_cast<float> (m_tcb->m_cWnd / segmentSize),
                       static_cast<float> (m_tcb->m_bytesInFlight / segmentSize)}};
  m_latestV2Features = {{static_cast<float> (sendingRatio),
                         static_cast<float> (normalizedThroughput),
                         static_cast<float> (latencyGradient),
                         static_cast<float> (latencyRatio),
                         static_cast<float> (m_lossHistory),
                         static_cast<float> (cwndBdpRatio),
                         static_cast<float> (inFlightCwndRatio)}};

  HybridCubicLossType intervalLossType = HybridCubicLossType::UNKNOWN;
  if (m_intervalQueueDrops > 0 && m_intervalRandomDrops == 0)
    {
      intervalLossType = HybridCubicLossType::CONGESTION;
    }
  else if (m_intervalQueueDrops == 0 && m_intervalRandomDrops > 0)
    {
      intervalLossType = HybridCubicLossType::RANDOM;
    }
  else if (m_intervalQueueDrops > 0 && m_intervalRandomDrops > 0)
    {
      intervalLossType = HybridCubicLossType::MIXED;
    }

  HybridCubicV2IntervalRecord v2Record;
  v2Record.timeSeconds = now.GetSeconds ();
  v2Record.intervalSeconds = duration;
  v2Record.valid = validInterval;
  v2Record.estimatorReady = m_v2EstimatorReady;
  v2Record.lossType = intervalLossType;
  v2Record.features = m_latestV2Features;
  v2Record.deliveryRatePps = throughputPps;
  v2Record.estimatedBandwidthPps = m_estimatedBandwidthPps;
  v2Record.estimatedBdpPackets = m_estimatedBdpPackets;
  v2Record.minRttSeconds = m_minRttSeconds;
  v2Record.cwndPackets = cwndPackets;
  v2Record.bytesInFlightPackets = inFlightPackets;
  v2Record.senderLossEvents = m_senderLossEvents;
  v2Record.queueDrops = m_intervalQueueDrops;
  v2Record.randomDrops = m_intervalRandomDrops;
  m_v2IntervalRecords.push_back (v2Record);
  m_lastAverageRtt = averageRtt;
  if (sentPackets > 0.0 && !m_rttSamples.empty ())
    {
      ++m_validIntervals;
    }

  m_lossHistory = 0.8 * m_lossHistory + 0.2 * currentLossRate;
  m_intervalStart = now;
  m_bytesSent = 0;
  m_bytesAcked = 0;
  m_senderLossEvents = 0;
  m_intervalQueueDrops = 0;
  m_intervalRandomDrops = 0;
  m_rttSamples.clear ();
  m_sampleEvent = Simulator::Schedule (m_monitorInterval,
                                       &TcpMlCubic::SampleInterval,
                                       this);
}

double
TcpMlCubic::EstimateBandwidthPps () const
{
  if (m_deliveryRateHistory.empty ())
    {
      return 0.0;
    }
  std::vector<double> rates;
  rates.reserve (m_deliveryRateHistory.size ());
  for (const auto &sample : m_deliveryRateHistory)
    {
      rates.push_back (sample.second);
    }
  std::sort (rates.begin (), rates.end ());
  const std::size_t index = std::min (
    rates.size () - 1,
    static_cast<std::size_t> (std::ceil (m_bandwidthQuantile * rates.size ())) - 1);
  return rates[index];
}

float
TcpMlCubic::PredictCongestion () const
{
  return PredictCongestionForFeatures (m_latestFeatures);
}

float
TcpMlCubic::PredictV2Congestion () const
{
  return PredictV2CongestionForFeatures (m_latestV2Features);
}

float
TcpMlCubic::PredictCongestionForFeatures (const std::array<float, 7> &features)
{
  std::array<float, FEATURE_COUNT> normalized;
  for (uint32_t i = 0; i < FEATURE_COUNT; ++i)
    {
      const float clipped = std::max (HybridCubicModel::CLIP_LOW[i],
                                      std::min (HybridCubicModel::CLIP_HIGH[i],
                                                features[i]));
      normalized[i] = (clipped - HybridCubicModel::MEAN[i])
                      / HybridCubicModel::STD[i];
    }

  std::array<float, 32> hidden1;
  for (uint32_t output = 0; output < hidden1.size (); ++output)
    {
      float value = HybridCubicModel::B1[output];
      for (uint32_t input = 0; input < FEATURE_COUNT; ++input)
        {
          value += normalized[input] * HybridCubicModel::W1[input * hidden1.size () + output];
        }
      hidden1[output] = std::tanh (value);
    }

  std::array<float, 16> hidden2;
  for (uint32_t output = 0; output < hidden2.size (); ++output)
    {
      float value = HybridCubicModel::B2[output];
      for (uint32_t input = 0; input < hidden1.size (); ++input)
        {
          value += hidden1[input] * HybridCubicModel::W2[input * hidden2.size () + output];
        }
      hidden2[output] = std::tanh (value);
    }

  float logit = HybridCubicModel::B3[0];
  for (uint32_t input = 0; input < hidden2.size (); ++input)
    {
      logit += hidden2[input] * HybridCubicModel::W3[input];
    }
  return 1.0f / (1.0f + std::exp (-logit));
}

float
TcpMlCubic::PredictV2CongestionForFeatures (const std::array<float, 7> &features)
{
  std::array<float, FEATURE_COUNT> normalized;
  for (uint32_t i = 0; i < FEATURE_COUNT; ++i)
    {
      const float clipped = std::max (HybridCubicV2Model::CLIP_LOW[i],
                                      std::min (HybridCubicV2Model::CLIP_HIGH[i],
                                                features[i]));
      normalized[i] = (clipped - HybridCubicV2Model::MEAN[i])
                      / HybridCubicV2Model::STD[i];
    }

  std::array<float, 32> hidden1;
  for (uint32_t output = 0; output < hidden1.size (); ++output)
    {
      float value = HybridCubicV2Model::B1[output];
      for (uint32_t input = 0; input < FEATURE_COUNT; ++input)
        {
          value += normalized[input]
                   * HybridCubicV2Model::W1[input * hidden1.size () + output];
        }
      hidden1[output] = std::tanh (value);
    }

  std::array<float, 16> hidden2;
  for (uint32_t output = 0; output < hidden2.size (); ++output)
    {
      float value = HybridCubicV2Model::B2[output];
      for (uint32_t input = 0; input < hidden1.size (); ++input)
        {
          value += hidden1[input]
                   * HybridCubicV2Model::W2[input * hidden2.size () + output];
        }
      hidden2[output] = std::tanh (value);
    }

  float logit = HybridCubicV2Model::B3[0];
  for (uint32_t input = 0; input < hidden2.size (); ++input)
    {
      logit += hidden2[input] * HybridCubicV2Model::W3[input];
    }
  return 1.0f / (1.0f + std::exp (-logit));
}

double
TcpMlCubic::GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                    uint32_t bytesInFlight)
{
  const double fallback = TcpCubic::GetLossReductionFactor (tcb, bytesInFlight);
  double selected = fallback;
  uint32_t assisted = 0;
  const bool featureValid = m_useV2Model ? m_latestV2FeatureValid : m_validIntervals > 0;
  const bool controlReady = m_useV2Model
                              ? m_latestV2FeatureValid
                              : m_validIntervals >= m_warmupIntervals;
  const double probability = featureValid
                               ? (m_useV2Model ? PredictV2Congestion ()
                                               : PredictCongestion ())
                               : std::numeric_limits<double>::quiet_NaN ();

  if (featureValid)
    {
      m_congestionProbability = probability;
      if (m_assistEnabled && controlReady && probability <= m_randomThreshold)
        {
          selected = m_randomBeta;
          assisted = 1;
        }
    }
  else
    {
      m_congestionProbability = 1.0;
    }

  HybridCubicLossType lossType = HybridCubicLossType::UNKNOWN;
  if (m_queueDrops > 0 && m_randomDrops == 0)
    {
      lossType = HybridCubicLossType::CONGESTION;
    }
  else if (m_queueDrops == 0 && m_randomDrops > 0)
    {
      lossType = HybridCubicLossType::RANDOM;
    }
  else if (m_queueDrops > 0 && m_randomDrops > 0)
    {
      lossType = HybridCubicLossType::MIXED;
    }

  const double segmentSize = static_cast<double> (tcb->m_segmentSize);
  const double cwndBefore = tcb->GetCwndInSegments ();
  HybridCubicDecisionRecord record;
  record.timeSeconds = Simulator::Now ().GetSeconds ();
  record.lossType = lossType;
  record.congestionProbability = probability;
  record.featureValid = featureValid;
  record.controlReady = controlReady;
  record.assisted = assisted != 0;
  record.selectedBeta = selected;
  record.cwndBeforePackets = cwndBefore;
  record.cwndAfterTargetPackets = std::max (static_cast<uint32_t> (cwndBefore * selected), 2U);
  record.bytesInFlightPackets = segmentSize > 0.0 ? bytesInFlight / segmentSize : 0.0;
  record.averageRttSeconds = m_lastAverageRtt;
  record.features = m_latestFeatures;
  record.v2Features = m_latestV2Features;
  record.v2EstimatorReady = m_v2EstimatorReady;
  record.estimatedBandwidthPps = m_estimatedBandwidthPps;
  record.estimatedBdpPackets = m_estimatedBdpPackets;
  record.minRttSeconds = m_minRttSeconds;
  record.queueDrops = m_queueDrops;
  record.randomDrops = m_randomDrops;
  m_decisionRecords.push_back (record);

  m_selectedBeta = selected;
  m_randomAssist = assisted;
  ++m_lossDecisions;
  m_randomAssists += assisted;
  ++m_senderLossEvents;
  m_queueDrops = 0;
  m_randomDrops = 0;
  return selected;
}

TypeId
TcpOracleCubic::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpOracleCubic")
    .SetParent<TcpCubic> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpOracleCubic> ()
    .AddAttribute ("RandomBeta",
                   "Multiplicative decrease factor for simulator-confirmed random loss.",
                   DoubleValue (0.95),
                   MakeDoubleAccessor (&TcpOracleCubic::m_randomBeta),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddTraceSource ("SelectedBeta",
                     "Reduction factor selected for the current CUBIC loss event.",
                     MakeTraceSourceAccessor (&TcpOracleCubic::m_selectedBeta),
                     "ns3::TracedValueCallback::Double")
    .AddTraceSource ("RandomAssist",
                     "One when the oracle identifies random loss without queue loss.",
                     MakeTraceSourceAccessor (&TcpOracleCubic::m_randomAssist),
                     "ns3::TracedValueCallback::Uint32");
  return tid;
}

TcpOracleCubic::TcpOracleCubic ()
  : TcpCubic ()
{
}

TcpOracleCubic::TcpOracleCubic (const TcpOracleCubic &other)
  : TcpCubic (other),
    m_randomBeta (other.m_randomBeta)
{
}

std::string
TcpOracleCubic::GetName () const
{
  return "TcpOracleCubic";
}

Ptr<TcpCongestionOps>
TcpOracleCubic::Fork ()
{
  return CopyObject<TcpOracleCubic> (this);
}

void
TcpOracleCubic::NotifyQueueDrop ()
{
  ++m_queueDrops;
}

void
TcpOracleCubic::NotifyRandomDrop ()
{
  ++m_randomDrops;
}

uint64_t
TcpOracleCubic::GetLossDecisions () const
{
  return m_lossDecisions;
}

uint64_t
TcpOracleCubic::GetRandomAssists () const
{
  return m_randomAssists;
}

double
TcpOracleCubic::GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                        uint32_t bytesInFlight)
{
  const double fallback = TcpCubic::GetLossReductionFactor (tcb, bytesInFlight);
  const bool randomOnly = m_randomDrops > 0 && m_queueDrops == 0;
  const double selected = randomOnly ? m_randomBeta : fallback;
  m_selectedBeta = selected;
  m_randomAssist = randomOnly ? 1 : 0;
  ++m_lossDecisions;
  m_randomAssists += randomOnly ? 1 : 0;
  m_queueDrops = 0;
  m_randomDrops = 0;
  return selected;
}

TypeId
TcpCubicBeta95::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpCubicBeta95")
    .SetParent<TcpCubic> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpCubicBeta95> ();
  return tid;
}

TcpCubicBeta95::TcpCubicBeta95 ()
  : TcpCubic ()
{
}

TcpCubicBeta95::TcpCubicBeta95 (const TcpCubicBeta95 &other)
  : TcpCubic (other)
{
}

std::string
TcpCubicBeta95::GetName () const
{
  return "TcpCubicBeta95";
}

Ptr<TcpCongestionOps>
TcpCubicBeta95::Fork ()
{
  return CopyObject<TcpCubicBeta95> (this);
}

double
TcpCubicBeta95::GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                        uint32_t bytesInFlight)
{
  (void) tcb;
  (void) bytesInFlight;
  return 0.95;
}

} // namespace ns3
