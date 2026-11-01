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

#include "arbiter-single-forward.h"

#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/tcp-harp-cubic-v4.h"
#include "ns3/tcp-option-harp.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (ArbiterSingleForward);
NS_LOG_COMPONENT_DEFINE("ArbiterSingleForward");
TypeId ArbiterSingleForward::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::ArbiterSingleForward")
            .SetParent<ArbiterSatnet> ()
            .SetGroupName("BasicSim")
            .AddAttribute("EnableHarpV5PacketMarking",
                          "Serialize HARP V5 marks into TCP packets after a first-hop change",
                          BooleanValue(false),
                          MakeBooleanAccessor(&ArbiterSingleForward::m_enableHarpV5PacketMarking),
                          MakeBooleanChecker())
            .AddAttribute("HarpV5MarkDuration",
                          "Maximum time to repeat a HARP V5 mark after a handover",
                          TimeValue(MilliSeconds(250)),
                          MakeTimeAccessor(&ArbiterSingleForward::m_harpV5MarkDuration),
                          MakeTimeChecker())
    ;
    return tid;
}

ArbiterSingleForward::ArbiterSingleForward(
        Ptr<Node> this_node,
        NodeContainer nodes,
        std::vector<std::tuple<int32_t, int32_t, int32_t>> next_hop_list
) : ArbiterSatnet(this_node, nodes)
{
    m_next_hop_list = next_hop_list;
    m_last_tcp_next_hop_list.assign(next_hop_list.size(), -2);
    m_harpV5DataEpoch.assign(next_hop_list.size(), 0);
    m_harpV5AckEpoch.assign(next_hop_list.size(), 0);
    m_harpV5DataMarkUntilNs.assign(next_hop_list.size(), -1);
    m_harpV5AckMarkUntilNs.assign(next_hop_list.size(), -1);
}

bool
ArbiterSingleForward::AppendHarpV5Mark(Ptr<const Packet> pkt,
                                       const Ipv4Header& ipHeader,
                                       uint8_t signal,
                                       uint16_t epoch) const
{
    if (!pkt)
    {
        return false;
    }

    // 此處只會處理 source node 的 RouteOutput 封包；底層 routing module
    // 直接修改即將上線的 TCP header，而不是把事件寫入 TCP 全域變數。
    Ptr<Packet> mutablePacket = ConstCast<Packet>(pkt);
    TcpHeader tcpHeader;
    if (mutablePacket->RemoveHeader(tcpHeader) == 0)
    {
        return false;
    }

    const auto harpSignal = static_cast<TcpOptionHarp::Signal>(signal);
    for (const auto& existingOption : tcpHeader.GetOptionList())
    {
        if (existingOption->GetKind() != TcpOption::HARP)
        {
            continue;
        }

        const auto existingHarp = DynamicCast<const TcpOptionHarp>(existingOption);
        if (existingHarp && existingHarp->GetSignal() == harpSignal &&
            existingHarp->GetEpoch() == epoch)
        {
            // RouteOutput can inspect the same packet more than once.  The
            // same signal epoch must remain idempotent on the wire.
            if (Node::ChecksumEnabled())
            {
                tcpHeader.EnableChecksums();
            }
            tcpHeader.InitializeChecksum(ipHeader.GetSource(), ipHeader.GetDestination(), 6);
            mutablePacket->AddHeader(tcpHeader);
            return true;
        }
    }

    // A header deserialized from a packet contains an END option representing
    // its old alignment padding.  Rebuild the header without END before adding
    // HARP; otherwise the receiver stops parsing before the new option and the
    // trailing HARP bytes are mistaken for TCP payload.
    TcpHeader markedHeader;
    markedHeader.SetSourcePort(tcpHeader.GetSourcePort());
    markedHeader.SetDestinationPort(tcpHeader.GetDestinationPort());
    markedHeader.SetSequenceNumber(tcpHeader.GetSequenceNumber());
    markedHeader.SetAckNumber(tcpHeader.GetAckNumber());
    markedHeader.SetFlags(tcpHeader.GetFlags());
    markedHeader.SetWindowSize(tcpHeader.GetWindowSize());
    markedHeader.SetUrgentPointer(tcpHeader.GetUrgentPointer());

    bool marked = true;
    for (const auto& existingOption : tcpHeader.GetOptionList())
    {
        if (existingOption->GetKind() == TcpOption::END)
        {
            break;
        }
        if (!markedHeader.AppendOption(existingOption))
        {
            marked = false;
            break;
        }
    }

    Ptr<TcpOptionHarp> option = CreateObject<TcpOptionHarp>();
    option->SetSignal(harpSignal);
    option->SetEpoch(epoch);
    marked = marked && markedHeader.AppendOption(option);

    TcpHeader& outputHeader = marked ? markedHeader : tcpHeader;
    if (Node::ChecksumEnabled())
    {
        outputHeader.EnableChecksums();
    }
    outputHeader.InitializeChecksum(ipHeader.GetSource(), ipHeader.GetDestination(), 6);
    mutablePacket->AddHeader(outputHeader);
    return marked;
}

