//
// Copyright (C) OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#define WANT_WINSOCK2

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <omnetpp/platdep/sockets.h>

#include "inet/common/ModuleAccess.h"
#include "inet/common/packet/Packet.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/emulation/networklayer/ipv4/ExtIpv4TunDeviceFileIo.h"
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"

namespace inet {

Define_Module(ExtIpv4TunDeviceFileIo);

ExtIpv4TunDeviceFileIo::~ExtIpv4TunDeviceFileIo()
{
    closeTap();
}

void ExtIpv4TunDeviceFileIo::initialize(int stage)
{
    cSimpleModule::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        device = par("device").stdstringValue();
        packetName = par("packetName").stdstringValue();
        rtScheduler = check_and_cast<RealTimeScheduler *>(getSimulation()->getScheduler());
        openTap(device);
        numSent = numReceived = 0;
        WATCH(numSent);
        WATCH(numReceived);
    }
}

void ExtIpv4TunDeviceFileIo::handleMessage(cMessage *msg)
{
    auto packet = check_and_cast<Packet *>(msg);
    emit(packetReceivedFromLowerSignal, packet);
    auto protocol = packet->getTag<PacketProtocolTag>()->getProtocol();
    if (protocol != &Protocol::ethernetMac)
        throw cRuntimeError("ExtInterface accepts ethernet packets only");
    const auto& ethHeader = packet->peekAtFront<EthernetMacHeader>();
    packet->popAtBack<EthernetFcs>(ETHER_FCS_BYTES);
    auto bytesChunk = packet->peekDataAsBytes();
    uint8_t buffer[packet->getByteLength() + 4];
    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0x86; // ethernet
    buffer[3] = 0xdd;
    size_t packetLength = bytesChunk->copyToBuffer(buffer + 4, sizeof(buffer) - 4);
    ASSERT(packetLength == (size_t)packet->getByteLength());
    packetLength += 4;
    ssize_t nwrite = write(fd, buffer, packetLength);
    if ((size_t)nwrite == packetLength) {
        emit(packetSentSignal, packet);
        EV_INFO << "Sent a " << packet->getTotalLength() << " packet from " << ethHeader->getSrc() << " to " << ethHeader->getDest() << " to tap device '" << device << "'.\n";
        numSent++;
    }
    else
        EV_ERROR << "Sending of an ethernet packet FAILED! (sendto returned " << nwrite << " (" << strerror(errno) << ") instead of " << packetLength << ").\n";
    delete packet;
}

void ExtIpv4TunDeviceFileIo::refreshDisplay() const
{
    char buf[180];
    sprintf(buf, "tap device: %s\nrcv:%d snt:%d", device.c_str(), numReceived, numSent);
    getDisplayString().setTagArg("t", 0, buf);
}

void ExtIpv4TunDeviceFileIo::finish()
{
    EV_INFO << numSent << " packets sent, " << numReceived << " packets received.\n";
    closeTap();
}

void ExtIpv4TunDeviceFileIo::openTap(std::string dev)
{
    if ((fd = open("/dev/net/tun", O_RDWR)) < 0)
        throw cRuntimeError("Cannot open TAP device: %s", strerror(errno));

    // preparation of the struct ifr, of type "struct ifreq"
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP; /* IFF_TUN or IFF_TAP, plus maybe IFF_NO_PI */
    if (!dev.empty())
        /* if a device name was specified, put it in the structure; otherwise,
         * the kernel will try to allocate the "next" device of the
         * specified type */
        strncpy(ifr.ifr_name, dev.c_str(), IFNAMSIZ);
    if (ioctl(fd, (TUNSETIFF), (void *) &ifr) < 0) {
        close(fd);
        throw cRuntimeError("Cannot create TAP device: %s", strerror(errno));
    }

    /* if the operation was successful, write back the name of the
     * interface to the variable "dev", so the caller can know
     * it. Note that the caller MUST reserve space in *dev (see calling
     * code below) */
    dev = ifr.ifr_name;
    rtScheduler->addCallback(fd, this);
}

void ExtIpv4TunDeviceFileIo::closeTap()
{
    if (fd != INVALID_SOCKET) {
        rtScheduler->removeCallback(fd, this);
        close(fd);
        fd = -1;
    }
}

bool ExtIpv4TunDeviceFileIo::notify(int fd)
{
    Enter_Method_Silent();
    ASSERT(fd == this->fd);
    uint8_t buffer[1 << 16];
    ssize_t nread = read(fd, buffer, sizeof(buffer));
    if (nread < 0) {
        close(fd);
        throw cRuntimeError("Cannot read '%s' device: %s", device.c_str(), strerror(errno));
    }
    else if (nread > 0) {
        ASSERT (nread > 4);
        std::string completePacketName = packetName + std::to_string(numReceived);
        // buffer[0..1]: flags, buffer[2..3]: ethertype
        Packet *packet = new Packet(completePacketName.c_str(), makeShared<BytesChunk>(buffer + 4, nread - 4));
        EtherEncap::addPaddingAndFcs(packet, FCS_COMPUTED);
        packet->addTag<DispatchProtocolReq>()->setProtocol(&Protocol::ethernetMac);
        packet->addTag<PacketProtocolTag>()->setProtocol(&Protocol::ethernetMac);
        emit(packetReceivedSignal, packet);
        const auto& macHeader = packet->peekAtFront<EthernetMacHeader>();
        EV_INFO << "Received a " << packet->getTotalLength() << " packet from " << macHeader->getSrc() << " to " << macHeader->getDest() << ".\n";
        send(packet, "lowerLayerOut");
        emit(packetSentToLowerSignal, packet);
        numReceived++;
        return true;
    }
    else
        return false;
}

} // namespace inet

