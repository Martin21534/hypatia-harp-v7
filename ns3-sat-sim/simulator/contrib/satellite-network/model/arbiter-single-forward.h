/*
 * Copyright (c) 2020 ETH Zurich
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Simon               2020
 */

#ifndef ARBITER_SINGLE_FORWARD_H
#define ARBITER_SINGLE_FORWARD_H

#include <tuple>
#include "ns3/arbiter-satnet.h"
#include "ns3/topology-satellite-network.h"
#include "ns3/hash.h"
#include "ns3/abort.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/tcp-header.h"
#include "ns3/nstime.h"

namespace ns3 {

class ArbiterSingleForward : public ArbiterSatnet
{
public:
    static TypeId GetTypeId (void);

    // Constructor for single forward next-hop forwarding state
    ArbiterSingleForward(
            Ptr<Node> this_node,
            NodeContainer nodes,
            std::vector<std::tuple<int32_t, int32_t, int32_t>> next_hop_list
    );

    // Single forward next-hop implementation
    std::tuple<int32_t, int32_t, int32_t> TopologySatelliteNetworkDecide(
            int32_t source_node_id,
            int32_t target_node_id,
            ns3::Ptr<const ns3::Packet> pkt,
            ns3::Ipv4Header const &ipHeader,
            bool is_socket_request_for_source_ip
    );

    // Updating of forward state
    void SetSingleForwardState(int32_t target_node_id, int32_t next_node_id, int32_t own_if_id, int32_t next_if_id);

    // Static routing table
    std::string StringReprOfForwardingState();

private:
    bool AppendHarpV5Mark(ns3::Ptr<const ns3::Packet> pkt,
                          const ns3::Ipv4Header& ipHeader,
                          uint8_t signal,
                          uint16_t epoch) const;

    std::vector<std::tuple<int32_t, int32_t, int32_t>> m_next_hop_list;

    // V4 只在實際 TCP 封包使用 first hop 時觀察變化；-2 表示尚未建立基準。
    std::vector<int32_t> m_last_tcp_next_hop_list;

    // V5 將實際換手事件序列化在封包內，不直接呼叫 TCP。每個目的地與
    // 方向各自維護 epoch，避免不同 flow/path 的事件互相覆蓋。
    bool m_enableHarpV5PacketMarking{false};
    Time m_harpV5MarkDuration{MilliSeconds(250)};
    std::vector<uint16_t> m_harpV5DataEpoch;
    std::vector<uint16_t> m_harpV5AckEpoch;
    std::vector<int64_t> m_harpV5DataMarkUntilNs;
    std::vector<int64_t> m_harpV5AckMarkUntilNs;

};

}

#endif //ARBITER_SINGLE_FORWARD_H