std::tuple<int32_t, int32_t, int32_t> ArbiterSingleForward::TopologySatelliteNetworkDecide(
        int32_t source_node_id,
        int32_t target_node_id,
        Ptr<const Packet> pkt,
        Ipv4Header const &ipHeader,
        bool is_request_for_source_ip_so_no_next_header
) {
    const auto decision = m_next_hop_list[target_node_id];
    const int32_t next_hop_node_id = std::get<0>(decision);

    // V4 不查看未來的 forwarding-state 檔案。只有真正送出的 TCP 封包，
    // 在來源端第一次使用不同的 first hop 時，才通知 TCP 發生換手。
    if (!is_request_for_source_ip_so_no_next_header &&
        m_node_id == source_node_id &&
        ipHeader.GetProtocol() == 6 &&
        pkt != nullptr)
    {
        const int32_t previous_next_hop = m_last_tcp_next_hop_list[target_node_id];
        if (previous_next_hop >= 0 &&
            next_hop_node_id >= 0 &&
            previous_next_hop != next_hop_node_id)
        {
            Ptr<Packet> packet_copy = pkt->Copy();
            TcpHeader tcp_header;
            packet_copy->RemoveHeader(tcp_header);
            const bool carries_data = packet_copy->GetSize() > 0;

            TcpHarpCubicV4::NotifyHandover(
                carries_data ? TcpHarpCubicV4::HandoverDirection::DATA_PATH
                             : TcpHarpCubicV4::HandoverDirection::ACK_PATH);

            if (m_enableHarpV5PacketMarking)
            {
                auto& epoch = carries_data ? m_harpV5DataEpoch[target_node_id]
                                           : m_harpV5AckEpoch[target_node_id];
                ++epoch;
                if (epoch == 0)
                {
                    ++epoch;
                }

                const int64_t untilNs =
                    Simulator::Now().GetNanoSeconds() + m_harpV5MarkDuration.GetNanoSeconds();
                if (carries_data)
                {
                    m_harpV5DataMarkUntilNs[target_node_id] = untilNs;
                }
                else
                {
                    m_harpV5AckMarkUntilNs[target_node_id] = untilNs;
                }
            }
        }
        m_last_tcp_next_hop_list[target_node_id] = next_hop_node_id;

        if (m_enableHarpV5PacketMarking)
        {
            Ptr<Packet> packetCopy = pkt->Copy();
            TcpHeader tcpHeader;
            packetCopy->RemoveHeader(tcpHeader);
            const bool carriesData = packetCopy->GetSize() > 0;
            const int64_t nowNs = Simulator::Now().GetNanoSeconds();
            const bool markActive = carriesData
                                        ? nowNs <= m_harpV5DataMarkUntilNs[target_node_id]
                                        : nowNs <= m_harpV5AckMarkUntilNs[target_node_id];
            if (markActive)
            {
                const auto signal = carriesData ? TcpOptionHarp::Signal::DATA_MARK
                                                : TcpOptionHarp::Signal::ACK_MARK;
                const uint16_t epoch = carriesData ? m_harpV5DataEpoch[target_node_id]
                                                   : m_harpV5AckEpoch[target_node_id];
                const bool appended = AppendHarpV5Mark(pkt,
                                                       ipHeader,
                                                       static_cast<uint8_t>(signal),
                                                       epoch);
                NS_LOG_INFO("HarpV5Signal"
                            << " event=" << (carriesData ? "MARK_TX" : "ACK_MARK_TX")
                            << " time_ns=" << nowNs
                            << " source=" << source_node_id
                            << " target=" << target_node_id
                            << " epoch=" << epoch
                            << " appended=" << (appended ? 1 : 0));
            }
        }
    }

    return decision;
}

void ArbiterSingleForward::SetSingleForwardState(int32_t target_node_id, int32_t next_node_id, int32_t own_if_id, int32_t next_if_id) {
    NS_ABORT_MSG_IF(next_node_id == -2 || own_if_id == -2 || next_if_id == -2, "Not permitted to set invalid (-2).");
    m_next_hop_list[target_node_id] = std::make_tuple(next_node_id, own_if_id, next_if_id);
}

std::string ArbiterSingleForward::StringReprOfForwardingState() {
    std::ostringstream res;
    res << "Single-forward state of node " << m_node_id << std::endl;
    for (size_t i = 0; i < m_nodes.GetN(); i++) {
        res << "  -> " << i << ": (" << std::get<0>(m_next_hop_list[i]) << ", "
            << std::get<1>(m_next_hop_list[i]) << ", "
            << std::get<2>(m_next_hop_list[i]) << ")" << std::endl;
    }
    return res.str();
}

}
