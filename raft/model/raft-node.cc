#include "ns3/address.h"
#include "ns3/address-utils.h"
#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/node.h"
#include "ns3/socket.h"
#include "ns3/udp-socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "raft-node.h"
#include "stdlib.h"
#include "ns3/ipv4.h"
#include <ctime>
#include <map>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("RaftNode");

NS_OBJECT_ENSURE_REGISTERED (RaftNode);

TypeId
RaftNode::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::RaftNode")
    .SetParent<Application> ()
    .SetGroupName("Applications")
    .AddConstructor<RaftNode> ()
    ;

    return tid;
}

RaftNode::RaftNode(void) {

}

RaftNode::~RaftNode(void) {
    NS_LOG_FUNCTION (this);
}


static char intToChar(int a) {
    return a + '0';
}

static int charToInt(char a) {
    return a - '0';
}

// 信息接收延迟 0 - 5 ms
float 
getRandomDelay() {
  return (rand() % 5) * 1.0 / 1000;
}

// 选举超时时间 100 - 300 ms
float 
getElectionTimeout() {
  return ((rand() % 150) + 150) * 1.0 / 1000;
}


void 
RaftNode::StartApplication ()            
{
    // 初始化raft参数
    m_value = 0;                          // 变量初始值，每个节点均为0
    proposal = intToChar(m_id);           // 提案要修改变量的值，等于节点id
    heartbeat_timeout = 0.02;             // 心跳发送周期，统一设置为20ms
    vote_success = 0;                     // 获得的同意数
    vote_failed = 0;                      // 获得的失败数
    has_voted = 0;                        // 未开始投票
    add_change_value = 0;                 // 是否在心跳中加入提案
    int is_leader = 0;

    // 初始化socket
    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName ("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket (GetNode (), tid);

        // 注意 相当于监听所有网卡ip
        InetSocketAddress local = InetSocketAddress (Ipv4Address::GetAny (), 7071);
        m_socket->Bind (local);           // 绑定本机的ip和port
        m_socket->Listen ();
    }
    m_socket->SetRecvCallback (MakeCallback (&RaftNode::HandleRead, this));
    m_socket->SetAllowBroadcast (true);

    std::vector<Ipv4Address>::iterator iter = m_peersAddresses.begin();
    // 与所有节点建立连接
    while(iter != m_peersAddresses.end()) {
        // NS_LOG_INFO("node"<< m_id << *iter << "\n");
        TypeId tid = TypeId::LookupByName ("ns3::UdpSocketFactory");
        Ptr<Socket> socketClient = Socket::CreateSocket (GetNode (), tid);
        socketClient->Connect (InetSocketAddress(*iter, 7071));
        m_peersSockets[*iter] = socketClient;
        iter++;
    }
    // 开始为follower，超出 election_timeout 后成为candidate节点， 向所有邻节点广播票
    m_nextElection = Simulator::Schedule (Seconds(getElectionTimeout()), &RaftNode::sendVote, this);
}

void 
RaftNode::StopApplication ()
{
  NS_LOG_INFO ("At time " << Simulator::Now ().GetSeconds () << " finish the raft consensus");
}

