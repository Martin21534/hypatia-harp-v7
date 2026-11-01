/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "gsl-random-loss-helper.h"

#include "ns3/exp-util.h"
#include "ns3/gsl-net-device.h"
#include "ns3/ipv4.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace ns3 {

GslRandomLossHelper::GslRandomLossHelper(
    Ptr<BasicSimulation> basicSimulation,
    Ptr<TopologySatelliteNetwork> topology)
    : m_basicSimulation(basicSimulation),
      m_topology(topology)
{
    std::cout << "SETUP GSL RANDOM LOSS" << std::endl;

    m_enabled = parse_boolean(
        m_basicSimulation->GetConfigParamOrDefault("enable_gsl_random_loss", "false"));
    if (!m_enabled)
    {
        std::cout << "  > Not enabled explicitly, so disabled" << std::endl;
        std::cout << std::endl;
        return;
    }

    m_mode = m_basicSimulation->GetConfigParamOrDefault("gsl_random_loss_mode", "continuous");
    m_sourceNodeId = parse_positive_int64(
        m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_source_node_id"));
    m_targetNodeId = parse_positive_int64(
        m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_target_node_id"));
    m_stream = parse_positive_int64(
        m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_stream"));

    if (m_sourceNodeId == m_targetNodeId)
    {
        throw std::invalid_argument(
            "GSL random-loss source and target node IDs must be different");
    }
    if (m_sourceNodeId >= m_topology->GetNumNodes() ||
        !m_topology->IsValidEndpoint(m_sourceNodeId))
    {
        throw std::invalid_argument(
            "gsl_random_loss_source_node_id must identify a ground-station endpoint");
    }
    if (m_targetNodeId >= m_topology->GetNumNodes() ||
        !m_topology->IsValidEndpoint(m_targetNodeId))
    {
        throw std::invalid_argument(
            "gsl_random_loss_target_node_id must identify a ground-station endpoint");
    }

    if (m_mode == "continuous")
    {
        ConfigureContinuous();
    }
    else if (m_mode == "scheduled")
    {
        ConfigureScheduled();
    }
    else
    {
        throw std::invalid_argument(
            "gsl_random_loss_mode must be either continuous or scheduled");
    }

    std::cout << std::endl;
}

GslRandomLossHelper::~GslRandomLossHelper()
{
    if (m_dropLog.is_open())
    {
        m_dropLog.flush();
        m_dropLog.close();
    }
}

bool
GslRandomLossHelper::IsEnabled() const
{
    return m_enabled;
}

uint32_t
GslRandomLossHelper::GetDataDeviceCount() const
{
    return m_dataDeviceCount;
}

uint32_t
GslRandomLossHelper::GetAckDeviceCount() const
{
    return m_ackDeviceCount;
}

uint32_t
GslRandomLossHelper::GetUplinkDeviceCount() const
{
    return m_uplinkDeviceCount;
}

uint32_t
GslRandomLossHelper::GetDownlinkDeviceCount() const
{
    return m_downlinkDeviceCount;
}

void
GslRandomLossHelper::ConfigureContinuous()
{
    m_rate = parse_double_between_zero_and_one(
        m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_rate"));
    if (!std::isfinite(m_rate))
    {
        throw std::invalid_argument("gsl_random_loss_rate must be finite");
    }

    m_direction = m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_direction");
    if (m_direction != "data" && m_direction != "ack" && m_direction != "both")
    {
        throw std::invalid_argument(
            "gsl_random_loss_direction must be one of: data, ack, both");
    }
    if (m_direction == "both" && m_stream == std::numeric_limits<int64_t>::max())
    {
        throw std::invalid_argument(
            "gsl_random_loss_stream is too large to allocate a second stream");
    }

    if (m_direction == "data" || m_direction == "both")
    {
        m_dataErrorModel = CreateErrorModel(m_stream);
        m_dataDeviceCount =
            InstallOnEndpoint(m_targetNodeId, "DATA", m_dataErrorModel);
    }
    if (m_direction == "ack" || m_direction == "both")
    {
        const int64_t ackStream = m_direction == "both" ? m_stream + 1 : m_stream;
        m_ackErrorModel = CreateErrorModel(ackStream);
        m_ackDeviceCount = InstallOnEndpoint(m_sourceNodeId, "ACK", m_ackErrorModel);
    }

    std::cout << "  > Mode....................... continuous" << std::endl;
    std::cout << "  > Packet loss rate........... " << m_rate << std::endl;
    std::cout << "  > Direction.................. " << m_direction << std::endl;
    std::cout << "  > Source endpoint............ " << m_sourceNodeId << std::endl;
    std::cout << "  > Target endpoint............ " << m_targetNodeId << std::endl;
    std::cout << "  > Base RNG stream............ " << m_stream << std::endl;
    std::cout << "  > DATA GSL devices........... " << m_dataDeviceCount << std::endl;
    std::cout << "  > ACK GSL devices............ " << m_ackDeviceCount << std::endl;
}

void
GslRandomLossHelper::ConfigureScheduled()
{
    if (m_stream == std::numeric_limits<int64_t>::max())
    {
        throw std::invalid_argument(
            "gsl_random_loss_stream is too large to allocate uplink and downlink streams");
    }

    const std::string scheduleFilename =
        m_basicSimulation->GetConfigParamOrFail("gsl_random_loss_schedule_filename");
    const std::vector<ScheduledWindow> schedule = ReadSchedule(scheduleFilename);
    for (const auto& entry : schedule)
    {
        if (entry.linkDirection == "uplink")
        {
            m_uplinkWindows.push_back(entry.loss);
        }
        else
        {
            m_downlinkWindows.push_back(entry.loss);
        }
    }
    if (m_uplinkWindows.empty() || m_downlinkWindows.empty())
    {
        throw std::invalid_argument(
            "Scheduled GSL loss requires at least one uplink and one downlink window");
    }

    const Ipv4Address sourceAddress = GetEndpointIpv4Address(m_sourceNodeId);
    const Ipv4Address targetAddress = GetEndpointIpv4Address(m_targetNodeId);

    m_uplinkErrorModel = CreateObject<GslScheduledErrorModel>();
    m_uplinkErrorModel->SetFlow(sourceAddress, targetAddress);
    m_uplinkErrorModel->SetWindows(m_uplinkWindows);
    m_uplinkErrorModel->AssignStreams(m_stream);
    m_uplinkErrorModel->SetDropCallback(
        MakeCallback(&GslRandomLossHelper::RecordUplinkDrop, this));
    m_uplinkDeviceCount = InstallOnSatelliteReceivers(m_uplinkErrorModel);

    m_downlinkErrorModel = CreateObject<GslScheduledErrorModel>();
    m_downlinkErrorModel->SetFlow(sourceAddress, targetAddress);
    m_downlinkErrorModel->SetWindows(m_downlinkWindows);
    m_downlinkErrorModel->AssignStreams(m_stream + 1);
    m_downlinkErrorModel->SetDropCallback(
        MakeCallback(&GslRandomLossHelper::RecordDownlinkDrop, this));
    m_downlinkDeviceCount =
        InstallOnEndpoint(m_targetNodeId, "DATA downlink", m_downlinkErrorModel);

    const std::string dropLogPath =
        m_basicSimulation->GetLogsDir() + "/gsl_random_loss_drops.csv";
    m_dropLog.open(dropLogPath, std::ios::out | std::ios::trunc);
    if (!m_dropLog.is_open())
    {
        throw std::runtime_error("Cannot open scheduled GSL loss log: " + dropLogPath);
    }
    m_dropLog << "time_ns,packet_uid,link_direction,window_label,rate\n";

    std::cout << "  > Mode....................... scheduled" << std::endl;
    std::cout << "  > Schedule................... " << scheduleFilename << std::endl;
    std::cout << "  > Source endpoint............ " << m_sourceNodeId << " ("
              << sourceAddress << ")" << std::endl;
    std::cout << "  > Target endpoint............ " << m_targetNodeId << " ("
              << targetAddress << ")" << std::endl;
    std::cout << "  > Base RNG stream............ " << m_stream << std::endl;
    std::cout << "  > Uplink satellite devices... " << m_uplinkDeviceCount << std::endl;
    std::cout << "  > Downlink target devices.... " << m_downlinkDeviceCount << std::endl;
    std::cout << "  > Drop log................... " << dropLogPath << std::endl;
    for (const auto& entry : schedule)
    {
        std::cout << "    - " << entry.linkDirection << " [" << entry.loss.startTimeNs
                  << ", " << entry.loss.endTimeNs << ") ns rate=" << entry.loss.rate
                  << " label=" << entry.loss.label << std::endl;
    }
}

std::vector<GslRandomLossHelper::ScheduledWindow>
GslRandomLossHelper::ReadSchedule(const std::string& filename) const
{
    const std::string path = m_basicSimulation->GetRunDir() + "/" + filename;
    std::ifstream input(path);
    if (!input.is_open())
    {
        throw std::runtime_error("Cannot open GSL random-loss schedule: " + path);
    }

    std::vector<ScheduledWindow> schedule;
    std::set<std::string> labels;
    std::string line;
    uint32_t lineNumber = 0;
    int64_t previousEndNs = -1;
    bool headerSeen = false;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        if (line == "start_ns,end_ns,link_direction,rate,label")
        {
            if (headerSeen || !schedule.empty() || lineNumber != 1)
            {
                throw std::invalid_argument("GSL loss schedule header must be the first line");
            }
            headerSeen = true;
            continue;
        }
        if (!headerSeen)
        {
            throw std::invalid_argument(
                "GSL loss schedule must start with the expected CSV header");
        }

        const std::vector<std::string> fields = split_string(line, ",", 5);
        ScheduledWindow entry;
        entry.loss.startTimeNs = parse_positive_int64(fields[0]);
        entry.loss.endTimeNs = parse_positive_int64(fields[1]);
        entry.linkDirection = fields[2];
        entry.loss.rate = parse_double_between_zero_and_one(fields[3]);
        entry.loss.label = fields[4];

        if (entry.loss.endTimeNs <= entry.loss.startTimeNs ||
            entry.loss.endTimeNs > m_basicSimulation->GetSimulationEndTimeNs())
        {
            throw std::invalid_argument(
                "GSL loss schedule window must be non-empty and inside the simulation");
        }
        if (entry.linkDirection != "uplink" && entry.linkDirection != "downlink")
        {
            throw std::invalid_argument(
                "GSL loss schedule link_direction must be uplink or downlink");
        }
        if (!std::isfinite(entry.loss.rate) || entry.loss.rate <= 0.0)
        {
            throw std::invalid_argument("GSL loss schedule rate must be in (0, 1]");
        }
        if (entry.loss.label.empty() || !labels.insert(entry.loss.label).second)
        {
            throw std::invalid_argument("GSL loss schedule labels must be non-empty and unique");
        }
        if (previousEndNs > entry.loss.startTimeNs)
        {
            throw std::invalid_argument("GSL loss schedule windows cannot overlap");
        }
        previousEndNs = entry.loss.endTimeNs;
        schedule.push_back(entry);
    }

    if (!headerSeen || schedule.empty())
    {
        throw std::invalid_argument("GSL random-loss schedule cannot be empty");
    }
    return schedule;
}

Ipv4Address
GslRandomLossHelper::GetEndpointIpv4Address(int64_t nodeId) const
{
    Ptr<Node> node = m_topology->GetNodes().Get(static_cast<uint32_t>(nodeId));
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4)
    {
        throw std::runtime_error("Endpoint has no IPv4 stack: " + std::to_string(nodeId));
    }

    for (uint32_t interface = 1; interface < ipv4->GetNInterfaces(); ++interface)
    {
        if (!DynamicCast<GSLNetDevice>(ipv4->GetNetDevice(interface)))
        {
            continue;
        }
        for (uint32_t address = 0; address < ipv4->GetNAddresses(interface); ++address)
        {
            const Ipv4Address local = ipv4->GetAddress(interface, address).GetLocal();
            if (local != Ipv4Address::GetAny())
            {
                return local;
            }
        }
    }
    throw std::runtime_error("Endpoint has no GSL IPv4 address: " + std::to_string(nodeId));
}

Ptr<RateErrorModel>
GslRandomLossHelper::CreateErrorModel(int64_t stream) const
{
    Ptr<UniformRandomVariable> randomVariable = CreateObject<UniformRandomVariable>();
    randomVariable->SetStream(stream);

    Ptr<RateErrorModel> errorModel = CreateObject<RateErrorModel>();
    errorModel->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    errorModel->SetRate(m_rate);
    errorModel->SetRandomVariable(randomVariable);
    return errorModel;
}

uint32_t
GslRandomLossHelper::InstallOnEndpoint(int64_t nodeId,
                                      const std::string& trafficDirection,
                                      Ptr<ErrorModel> errorModel)
{
    Ptr<Node> node = m_topology->GetNodes().Get(static_cast<uint32_t>(nodeId));
    uint32_t installed = 0;
    for (uint32_t deviceIndex = 0; deviceIndex < node->GetNDevices(); ++deviceIndex)
    {
        Ptr<GSLNetDevice> gslDevice = DynamicCast<GSLNetDevice>(node->GetDevice(deviceIndex));
        if (!gslDevice)
        {
            continue;
        }

        PointerValue existingValue;
        gslDevice->GetAttribute("ReceiveErrorModel", existingValue);
        if (existingValue.GetObject())
        {
            throw std::runtime_error("Cannot install " + trafficDirection +
                                     " GSL random loss on node " +
                                     std::to_string(nodeId) +
                                     ": ReceiveErrorModel is already configured");
        }

        // A shared model preserves one RNG stream across GSL interfaces.
        gslDevice->SetReceiveErrorModel(errorModel);
        ++installed;
    }

    if (installed == 0)
    {
        throw std::runtime_error("Cannot install " + trafficDirection +
                                 " GSL random loss: endpoint has no GSL devices");
    }
    return installed;
}

uint32_t
GslRandomLossHelper::InstallOnSatelliteReceivers(Ptr<ErrorModel> errorModel)
{
    uint32_t installed = 0;
    const NodeContainer& satellites = m_topology->GetSatelliteNodes();
    for (uint32_t nodeIndex = 0; nodeIndex < satellites.GetN(); ++nodeIndex)
    {
        Ptr<Node> node = satellites.Get(nodeIndex);
        for (uint32_t deviceIndex = 0; deviceIndex < node->GetNDevices(); ++deviceIndex)
        {
            Ptr<GSLNetDevice> gslDevice =
                DynamicCast<GSLNetDevice>(node->GetDevice(deviceIndex));
            if (!gslDevice)
            {
                continue;
            }

            PointerValue existingValue;
            gslDevice->GetAttribute("ReceiveErrorModel", existingValue);
            if (existingValue.GetObject())
            {
                throw std::runtime_error(
                    "Cannot install DATA uplink random loss: satellite GSL receiver already "
                    "has an error model");
            }
            gslDevice->SetReceiveErrorModel(errorModel);
            ++installed;
        }
    }
    if (installed == 0)
    {
        throw std::runtime_error(
            "Cannot install DATA uplink random loss: no satellite GSL receivers found");
    }
    return installed;
}

void
GslRandomLossHelper::RecordUplinkDrop(Ptr<const Packet> packet, uint32_t windowIndex)
{
    RecordDrop("uplink", packet, m_uplinkWindows.at(windowIndex));
}

void
GslRandomLossHelper::RecordDownlinkDrop(Ptr<const Packet> packet, uint32_t windowIndex)
{
    RecordDrop("downlink", packet, m_downlinkWindows.at(windowIndex));
}

void
GslRandomLossHelper::RecordDrop(const std::string& linkDirection,
                                Ptr<const Packet> packet,
                                const GslLossWindow& window)
{
    if (!m_dropLog.is_open())
    {
        return;
    }
    m_dropLog << Simulator::Now().GetNanoSeconds() << ',' << packet->GetUid() << ','
              << linkDirection << ',' << window.label << ',' << window.rate << '\n';
    m_dropLog.flush();
}

} // namespace ns3
