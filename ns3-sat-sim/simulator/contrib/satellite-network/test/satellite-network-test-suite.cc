/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "end-to-end-special-test.h"
#include "end-to-end-test.h"
#include "ground-station-info-test.h"
#include "ns3/arbiter-single-forward.h"
#include "manual-two-sat-two-gs-test.h"
#include "ns3/basic-simulation.h"
#include "ns3/boolean.h"
#include "ns3/gsl-scheduled-error-model.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-header.h"
#include "ns3/packet.h"
#include "ns3/ppp-header.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-harp.h"
#include "ns3/tcp-option-ts.h"
#include "ns3/test.h"
#include "satellite-info-test.h"

#include <array>

using namespace ns3;

class GslScheduledErrorModelTestCase : public TestCase {
public:
  GslScheduledErrorModelTestCase()
      : TestCase("Scheduled GSL loss is time-windowed and flow-filtered") {}

private:
  Ptr<Packet> MakePacket(Ipv4Address source, Ipv4Address target) {
    Ptr<Packet> packet = Create<Packet>(32);

    Ipv4Header ipv4;
    ipv4.SetSource(source);
    ipv4.SetDestination(target);
    ipv4.SetProtocol(6);
    ipv4.SetPayloadSize(packet->GetSize());
    packet->AddHeader(ipv4);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    packet->AddHeader(ppp);
    return packet;
  }

  void CheckCorrupt(Ptr<GslScheduledErrorModel> model,
                    Ptr<Packet> packet,
                    bool expected,
                    const std::string& message) {
    NS_TEST_ASSERT_MSG_EQ(model->IsCorrupt(packet), expected, message);
  }

  void OnDrop(Ptr<const Packet> packet, uint32_t windowIndex) {
    NS_TEST_ASSERT_MSG_NE(packet, nullptr,
                          "Drop callback must receive the dropped packet");
    NS_TEST_ASSERT_MSG_EQ(windowIndex, 0,
                          "Drop callback must identify the active window");
    ++m_callbackCount;
  }

  void DoRun() override {
    const Ipv4Address source("10.0.0.1");
    const Ipv4Address target("10.0.0.2");

    Ptr<GslScheduledErrorModel> model =
        CreateObject<GslScheduledErrorModel>();
    model->SetFlow(source, target);
    model->SetWindows({{1000000000, 2000000000, 1.0, "test-window"}});
    model->SetDropCallback(
        MakeCallback(&GslScheduledErrorModelTestCase::OnDrop, this));

    Ptr<Packet> matching = MakePacket(source, target);
    Ptr<Packet> reverse = MakePacket(target, source);

    CheckCorrupt(model, matching, false,
                 "Matching flow must pass before the loss window");
    Simulator::Schedule(Seconds(1.1),
                        &GslScheduledErrorModelTestCase::CheckCorrupt,
                        this,
                        model,
                        matching,
                        true,
                        "Matching flow must drop inside a rate-one window");
    Simulator::Schedule(Seconds(1.2),
                        &GslScheduledErrorModelTestCase::CheckCorrupt,
                        this,
                        model,
                        reverse,
                        false,
                        "Reverse flow must not be eligible for loss");
    Simulator::Schedule(Seconds(2.0),
                        &GslScheduledErrorModelTestCase::CheckCorrupt,
                        this,
                        model,
                        matching,
                        false,
                        "The loss window end must be exclusive");
    Simulator::Run();

    NS_TEST_ASSERT_MSG_EQ(model->GetEligiblePacketCount(), 1,
                          "Only the matching packet inside the window is eligible");
    NS_TEST_ASSERT_MSG_EQ(model->GetDroppedPacketCount(), 1,
                          "The rate-one window must drop its eligible packet");
    NS_TEST_ASSERT_MSG_EQ(m_callbackCount, 1,
                          "Exactly one drop callback must be emitted");
    Simulator::Destroy();
  }

  uint32_t m_callbackCount{0};
};

class HarpV5RouteMarkTestCase : public TestCase {
public:
  HarpV5RouteMarkTestCase()
      : TestCase("HARP V5 route marking preserves TCP payload and is idempotent") {}

private:
  Ptr<Packet> MakeTcpPacket(const uint8_t* payload, uint32_t payloadSize) {
    Ptr<Packet> packet = Create<Packet>(payload, payloadSize);
    TcpHeader header;
    header.SetSourcePort(1000);
    header.SetDestinationPort(2000);
    header.SetSequenceNumber(SequenceNumber32(1234));
    header.SetAckNumber(SequenceNumber32(5678));
    header.SetFlags(TcpHeader::ACK);

    Ptr<TcpOptionTS> timestamp = CreateObject<TcpOptionTS>();
    timestamp->SetTimestamp(10);
    timestamp->SetEcho(9);
    header.AppendOption(timestamp);
    packet->AddHeader(header);
    return packet;
  }

