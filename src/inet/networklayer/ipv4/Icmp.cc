//
// Copyright (C) 2000 Institut fuer Telematik, Universitaet Karlsruhe
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

//  Cleanup and rewrite: Andras Varga, 2004

#include <string.h>

#include "inet/networklayer/ipv4/Icmp.h"

#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/ProtocolGroup.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/serializer/TcpIpChecksum.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/networklayer/contract/IInterfaceTable.h"
#include "inet/networklayer/ipv4/Ipv4Header.h"
#include "inet/networklayer/ipv4/Ipv4InterfaceData.h"

namespace inet {

Define_Module(Icmp);

void Icmp::initialize(int stage)
{
    cSimpleModule::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        const char *crcModeString = par("crcMode");
        if (!strcmp(crcModeString, "declared"))
            crcMode = CRC_DECLARED_CORRECT;
        else if (!strcmp(crcModeString, "computed"))
            crcMode = CRC_COMPUTED;
        else
            throw cRuntimeError("unknown CRC mode: '%s'", crcModeString);
    }
    else if (stage == INITSTAGE_NETWORK_LAYER) {
        registerProtocol(Protocol::icmpv4, gate("ipOut"));
        registerProtocol(Protocol::icmpv4, gate("transportOut"));
    }
}

void Icmp::handleMessage(cMessage *msg)
{
    cGate *arrivalGate = msg->getArrivalGate();

    // process arriving ICMP message
    if (arrivalGate->isName("ipIn")) {
        EV_INFO << "Received " << msg << " from network protocol.\n";
        processICMPMessage(check_and_cast<Packet *>(msg));
        return;
    }
    else
        throw cRuntimeError("Message %s(%s) arrived in unknown '%s' gate", msg->getName(), msg->getClassName(), msg->getArrivalGate()->getName());
}

void Icmp::sendErrorMessage(Packet *packet, int inputInterfaceId, IcmpType type, IcmpCode code)
{
    Enter_Method("sendErrorMessage(datagram, type=%d, code=%d)", type, code);

    const auto& ipv4Header = packet->peekHeader<Ipv4Header>();
    Ipv4Address origSrcAddr = ipv4Header->getSrcAddress();
    Ipv4Address origDestAddr = ipv4Header->getDestAddress();

    // don't send ICMP error messages in response to broadcast or multicast messages
    if (origDestAddr.isMulticast() || origDestAddr.isLimitedBroadcastAddress() || possiblyLocalBroadcast(origDestAddr, inputInterfaceId)) {
        EV_DETAIL << "won't send ICMP error messages for broadcast/multicast message " << ipv4Header << endl;
        delete packet;
        return;
    }

    // don't send ICMP error messages response to unspecified, broadcast or multicast addresses
    if ((inputInterfaceId != -1 && origSrcAddr.isUnspecified())
            || origSrcAddr.isMulticast()
            || origSrcAddr.isLimitedBroadcastAddress()
            || possiblyLocalBroadcast(origSrcAddr, inputInterfaceId)) {
        EV_DETAIL << "won't send ICMP error messages to broadcast/multicast address, message " << ipv4Header << endl;
        delete packet;
        return;
    }

    // ICMP messages are only sent about errors in handling fragment zero of fragmented datagrams
    if (ipv4Header->getFragmentOffset() != 0) {
        EV_DETAIL << "won't send ICMP error messages about errors in non-first fragments" << endl;
        delete packet;
        return;
    }

    // do not reply with error message to error message
    if (ipv4Header->getProtocolId() == IP_PROT_ICMP) {
        const auto& recICMPMsg = packet->peekDataAt<IcmpHeader>(B(ipv4Header->getHeaderLength()));
        if (!isIcmpInfoType(recICMPMsg->getType())) {
            EV_DETAIL << "ICMP error received -- do not reply to it" << endl;
            delete packet;
            return;
        }
    }

    // assemble a message name
    char msgname[80];
    static long ctr;
    sprintf(msgname, "ICMP-error-#%ld-type%d-code%d", ++ctr, type, code);

    // debugging information
    EV_DETAIL << "sending ICMP error " << msgname << endl;

    // create and send ICMP packet
    Packet *errorPacket = new Packet(msgname);
    const auto& icmpHeader = makeShared<IcmpHeader>();
    icmpHeader->setChunkLength(B(8));      //FIXME second 4 byte in icmp header not represented yet
    icmpHeader->setType(type);
    icmpHeader->setCode(code);
    // ICMP message length: the internet header plus the first 8 bytes of
    // the original datagram's data is returned to the sender.
    errorPacket->insertAtEnd(packet->peekDataAt(B(0), B(ipv4Header->getHeaderLength() + 8)));
    insertCrc(icmpHeader,errorPacket);
    errorPacket->insertHeader(icmpHeader);

    errorPacket->addTagIfAbsent<PacketProtocolTag>()->setProtocol(&Protocol::icmpv4);

    // if srcAddr is not filled in, we're still in the src node, so we just
    // process the ICMP message locally, right away
    if (origSrcAddr.isUnspecified()) {
        // pretend it came from the Ipv4 layer
        errorPacket->addTagIfAbsent<L3AddressInd>()->setDestAddress(Ipv4Address::LOOPBACK_ADDRESS);    // FIXME maybe use configured loopback address

        // then process it locally
        processICMPMessage(errorPacket);
    }
    else {
        sendToIP(errorPacket, ipv4Header->getSrcAddress());
    }
    delete packet;
}

