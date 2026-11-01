/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "gsl-scheduled-error-model.h"

#include "ns3/ipv4-header.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/ppp-header.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("GslScheduledErrorModel");
NS_OBJECT_ENSURE_REGISTERED(GslScheduledErrorModel);

TypeId
GslScheduledErrorModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::GslScheduledErrorModel")
                            .SetParent<ErrorModel>()
                            .SetGroupName("SatelliteNetwork")
                            .AddConstructor<GslScheduledErrorModel>();
    return tid;
}

GslScheduledErrorModel::GslScheduledErrorModel()
    : m_random(CreateObject<UniformRandomVariable>())
{
}

GslScheduledErrorModel::~GslScheduledErrorModel() = default;

void
GslScheduledErrorModel::SetFlow(Ipv4Address source, Ipv4Address target)
{
    if (source == Ipv4Address::GetAny() || target == Ipv4Address::GetAny() ||
        source == target)
    {
        throw std::invalid_argument("Scheduled GSL loss requires distinct non-zero IPv4 endpoints");
    }
    m_source = source;
    m_target = target;
    m_flowConfigured = true;
}

void
GslScheduledErrorModel::SetWindows(const std::vector<GslLossWindow>& windows)
{
    int64_t previousEndNs = -1;
    for (const auto& window : windows)
    {
        if (window.startTimeNs < 0 || window.endTimeNs <= window.startTimeNs)
        {
            throw std::invalid_argument("Scheduled GSL loss window has an invalid time range");
        }
        if (!std::isfinite(window.rate) || window.rate <= 0.0 || window.rate > 1.0)
        {
            throw std::invalid_argument("Scheduled GSL loss rate must be in (0, 1]");
        }
        if (window.label.empty())
        {
            throw std::invalid_argument("Scheduled GSL loss window label cannot be empty");
        }
        if (previousEndNs > window.startTimeNs)
        {
            throw std::invalid_argument("Scheduled GSL loss windows cannot overlap");
        }
        previousEndNs = window.endTimeNs;
    }
    m_windows = windows;
    DoReset();
}

void
GslScheduledErrorModel::SetDropCallback(
    Callback<void, Ptr<const Packet>, uint32_t> callback)
{
    m_dropCallback = callback;
}

int64_t
GslScheduledErrorModel::AssignStreams(int64_t stream)
{
    m_random->SetStream(stream);
    return 1;
}

const std::vector<GslLossWindow>&
GslScheduledErrorModel::GetWindows() const
{
    return m_windows;
}

uint64_t
GslScheduledErrorModel::GetEligiblePacketCount() const
{
    return m_eligiblePackets;
}

uint64_t
GslScheduledErrorModel::GetDroppedPacketCount() const
{
    return m_droppedPackets;
}

bool
GslScheduledErrorModel::DoCorrupt(Ptr<Packet> packet)
{
    if (!m_flowConfigured || !MatchesFlow(packet))
    {
        return false;
    }

    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    for (uint32_t index = 0; index < m_windows.size(); ++index)
    {
        const auto& window = m_windows[index];
        if (nowNs < window.startTimeNs)
        {
            break;
        }
        if (nowNs >= window.endTimeNs)
        {
            continue;
        }

        ++m_eligiblePackets;
        if (m_random->GetValue() >= window.rate)
        {
            return false;
        }

        ++m_droppedPackets;
        if (!m_dropCallback.IsNull())
        {
            m_dropCallback(packet, index);
        }
        return true;
    }
    return false;
}

void
GslScheduledErrorModel::DoReset()
{
    m_eligiblePackets = 0;
    m_droppedPackets = 0;
}

bool
GslScheduledErrorModel::MatchesFlow(Ptr<const Packet> packet) const
{
    Ptr<Packet> copy = packet->Copy();
    PppHeader ppp;
    if (copy->GetSize() < ppp.GetSerializedSize())
    {
        return false;
    }
    copy->RemoveHeader(ppp);
    if (ppp.GetProtocol() != 0x0021) // PPP IPv4
    {
        return false;
    }

    Ipv4Header ipv4;
    if (copy->PeekHeader(ipv4) == 0)
    {
        return false;
    }
    return ipv4.GetSource() == m_source && ipv4.GetDestination() == m_target;
}

} // namespace ns3