  uint32_t CountHarpSignal(const TcpHeader& header,
                           TcpOptionHarp::Signal signal,
                           uint16_t epoch) {
    uint32_t count = 0;
    for (const auto& option : header.GetOptionList()) {
      if (option->GetKind() != TcpOption::HARP) {
        continue;
      }
      const auto harp = DynamicCast<const TcpOptionHarp>(option);
      if (harp && harp->GetSignal() == signal && harp->GetEpoch() == epoch) {
        ++count;
      }
    }
    return count;
  }

  void AssertPayload(Ptr<Packet> packet,
                     const std::array<uint8_t, 16>& expected,
                     TcpOptionHarp::Signal signal,
                     uint16_t epoch) {
    TcpHeader decoded;
    packet->RemoveHeader(decoded);
    NS_TEST_ASSERT_MSG_EQ(packet->GetSize(), expected.size(),
                          "Route mark must not change TCP payload size");

    std::array<uint8_t, 16> actual{};
    packet->CopyData(actual.data(), actual.size());
    NS_TEST_ASSERT_MSG_EQ(actual == expected, true,
                          "Route mark must not change TCP payload bytes");
    NS_TEST_ASSERT_MSG_EQ(CountHarpSignal(decoded, signal, epoch), 1,
                          "Exactly one requested HARP mark must be visible");
    packet->AddHeader(decoded);
  }

  void DoRun() override {
    NodeContainer nodes;
    nodes.Create(2);
    InternetStackHelper internet;
    internet.Install(nodes);

    const uint32_t sourceId = nodes.Get(0)->GetId();
    const uint32_t targetId = nodes.Get(1)->GetId();
    std::vector<std::tuple<int32_t, int32_t, int32_t>> nextHops(
        targetId + 1, std::make_tuple(0, 0, 0));
    nextHops[targetId] = std::make_tuple(1, 0, 0);

    Ptr<ArbiterSingleForward> arbiter =
        CreateObject<ArbiterSingleForward>(nodes.Get(0), nodes, nextHops);
    arbiter->SetAttribute("EnableHarpV5PacketMarking", BooleanValue(true));

    Ipv4Header ipHeader;
    ipHeader.SetSource(Ipv4Address("10.0.0.1"));
    ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
    ipHeader.SetProtocol(6);

    const std::array<uint8_t, 16> payload = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    // Establish the old first hop without creating a handover signal.
    Ptr<Packet> baseline = MakeTcpPacket(payload.data(), payload.size());
    arbiter->TopologySatelliteNetworkDecide(sourceId, targetId, baseline, ipHeader, false);

    // Change first hop and exercise the real serialized-header marking path.
    arbiter->SetSingleForwardState(targetId, 2, 0, 0);
    Ptr<Packet> marked = MakeTcpPacket(payload.data(), payload.size());
    arbiter->TopologySatelliteNetworkDecide(sourceId, targetId, marked, ipHeader, false);
    AssertPayload(marked, payload, TcpOptionHarp::Signal::DATA_MARK, 1);

    // Re-inspecting the same packet must not append a duplicate mark.
    arbiter->TopologySatelliteNetworkDecide(sourceId, targetId, marked, ipHeader, false);
    AssertPayload(marked, payload, TcpOptionHarp::Signal::DATA_MARK, 1);

    Simulator::Destroy();
  }
};

class SatelliteNetworkTestSuite : public TestSuite {
public:
  SatelliteNetworkTestSuite() : TestSuite("satellite-network", Type::UNIT) {

    // Running it complete with reading in files etc.
    AddTestCase(new EndToEndTestCase, TestCase::Duration::QUICK);
    AddTestCase(new EndToEndSpecialTestCase, TestCase::Duration::QUICK);

    // Running it by creating every component manually (not using
    // satellite-network.cc/h)
    AddTestCase(new ManualTwoSatTwoGsFirstTest, TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsDifferentPropSpeedTest,
                TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsUpSharedTest, TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsUpSharedUdpTest,
                TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsDownBothFullTest,
                TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsChangingForwardingTest,
                TestCase::Duration::QUICK);
    AddTestCase(new ManualTwoSatTwoGsChangingRateTest,
                TestCase::Duration::QUICK);

    // Simple info wrappers
    AddTestCase(new SatelliteInfoTestCase, TestCase::Duration::QUICK);
    AddTestCase(new GroundStationInfoTestCase, TestCase::Duration::QUICK);
    AddTestCase(new GslScheduledErrorModelTestCase, TestCase::Duration::QUICK);
    AddTestCase(new HarpV5RouteMarkTestCase, TestCase::Duration::QUICK);
  }
};
static SatelliteNetworkTestSuite SatelliteNetworkTestSuite;