bool Icmp::possiblyLocalBroadcast(const Ipv4Address& addr, int interfaceId)
{
    if ((addr.getInt() & 1) == 0)
        return false;

    IIpv4RoutingTable *rt = getModuleFromPar<IIpv4RoutingTable>(par("routingTableModule"), this);
    if (rt->isLocalBroadcastAddress(addr))
        return true;

    // if the input interface is unconfigured, we won't recognize network-directed broadcasts because we don't what network we are on
    IInterfaceTable *ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
    if (interfaceId != -1) {
        InterfaceEntry *ie = ift->getInterfaceById(interfaceId);
        bool interfaceUnconfigured = (ie->ipv4Data() == nullptr) || ie->ipv4Data()->getIPAddress().isUnspecified();
        return interfaceUnconfigured;
    }
    else {
        // if all interfaces are configured, we are OK
        bool allInterfacesConfigured = true;
        for (int i = 0; i < (int)ift->getNumInterfaces(); i++)
            if ((ift->getInterface(i)->ipv4Data() == nullptr) || ift->getInterface(i)->ipv4Data()->getIPAddress().isUnspecified())
                allInterfacesConfigured = false;

        return !allInterfacesConfigured;
    }
}

void Icmp::processICMPMessage(Packet *packet)
{
    if (!verifyCrc(packet)) {
        EV_WARN << "incoming ICMP packet has wrong CRC, dropped\n";
        // drop packet
        delete packet;
        return;
    }

    const auto& icmpmsg = packet->peekHeader<IcmpHeader>();
    switch (icmpmsg->getType()) {
        case ICMP_REDIRECT:
            // TODO implement redirect handling
            delete packet;
            break;

        case ICMP_DESTINATION_UNREACHABLE:
        case ICMP_TIME_EXCEEDED:
        case ICMP_PARAMETER_PROBLEM: {
            // ICMP errors are delivered to the appropriate higher layer protocol
            const auto& bogusL3Packet = packet->peekDataAt<Ipv4Header>(icmpmsg->getChunkLength());
            int transportProtocol = bogusL3Packet->getProtocolId();
            if (transportProtocol == IP_PROT_ICMP) {
                // received ICMP error answer to an ICMP packet:
                //FIXME should send up dest unreachable answers to pingapps
                errorOut(packet);
            }
            else {
                if (transportProtocols.find(transportProtocol) == transportProtocols.end()) {
                    EV_ERROR << "Transport protocol " << transportProtocol << " not registered, packet dropped\n";
                    delete packet;
                }
                else {
                    packet->addTagIfAbsent<DispatchProtocolReq>()->setProtocol(ProtocolGroup::ipprotocol.getProtocol(transportProtocol));
                    packet->addTagIfAbsent<PacketProtocolTag>()->setProtocol(&Protocol::icmpv4);
                    send(packet, "transportOut");
                }
            }
            break;
        }

        case ICMP_ECHO_REQUEST:
            processEchoRequest(packet);
            break;

        case ICMP_ECHO_REPLY:
            delete packet;
            break;

        case ICMP_TIMESTAMP_REQUEST:
            processEchoRequest(packet);
            break;

        case ICMP_TIMESTAMP_REPLY:
            delete packet;
            break;

        default:
            throw cRuntimeError("Unknown ICMP type %d", icmpmsg->getType());
    }
}

void Icmp::errorOut(Packet *packet)
{
    delete packet;
}

void Icmp::processEchoRequest(Packet *request)
{
    // turn request into a reply
    const auto& icmpReq = request->popHeader<IcmpEchoRequest>();
    Packet *reply = new Packet((std::string(request->getName()) + "-reply").c_str());
    const auto& icmpReply = makeShared<IcmpEchoReply>();
    icmpReply->setIdentifier(icmpReq->getIdentifier());
    icmpReply->setSeqNumber(icmpReq->getSeqNumber());
    auto addressInd = request->getTag<L3AddressInd>();
    Ipv4Address src = addressInd->getSrcAddress().toIPv4();
    Ipv4Address dest = addressInd->getDestAddress().toIPv4();
    reply->insertAtEnd(request->peekData());
    insertCrc(icmpReply, reply);
    reply->insertHeader(icmpReply);

    // swap src and dest
    // TBD check what to do if dest was multicast etc?
    // A. Ariza Modification 5/1/2011 clean the interface id, this forces the use of routing table in the Ipv4 layer
    auto addressReq = reply->addTagIfAbsent<L3AddressReq>();
    addressReq->setSrcAddress(addressInd->getDestAddress().toIPv4());
    addressReq->setDestAddress(addressInd->getSrcAddress().toIPv4());

    sendToIP(reply);
    delete request;
}

