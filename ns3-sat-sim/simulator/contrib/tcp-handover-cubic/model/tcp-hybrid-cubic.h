/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef TCP_HYBRID_CUBIC_H
#define TCP_HYBRID_CUBIC_H

#include "ns3/event-id.h"
#include "ns3/tcp-cubic.h"
#include "ns3/tcp-header.h"
#include "ns3/traced-value.h"

#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace ns3 {

class TcpSocketBase;

enum class HybridCubicLossType : uint8_t
{
  UNKNOWN,
  RANDOM,
  CONGESTION,
  MIXED
};

struct HybridCubicDecisionRecord
{
  double timeSeconds {0.0};
  HybridCubicLossType lossType {HybridCubicLossType::UNKNOWN};
  double congestionProbability {0.0};
  bool featureValid {false};
  bool controlReady {false};
  bool assisted {false};
  double selectedBeta {0.0};
  double cwndBeforePackets {0.0};
  double cwndAfterTargetPackets {0.0};
  double bytesInFlightPackets {0.0};
  double averageRttSeconds {0.0};
  std::array<float, 7> features {{0.0f}};
  std::array<float, 7> v2Features {{0.0f}};
  bool v2EstimatorReady {false};
  double estimatedBandwidthPps {0.0};
  double estimatedBdpPackets {0.0};
  double minRttSeconds {0.0};
  uint32_t queueDrops {0};
  uint32_t randomDrops {0};
};

struct HybridCubicV2IntervalRecord
{
  double timeSeconds {0.0};
  double intervalSeconds {0.0};
  bool valid {false};
  bool estimatorReady {false};
  HybridCubicLossType lossType {HybridCubicLossType::UNKNOWN};
  std::array<float, 7> features {{0.0f}};
  double deliveryRatePps {0.0};
  double estimatedBandwidthPps {0.0};
  double estimatedBdpPackets {0.0};
  double minRttSeconds {0.0};
  double cwndPackets {0.0};
  double bytesInFlightPackets {0.0};
  uint32_t senderLossEvents {0};
  uint32_t queueDrops {0};
  uint32_t randomDrops {0};
};

struct HybridCubicGrowthRecord
{
  double timeSeconds {0.0};
  uint32_t ackNumber {0};
  uint32_t segmentsAcked {0};
  double cwndBeforePackets {0.0};
  double cwndAfterPackets {0.0};
  double ssThreshPackets {0.0};
  double bytesInFlightPackets {0.0};
  TcpSocketState::TcpCongState_t congestionState {TcpSocketState::CA_OPEN};
};

/**
 * Conservative ML-assisted CUBIC.
 *
 * The embedded classifier distinguishes congestion loss from random loss.
 * Standard CUBIC is retained unless the prediction is confidently random.
 */
class TcpMlCubic : public TcpCubic
{
public:
  static TypeId GetTypeId (void);

  TcpMlCubic ();
  TcpMlCubic (const TcpMlCubic &other);
  ~TcpMlCubic () override;

  std::string GetName () const override;
  void Init (Ptr<TcpSocketState> tcb) override;
  void PktsAcked (Ptr<TcpSocketState> tcb,
                  uint32_t segmentsAcked,
                  const Time &rtt) override;
  void IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;
  Ptr<TcpCongestionOps> Fork () override;
  uint64_t GetLossDecisions () const;
  uint64_t GetRandomAssists () const;
  uint32_t GetMaxSegmentsAcked () const;
  uint64_t GetSegmentsAckedGt10Count () const;
  uint64_t GetSegmentsAckedGt50Count () const;
  uint64_t GetSegmentsAckedGt100Count () const;
  static float PredictCongestionForFeatures (const std::array<float, 7> &features);
  static float PredictV2CongestionForFeatures (const std::array<float, 7> &features);
  void NotifyQueueDrop ();
  void NotifyRandomDrop ();
  const std::vector<HybridCubicDecisionRecord> &GetDecisionRecords () const;
  const std::vector<HybridCubicGrowthRecord> &GetGrowthRecords () const;
  const std::vector<HybridCubicV2IntervalRecord> &GetV2IntervalRecords () const;

protected:
  double GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                 uint32_t bytesInFlight) override;

