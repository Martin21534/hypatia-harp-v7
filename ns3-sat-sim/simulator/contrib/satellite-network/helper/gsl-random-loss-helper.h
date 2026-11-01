/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef GSL_RANDOM_LOSS_HELPER_H
#define GSL_RANDOM_LOSS_HELPER_H

#include "ns3/basic-simulation.h"
#include "ns3/error-model.h"
#include "ns3/gsl-scheduled-error-model.h"
#include "ns3/ipv4-address.h"
#include "ns3/topology-satellite-network.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ns3 {

/**
 * Installs reproducible packet-level random loss on GSL receivers.
 *
 * Continuous mode retains the original endpoint DATA/ACK behavior. Scheduled
 * mode models physical DATA uplink and downlink legs independently and filters
 * packets by the configured endpoint IPv4 flow.
 */
class GslRandomLossHelper
{
  public:
    GslRandomLossHelper(Ptr<BasicSimulation> basicSimulation,
                        Ptr<TopologySatelliteNetwork> topology);
    ~GslRandomLossHelper();

    bool IsEnabled() const;
    uint32_t GetDataDeviceCount() const;
    uint32_t GetAckDeviceCount() const;
    uint32_t GetUplinkDeviceCount() const;
    uint32_t GetDownlinkDeviceCount() const;

  private:
    struct ScheduledWindow
    {
        std::string linkDirection;
        GslLossWindow loss;
    };

    void ConfigureContinuous();
    void ConfigureScheduled();
    std::vector<ScheduledWindow> ReadSchedule(const std::string& filename) const;
    Ipv4Address GetEndpointIpv4Address(int64_t nodeId) const;

    Ptr<RateErrorModel> CreateErrorModel(int64_t stream) const;
    uint32_t InstallOnEndpoint(int64_t nodeId,
                               const std::string& trafficDirection,
                               Ptr<ErrorModel> errorModel);
    uint32_t InstallOnSatelliteReceivers(Ptr<ErrorModel> errorModel);
    void RecordUplinkDrop(Ptr<const Packet> packet, uint32_t windowIndex);
    void RecordDownlinkDrop(Ptr<const Packet> packet, uint32_t windowIndex);
    void RecordDrop(const std::string& linkDirection,
                    Ptr<const Packet> packet,
                    const GslLossWindow& window);

    Ptr<BasicSimulation> m_basicSimulation;
    Ptr<TopologySatelliteNetwork> m_topology;
    bool m_enabled{false};
    std::string m_mode{"continuous"};
    double m_rate{0.0};
    std::string m_direction{"data"};
    int64_t m_sourceNodeId{-1};
    int64_t m_targetNodeId{-1};
    int64_t m_stream{0};

    Ptr<RateErrorModel> m_dataErrorModel;
    Ptr<RateErrorModel> m_ackErrorModel;
    Ptr<GslScheduledErrorModel> m_uplinkErrorModel;
    Ptr<GslScheduledErrorModel> m_downlinkErrorModel;
    std::vector<GslLossWindow> m_uplinkWindows;
    std::vector<GslLossWindow> m_downlinkWindows;
    std::ofstream m_dropLog;

    uint32_t m_dataDeviceCount{0};
    uint32_t m_ackDeviceCount{0};
    uint32_t m_uplinkDeviceCount{0};
    uint32_t m_downlinkDeviceCount{0};
};

} // namespace ns3

#endif // GSL_RANDOM_LOSS_HELPER_H