void Icmp::sendToIP(Packet *msg, const Ipv4Address& dest)
{
    msg->addTagIfAbsent<L3AddressReq>()->setDestAddress(dest);
    sendToIP(msg);
}

void Icmp::sendToIP(Packet *msg)
{
    // assumes Ipv4ControlInfo is already attached
    EV_INFO << "Sending " << msg << " to lower layer.\n";
    msg->addTagIfAbsent<DispatchProtocolReq>()->setProtocol(&Protocol::ipv4);
    msg->addTagIfAbsent<PacketProtocolTag>()->setProtocol(&Protocol::icmpv4);
    send(msg, "ipOut");
}

void Icmp::handleRegisterProtocol(const Protocol& protocol, cGate *gate)
{
    Enter_Method("handleRegisterProtocol");
    if (!strcmp("transportIn", gate->getBaseName())) {
        transportProtocols.insert(ProtocolGroup::ipprotocol.getProtocolNumber(&protocol));
    }
}

void Icmp::insertCrc(CrcMode crcMode, const Ptr<IcmpHeader>& icmpHeader, Packet *packet)
{
    icmpHeader->setCrcMode(crcMode);
    switch (crcMode) {
        case CRC_DECLARED_CORRECT:
            // if the CRC mode is declared to be correct, then set the CRC to an easily recognizable value
            icmpHeader->setChksum(0xC00D);
            break;
        case CRC_DECLARED_INCORRECT:
            // if the CRC mode is declared to be incorrect, then set the CRC to an easily recognizable value
            icmpHeader->setChksum(0xBAAD);
            break;
        case CRC_COMPUTED: {
            // if the CRC mode is computed, then compute the CRC and set it
            icmpHeader->setChksum(0x0000); // make sure that the CRC is 0 in the header before computing the CRC
            MemoryOutputStream icmpHeaderStream;
            Chunk::serialize(icmpHeaderStream, icmpHeader);
            const auto& icmpHeaderBytes = icmpHeaderStream.getData();
            auto icmpDataBytes = packet->peekDataBytes()->getBytes();
            auto icmpHeaderLength = icmpHeaderBytes.size();
            auto icmpDataLength =  icmpDataBytes.size();
            auto bufferLength = icmpHeaderLength + icmpDataLength;
            auto buffer = new uint8_t[bufferLength];
            std::copy(icmpHeaderBytes.begin(), icmpHeaderBytes.end(), buffer);
            std::copy(icmpDataBytes.begin(), icmpDataBytes.end(), buffer + icmpHeaderLength);
            uint16_t crc = inet::serializer::TcpIpChecksum::checksum(buffer, bufferLength);
            delete [] buffer;
            icmpHeader->setChksum(crc);
            break;
        }
        default:
            throw cRuntimeError("Unknown CRC mode");
    }
}

bool Icmp::verifyCrc(const Packet *packet)
{
    const auto& icmpHeader = packet->peekHeader<IcmpHeader>(b(-1), Chunk::PF_ALLOW_INCORRECT);
    switch (icmpHeader->getCrcMode()) {
        case CRC_DECLARED_CORRECT:
            // if the CRC mode is declared to be correct, then the check passes if and only if the chunks are correct
            return icmpHeader->isCorrect();
        case CRC_DECLARED_INCORRECT:
            // if the CRC mode is declared to be incorrect, then the check fails
            return false;
        case CRC_COMPUTED: {
            // otherwise compute the CRC, the check passes if the result is 0xFFFF (includes the received CRC)
            auto dataBytes = packet->peekDataBytes(Chunk::PF_ALLOW_INCORRECT);
            size_t bufferLength = B(dataBytes->getChunkLength()).get();
            auto buffer = new uint8_t[bufferLength];
            dataBytes->copyToBuffer(buffer, bufferLength);
            uint16_t crc = inet::serializer::TcpIpChecksum::checksum(buffer, bufferLength);
            delete [] buffer;
            // TODO: delete these isCorrect calls, rely on CRC only
            return crc == 0 && icmpHeader->isCorrect();
        }
        default:
            throw cRuntimeError("Unknown CRC mode");
    }
}

} // namespace inet