private:
  static constexpr uint32_t FEATURE_COUNT = 7;

  void EnsureStarted (Ptr<TcpSocketState> tcb);
  void ConnectSocketTrace ();
  void TxTrace (Ptr<const Packet> packet,
                const TcpHeader &header,
                Ptr<const TcpSocketBase> socket);
  void SampleInterval ();
  float PredictCongestion () const;
  float PredictV2Congestion () const;
  double EstimateBandwidthPps () const;

  Time m_monitorInterval {MilliSeconds (100)};
  uint32_t m_warmupIntervals {5};
  bool m_assistEnabled {true};
  bool m_useV2Model {false};
  bool m_growthTraceEnabled {true};
  double m_randomThreshold {0.010};
  double m_randomBeta {0.95};
  Time m_bandwidthWindow {Seconds (5)};
  double m_bandwidthQuantile {0.90};

  Ptr<TcpSocketState> m_tcb;
  Ptr<TcpSocketBase> m_socket;
  EventId m_sampleEvent;
  bool m_started {false};
  bool m_traceConnected {false};
  Time m_intervalStart {Seconds (0)};

  uint64_t m_bytesSent {0};
  uint64_t m_bytesAcked {0};
  uint32_t m_senderLossEvents {0};
  std::vector<double> m_rttSamples;
  double m_minAverageRtt {0.0};
  double m_lastAverageRtt {0.0};
  double m_lossHistory {0.0};
  uint32_t m_validIntervals {0};
  std::array<float, FEATURE_COUNT> m_latestFeatures {{0.0f}};
  std::array<float, FEATURE_COUNT> m_latestV2Features {{0.0f}};
  std::deque<std::pair<double, double>> m_deliveryRateHistory;
  double m_estimatedBandwidthPps {0.0};
  double m_estimatedBdpPackets {0.0};
  double m_minRttSeconds {0.0};
  bool m_v2EstimatorReady {false};
  bool m_latestV2FeatureValid {false};
  uint32_t m_queueDrops {0};
  uint32_t m_randomDrops {0};
  uint32_t m_intervalQueueDrops {0};
  uint32_t m_intervalRandomDrops {0};
  uint64_t m_lossDecisions {0};
  uint64_t m_randomAssists {0};
  uint32_t m_maxSegmentsAcked {0};
  uint64_t m_segmentsAckedGt10 {0};
  uint64_t m_segmentsAckedGt50 {0};
  uint64_t m_segmentsAckedGt100 {0};
  std::vector<HybridCubicDecisionRecord> m_decisionRecords;
  std::vector<HybridCubicGrowthRecord> m_growthRecords;
  std::vector<HybridCubicV2IntervalRecord> m_v2IntervalRecords;

  TracedValue<double> m_congestionProbability {1.0};
  TracedValue<double> m_selectedBeta {0.0};
  TracedValue<uint32_t> m_randomAssist {0};
};

/** CUBIC with perfect simulator drop-cause information. */
class TcpOracleCubic : public TcpCubic
{
public:
  static TypeId GetTypeId (void);

  TcpOracleCubic ();
  TcpOracleCubic (const TcpOracleCubic &other);

  std::string GetName () const override;
  Ptr<TcpCongestionOps> Fork () override;
  void NotifyQueueDrop ();
  void NotifyRandomDrop ();
  uint64_t GetLossDecisions () const;
  uint64_t GetRandomAssists () const;

protected:
  double GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                 uint32_t bytesInFlight) override;

private:
  double m_randomBeta {0.95};
  uint32_t m_queueDrops {0};
  uint32_t m_randomDrops {0};
  uint64_t m_lossDecisions {0};
  uint64_t m_randomAssists {0};
  TracedValue<double> m_selectedBeta {0.0};
  TracedValue<uint32_t> m_randomAssist {0};
};

/** Deliberately aggressive baseline that uses beta=0.95 for every loss. */
class TcpCubicBeta95 : public TcpCubic
{
public:
  static TypeId GetTypeId (void);

  TcpCubicBeta95 ();
  TcpCubicBeta95 (const TcpCubicBeta95 &other);

  std::string GetName () const override;
  Ptr<TcpCongestionOps> Fork () override;

protected:
  double GetLossReductionFactor (Ptr<const TcpSocketState> tcb,
                                 uint32_t bytesInFlight) override;
};

} // namespace ns3

#endif // TCP_HYBRID_CUBIC_H