void 
RaftNode::HandleRead (Ptr<Socket> socket)
{   
    Ptr<Packet> packet;
    Address from;
    Address localAddress;

    while ((packet = socket->RecvFrom (from)))
    {
        socket->SendTo(packet, 0, from);
        if (packet->GetSize () == 0)
        {   //EOF
            break;
        }
        if (InetSocketAddress::IsMatchingType (from))
        {
            std::string msg = getPacketContent(packet, from);

            // NS_LOG_INFO ("At time " << Simulator::Now ().GetSeconds () << "s, Node " << GetNode ()->GetId () << " received " << packet->GetSize () << " bytes from " <<
            //     InetSocketAddress::ConvertFrom (from).GetIpv4 () << " port " <<
            //     InetSocketAddress::ConvertFrom (from).GetPort ());

            // // 打印接收到的结果
            // NS_LOG_INFO("Node " << GetNode ()->GetId () << " Total Received Data: " << msg);
            uint8_t data[4];
            switch (charToInt(msg[0]))
            {
                case VOTE_REQ:           
                {   
                    // 处理投票请求，如果未投过票则返回成功
                    data[0] = intToChar(VOTE_RES);
                    if (has_voted == 0) {
                      data[1] = intToChar(SUCCESS);
                      has_voted = 1;
                    } 
                    else
                    {
                      data[1] = intToChar(FAILED);    
                    }             
                    Send(data, from);
                    break;
                }

                case HEARTBEAT:                   
                {
                  data[0] = intToChar(HEARTBEAT_RES);
                  int type = charToInt(msg[1]);
                  if (type == HEART_BEAT) {         // 表示普通心跳                                                 
                      data[1] = intToChar(0);       // 普通回复
                      // 重置选举超时时间      
                      Simulator::Cancel (m_nextElection);
                      m_nextElection = Simulator::Schedule (Seconds(getElectionTimeout()), &RaftNode::sendVote, this);

                  } else {                          // 表示是修改请求
                    data[1] = intToChar(1);         // 提案回复
                    int value = charToInt(msg[2]); 
                    m_value = value;
                    // 放弃继续选举, 停止模拟
                    NS_LOG_INFO("Node " << m_id << " change the value: " << value << " at time " <<Simulator::Now ().GetSeconds () << "s");
                    Simulator::Cancel (m_nextElection);
                  }
                  data[2] = intToChar(SUCCESS);
                  Send(data, from);
                  break;
                }

                case VOTE_RES:
                {
                  if (!is_leader) {
                    int state = charToInt(msg[1]);
                    if (state == SUCCESS) {
                        vote_success += 1;   
                    }
                    else {
                        vote_failed += 1;
                    }
                    // 如果得到超过半数的投票，则成为leader
                    // if (vote_success + vote_failed == N-1) {
                      
                      if (vote_success + 1 > N / 2) {
                          vote_success = 0;
                          vote_failed = 0;
                          NS_LOG_INFO("Node " << m_id << " become leader! at time " << Simulator::Now ().GetSeconds () << "s");
                          // 关闭自己的超时时间
                          Simulator::Cancel (m_nextElection);
                          // 在10s后开始在心跳中加入提案
                          Simulator::Schedule (Seconds(10), &RaftNode::setProposal, this);
                          sendHeartBeat();
                          is_leader = 1;
                        } 
                        else if (vote_failed >= N / 2)
                        {
                            vote_success = 0;
                            vote_failed = 0;
                            // 开启投票
                            has_voted = 0;
                            // 一半以上节点反对，重新选举            
                        }
                    //}
                  }
                
                  break;
                }
                case HEARTBEAT_RES:
                {
                    int type = charToInt(msg[1]);
                    if (type == PROPOSAL && charToInt(msg[2]) == SUCCESS) {
                        vote_success += 1;
                    } else {
                        vote_failed += 1;
                    }
                    if (vote_success + vote_failed == N-2) {
                        if (vote_success + 1 > N / 2) {
                            vote_success = 0;
                            vote_failed = 0;
                            NS_LOG_INFO ("At time " << Simulator::Now ().GetSeconds () << " leader finish the raft consensus");
                            // 停止心跳发送
                            Simulator::Cancel (m_nextHeartbeat);
                        } 
                        else
                        {
                            vote_success = 0;
                            vote_failed = 0;
                        }
                    }
                    break;
                   
                }
                default:
                {
                    NS_LOG_INFO("wrong msg");
                    break;
                }
            }
        }
        socket->GetSockName (localAddress);
    }
}


