/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef GSL_SCHEDULED_ERROR_MODEL_H
#define GSL_SCHEDULED_ERROR_MODEL_H

#include "ns3/callback.h"
#include "ns3/error-model.h"
#include "ns3/ipv4-address.h"
#include "ns3/random-variable-stream.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ns3 {

/**
 * Packet-loss window used by GslScheduledErrorModel.
 *
 * The interval is half-open: startTimeNs <= now < endTimeNs.
 */
struct GslLossWindow
{
    int64_t startTimeNs{0};
    int64_t endTimeNs{0};
    double rate{0.0};
    std::string label;
};

/**
 * Time-windowed, flow-filtered packet error model for GSL experiments.
 *
 * Only PPP-encapsulated IPv4 packets matching the configured source and
 * destination addresses are eligible.  Outside the configured windows the
 * model never consumes an RNG sample and never drops a packet.
 */
class GslScheduledErrorModel : public ErrorModel
{
  public:
    static TypeId GetTypeId();

    GslScheduledErrorModel();
    ~GslScheduledErrorModel() override;

    void SetFlow(Ipv4Address source, Ipv4Address target);
    void SetWindows(const std::vector<GslLossWindow>& windows);
    void SetDropCallback(Callback<void, Ptr<const Packet>, uint32_t> callback);
    int64_t AssignStreams(int64_t stream);

    const std::vector<GslLossWindow>& GetWindows() const;
    uint64_t GetEligiblePacketCount() const;
    uint64_t GetDroppedPacketCount() const;

  private:
    bool DoCorrupt(Ptr<Packet> packet) override;
    void DoReset() override;
    bool MatchesFlow(Ptr<const Packet> packet) const;

    Ipv4Address m_source;
    Ipv4Address m_target;
    bool m_flowConfigured{false};
    std::vector<GslLossWindow> m_windows;
    Ptr<UniformRandomVariable> m_random;
    Callback<void, Ptr<const Packet>, uint32_t> m_dropCallback;
    uint64_t m_eligiblePackets{0};
    uint64_t m_droppedPackets{0};
};

} // namespace ns3

#endif // GSL_SCHEDULED_ERROR_MODEL_H