std::string 
RaftNode::getPacketContent(Ptr<Packet> packet, Address from) 
{ 
    char *packetInfo = new char[packet->GetSize () + 1];
    std::ostringstream totalStream;
    packet->CopyData (reinterpret_cast<uint8_t*>(packetInfo), packet->GetSize ());
    packetInfo[packet->GetSize ()] = '\0'; // ensure that it is null terminated to avoid bugs
    /**
     * Add the buffered data to complete the packet
     */
    totalStream << m_bufferedData[from] << packetInfo; 
    std::string totalReceivedData(totalStream.str());

    return totalReceivedData;
}  

void 
SendPacket(Ptr<Socket> socketClient,Ptr<Packet> p) {
    socketClient->Send(p);
}

// 向接收到消息的节点方 返回消息
void 
RaftNode::Send(uint8_t data[], Address from)
{
     Ptr<Packet> p;
    p = Create<Packet> (data, 3);
    //NS_LOG_INFO("packet: " << p);
    
    TypeId tid = TypeId::LookupByName ("ns3::UdpSocketFactory");
  

    Ptr<Socket> socketClient;
    if (!m_peersSockets[InetSocketAddress::ConvertFrom(from).GetIpv4 ()]) {
        socketClient = Socket::CreateSocket (GetNode (), tid);
        socketClient->Connect (InetSocketAddress(InetSocketAddress::ConvertFrom(from).GetIpv4 (), 7071));
        m_peersSockets[InetSocketAddress::ConvertFrom(from).GetIpv4 ()] = socketClient;
    }
    socketClient = m_peersSockets[InetSocketAddress::ConvertFrom(from).GetIpv4 ()];
    Simulator::Schedule(Seconds(getRandomDelay()), SendPacket, socketClient, p);
}


// 向所有邻居节点广播消息
void 
RaftNode::Send (uint8_t data[])
{   
  Ptr<Packet> p;
  p = Create<Packet> (data, 3);
  
  TypeId tid = TypeId::LookupByName ("ns3::UdpSocketFactory");


  std::vector<Ipv4Address>::iterator iter = m_peersAddresses.begin();

  while(iter != m_peersAddresses.end()) {
    TypeId tid = TypeId::LookupByName ("ns3::UdpSocketFactory");
    
    Ptr<Socket> socketClient = m_peersSockets[*iter];
    double delay = getRandomDelay();
    Simulator::Schedule(Seconds(delay), SendPacket, socketClient, p);
    iter++;
  }
}

// candidate节点请求投票
void 
RaftNode::sendVote(void) {
  has_voted = 1;      // 投自己，不再投给别人

  uint8_t data[3];
  data[0] = intToChar(VOTE_REQ);
  data[1] = intToChar(m_id);
  Send(data);
  NS_LOG_INFO("node" << m_id << " start election: "<< data << " at time: " << Simulator::Now ().GetSeconds () << "s" );
  m_nextElection = Simulator::Schedule (Seconds(getElectionTimeout()), &RaftNode::sendVote, this);
}

// leader节点广播心跳
void 
RaftNode::sendHeartBeat(void) {
  has_voted = 1;                        // 投自己，不再投给别人
  uint8_t data[4];
  data[0] = intToChar(HEARTBEAT);
  if (add_change_value == 1) {          // 加入提案
    data[1] = intToChar(1);
    data[2] = intToChar(m_id);
    m_value = m_id;
    NS_LOG_INFO("node" << m_id << " start send proposal: "<< data << " at time: " << Simulator::Now ().GetSeconds () << "s" );
    Simulator::Cancel(m_nextHeartbeat);
  } 
  else                                  // 普通心跳
  {
    data[1] = intToChar(0);
    // 递归设置下一次的心跳
    m_nextHeartbeat = Simulator::Schedule (Seconds(heartbeat_timeout), &RaftNode::sendHeartBeat, this);
  }
  Send(data);
  // NS_LOG_INFO("node" << m_id << " send heartbeat: "<< data);
}

// 设置发送提案
void 
RaftNode::setProposal(void) {
  add_change_value = 1;
}
} 