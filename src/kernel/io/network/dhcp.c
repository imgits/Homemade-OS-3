#include"std.h"
#include"multiprocessor/processorlocal.h"
#include"task/task.h"
#include"network.h"

struct DHCPClient{
	uintptr_t udpFile;
	uint64_t macAddress;
	IPConfig *outputIPConfig;
};

static void dhcpClientTask(void *arg);

static uintptr_t openDHCPOnDevice(const FileEnumeration *fe){
	char udpName[MAX_FILE_ENUM_NAME_LENGTH] = "udp:255.255.255.255:67;src=0.0.0.0:68;dev=";
	int printLength = strlen(udpName);
	EXPECT(fe->nameLength <= LENGTH_OF(udpName) - printLength);
	memcpy(udpName + printLength, fe->name, fe->nameLength * sizeof(fe->name[0]));
	printLength += fe->nameLength;
	uintptr_t fileHandle = syncOpenFileN(udpName, printLength, OPEN_FILE_MODE_WRITABLE);
	EXPECT(fileHandle != IO_REQUEST_FAILURE);
	return fileHandle;
	ON_ERROR;
	ON_ERROR;
	return IO_REQUEST_FAILURE;
}

DHCPClient *createDHCPClient(const FileEnumeration *fe, IPConfig *ipConfig, uint64_t macAddress){
	DHCPClient *NEW(dhcp);
	EXPECT(dhcp != NULL);
	dhcp->udpFile = openDHCPOnDevice(fe);
	EXPECT(dhcp->udpFile != IO_REQUEST_FAILURE);
	dhcp->outputIPConfig = ipConfig;
	dhcp->macAddress = macAddress;
	Task *t = createSharedMemoryTask(dhcpClientTask, &dhcp, sizeof(dhcp), processorLocalTask());
	EXPECT(t != NULL);
	resume(t);
	return dhcp;
	// terminate t
	ON_ERROR;
	syncCloseFile(dhcp->udpFile);
	ON_ERROR;
	DELETE(dhcp);
	ON_ERROR;
	return NULL;
}

enum DHCPMessageType{
	DHCP_TYPE_DISCOVER = 1,
	DHCP_TYPE_OFFER = 2,
	DHCP_TYPE_REQUEST = 3,
	// DHCP_TYPE_DECLINE = 4,
	DHCP_TYPE_ACK = 5
	// DHCP_TYPE_NACK = 6,
	// DHCP_TYPE_RELEASE = 7,
	// DHCP_TYPE_INFORM = 8
};

#pragma pack(1)

enum DHCPOptionCode{
	DHCP_OPTION_0 = 0,
	DHCP_OPTION_SUBNET_MASK = 1,
	DHCP_OPTION_ROUTER = 3,
	DHCP_OPTION_DNS_SERVER = 6,
	DHCP_REQUESTED_IP_ADDRESS = 50,
	DHCP_OPTION_LEASE_TIME = 51,
	DHCP_OPTION_MSG_TYPE = 53,
	DHCP_OPTION_SERVER = 54,
	DHCP_OPTION_PARAMETER = 55,
	DHCP_OPTION_END = 255
};

typedef struct __attribute__((__packed__)){
	uint8_t optionCode, optionSize;
	union{
		uint8_t parameter[0];
		uint8_t msgType;
		uint32_t value32;
		IPV4Address ipv4Address;
	};
}DHCPOption;

#pragma pack()

static_assert(sizeof(DHCPOption) == 6);

#define SET_DHCP_OPTION(O, OPTION_CODE, MEMBER, VALUE) do{\
	(O)->optionCode = (OPTION_CODE);\
	(O)->optionSize = sizeof((O)->MEMBER);\
	(O)->MEMBER = (VALUE);\
}while(0)

static DHCPOption *nextOption(DHCPOption *o){
	uintptr_t p = ((uintptr_t)o->parameter) + o->optionSize;
	return (DHCPOption*)p;
}

static DHCPOption *setDHCPOption_MsgType(DHCPOption *o, enum DHCPMessageType t){
	SET_DHCP_OPTION(o, DHCP_OPTION_MSG_TYPE, msgType, t);
	return nextOption(o);
}

static DHCPOption *setDHCPOption_Address(DHCPOption *o, enum DHCPOptionCode optCode, IPV4Address addr){
	SET_DHCP_OPTION(o, optCode, ipv4Address, addr);
	return nextOption(o);
}
/*
static DHCPOption *setDHCPOption_Router(DHCPOption *o, IPV4Address router){
	return setDHCPOption_Address(o, DHCP_OPTION_ROUTER, router);
}
*/
static DHCPOption *setDHCPOption_Server(DHCPOption *o, IPV4Address server){
	return setDHCPOption_Address(o, DHCP_OPTION_SERVER, server);
}

static DHCPOption *setDHCPOption_RequestAddress(DHCPOption *o, IPV4Address request){
	return setDHCPOption_Address(o, DHCP_REQUESTED_IP_ADDRESS, request);
}

/*
static DHCPOption *setDHCPOption_LeaseTime(DHCPOption *o, uint32_t leaseTime){
	SET_DHCP_OPTION(o, DHCP_OPTION_LEASE_TIME, value32, leaseTime);
	return nextOption(o);
}
*/

static_assert(sizeof(enum DHCPOptionCode) == sizeof(unsigned));

static DHCPOption *setDHCPOption_Parameter(DHCPOption *o, uint8_t paramCount, ...){
	o->optionCode = DHCP_OPTION_PARAMETER;
	o->optionSize = paramCount;
	va_list args;
	va_start(args, paramCount);
	uint8_t i;
	for(i = 0; i < paramCount; i++){
		o->parameter[i] = va_arg(args, unsigned);
	}
	va_end(args);
	return nextOption(o);
}

static DHCPOption *setDHCPOption_End(DHCPOption *o){
	o->optionCode = DHCP_OPTION_END;
	o->optionSize = 0;
	return nextOption(o);
}

typedef struct{
	uint8_t operationCode; // 1: request; 2: reply
	uint8_t hardwareType; // 1: Ethernet
	uint8_t hardwareAddressLength; // 6
	uint8_t hops;
	uint32_t transactionID;
	uint16_t seconds;
	uint16_t flags;
	IPV4Address clientAddress;
	IPV4Address yourAddress;
	IPV4Address serverAddress;
	IPV4Address gatewayAddress;
	uint8_t clientHardwareAddress[16];
	uint8_t unused[192];
	uint32_t magic; // 0x63825363 in big endian
	DHCPOption option[];
}DHCPPacket;

#define DHCP_MAGIC ((uint32_t)0x63538263)

static_assert(sizeof(DHCPPacket) == 240);

static void initDHCPPacket(DHCPPacket *p, uint32_t id, uint64_t macAddress){
	p->operationCode = 1;
	p->hardwareType = 1;
	p->hardwareAddressLength = 6;
	p->hops = 0;
	p->transactionID = id;
	p->seconds = 0;
	p->flags = changeEndian16(0x8000);
	p->clientAddress = (IPV4Address)(uint32_t)0;
	p->yourAddress = (IPV4Address)(uint32_t)0;
	p->serverAddress = (IPV4Address)(uint32_t)0;
	p->gatewayAddress = (IPV4Address)(uint32_t)0;
	memset(p->clientHardwareAddress, 0, sizeof(p->clientHardwareAddress));
	toMACAddress(p->clientHardwareAddress, macAddress);
	memset(p->unused, 0, sizeof(p->unused));
	p->magic = DHCP_MAGIC;
}

static uintptr_t initDHCPDiscoverPacket(DHCPPacket *p, __attribute((__unused__)) uintptr_t packetSize, uint32_t id, uint64_t macAddress){
	initDHCPPacket(p, id, macAddress);
	DHCPOption *opt = p->option;
	opt = setDHCPOption_MsgType(opt, DHCP_TYPE_DISCOVER);
	opt = setDHCPOption_Parameter(opt, 3, DHCP_OPTION_SERVER, DHCP_OPTION_SUBNET_MASK, DHCP_OPTION_DNS_SERVER);
	opt = setDHCPOption_End(opt);//TODO: check size when appending options
	assert(((uintptr_t)opt) - ((uintptr_t)p) <= packetSize);
	return ((uintptr_t)opt) - ((uintptr_t)p);
}

static uintptr_t initDHCPRequestPacket(
	DHCPPacket *p, __attribute((__unused__)) uintptr_t packetSize, uint32_t id, uint64_t macAddress,
	IPV4Address requestAddress,IPV4Address dhcpServer
){
	initDHCPPacket(p, id, macAddress);
	DHCPOption *opt = p->option;
	opt = setDHCPOption_MsgType(opt, DHCP_TYPE_REQUEST);
	opt = setDHCPOption_RequestAddress(opt, requestAddress);
	opt = setDHCPOption_Server(opt, dhcpServer);
	opt = setDHCPOption_Parameter(opt, 3, DHCP_OPTION_SERVER, DHCP_OPTION_SUBNET_MASK, DHCP_OPTION_DNS_SERVER);
	opt = setDHCPOption_End(opt);
	assert(((uintptr_t)opt) - ((uintptr_t)p) <= packetSize);
	return ((uintptr_t)opt) - ((uintptr_t)p);
}

static int sendDHCPRequest(uintptr_t udpFile, DHCPPacket *packet, uintptr_t packetSize){
	uintptr_t r = syncSetFileParameter(udpFile, FILE_PARAM_DESTINATION_ADDRESS, 0xffffffff);
	if(r == IO_REQUEST_FAILURE)
		return 0;
	uintptr_t readSize = packetSize;
	r = syncWriteFile(udpFile, packet, &readSize);
	if(r == IO_REQUEST_FAILURE || packetSize != readSize)
		return 0;
	return 1;
}

// return number of returned values, according to the order of the arguments
static int parseDHCPOffer(
	const DHCPPacket *p, uintptr_t packetSize,
	uint32_t id, enum DHCPMessageType msgType,
	IPV4Address *offerAddress,
	IPV4Address *dhcpServerAddress,
	uintptr_t *leaseTime,
	IPV4Address *subnetMask,
	IPV4Address *gatewayAddress,
	IPV4Address *dnsServerAddress
	//IPV4Address *dnsServerAddress2
){
	if(packetSize < sizeof(*p) || p->operationCode != 2 || p->transactionID != id || p->magic != DHCP_MAGIC)
		return 0;
	*offerAddress = p->yourAddress;
	*dhcpServerAddress = p->serverAddress;
	*leaseTime = 0;
	*subnetMask = ANY_IPV4_ADDRESS;
	*gatewayAddress = ANY_IPV4_ADDRESS;
	*dnsServerAddress = ANY_IPV4_ADDRESS;

	uint8_t packetMsgType = 0; // invalid
	uintptr_t pos = ((uintptr_t)p->option) - ((uintptr_t)p);
	while(1){
		// option code
		if(pos + 1 > packetSize)
			return 0;
		DHCPOption *opt = (DHCPOption*)(((uintptr_t)p) + pos);
		pos++;
		if(opt->optionCode == DHCP_OPTION_END){
			break;
		}
		if(opt->optionCode == DHCP_OPTION_0){
			continue;
		}
		// option size
		if(pos + 1 > packetSize){
			return 0;
		}
		if(pos + opt->optionSize > packetSize){
			return 0;
		}
		pos += 1 + opt->optionSize;
		// TODO: minimum option size for each type
		switch(opt->optionCode){
		case DHCP_OPTION_MSG_TYPE:
			packetMsgType = opt->msgType;
			break;
		case DHCP_OPTION_SUBNET_MASK:
			*subnetMask = opt->ipv4Address;
			break;
		case DHCP_OPTION_ROUTER:
			*gatewayAddress = opt->ipv4Address;
			break;
		case DHCP_OPTION_DNS_SERVER: // may be multiple
			*dnsServerAddress = opt->ipv4Address;
			break;
		case DHCP_REQUESTED_IP_ADDRESS:
			*offerAddress = opt->ipv4Address;
			break;
		case DHCP_OPTION_LEASE_TIME:
			*leaseTime = changeEndian32(opt->value32);
			break;
		case DHCP_OPTION_SERVER:
			*dhcpServerAddress = opt->ipv4Address;
			break;
		}
	}
	if(packetMsgType != msgType)
		return 0;
	if(offerAddress->value == ANY_IPV4_ADDRESS.value)
		return 0;
	if(dhcpServerAddress->value == ANY_IPV4_ADDRESS.value)
		return 1;
	if(*leaseTime == 0)
		return 2;
	if(subnetMask->value == ANY_IPV4_ADDRESS.value)
		return 3;
	if(gatewayAddress->value == ANY_IPV4_ADDRESS.value)
		return 4;
	if(dnsServerAddress->value == ANY_IPV4_ADDRESS.value)
		return 5;
	return 6;
}

// TODO: timeout
static int readDHCPReply(
	uintptr_t udpFile, DHCPPacket *packet, uintptr_t maxPacketSize,
	uint32_t id, enum DHCPMessageType msgType,
	IPConfig *ipConf, uintptr_t *leaseTime){
	while(1){
		uintptr_t r = syncSetFileParameter(udpFile, FILE_PARAM_DESTINATION_ADDRESS, ANY_IPV4_ADDRESS.value);
		if(r == IO_REQUEST_FAILURE)
			return 0;
		uintptr_t readSize = maxPacketSize;
		r = syncReadFile(udpFile, packet, &readSize);
		if(r == IO_REQUEST_FAILURE)
			return 0;
		//if(rwSize == maxPacketSize){
		//	printk("warning: DHCP packet too big %u\n", rwSize);
		//}
		MEMSET0(ipConf);
		int parseCount = parseDHCPOffer(packet, readSize, id, msgType,
			&ipConf->localAddress, &ipConf->dhcpServer, leaseTime, &ipConf->subnetMask,
			&ipConf->gateway, &ipConf->dnsServer);
		if(parseCount >= 4)
			return 1;
	}
}

#define MAX_DHCP_OPTION_COUNT (30)
// return the lease time
static int dhcpTransaction(DHCPClient *dhcp, uint32_t id){
	const uintptr_t maxPacketSize = sizeof(DHCPPacket) + sizeof(DHCPOption) * MAX_DHCP_OPTION_COUNT;
	DHCPPacket *packet = allocateKernelMemory(maxPacketSize);
	EXPECT(packet != NULL);
	uintptr_t packetSize = initDHCPDiscoverPacket(packet, maxPacketSize, id, dhcp->macAddress);
	int ok = sendDHCPRequest(dhcp->udpFile, packet, packetSize);
	// from 0.0.0.0:68 to 255.255.255.255:67
	EXPECT(ok);
	IPConfig ipConf;
	uintptr_t leaseTime;
	ok =readDHCPReply(dhcp->udpFile, packet, maxPacketSize, id, DHCP_TYPE_OFFER, &ipConf, &leaseTime);
	EXPECT(ok);

	packetSize = initDHCPRequestPacket(packet, maxPacketSize, id, dhcp->macAddress, ipConf.localAddress, ipConf.dhcpServer);
	ok = sendDHCPRequest(dhcp->udpFile, packet, packetSize);
	EXPECT(ok);

	ok= readDHCPReply(dhcp->udpFile, packet, maxPacketSize, id, DHCP_TYPE_ACK, &ipConf, &leaseTime);
	EXPECT(ok);

	//TODO: set device IP
	printk("offer addr : %x\n", ipConf.localAddress.value);
	printk("dhcp server: %x\n", ipConf.dhcpServer.value);
	printk("lease time : %d\n", leaseTime);
	printk("subnet mask: %x\n", ipConf.subnetMask.value);
	printk("gateway    : %x\n", ipConf.gateway.value);
	printk("dns server : %x\n", ipConf.dnsServer.value);
	DELETE(packet);
	return leaseTime;
	ON_ERROR;
	ON_ERROR;
	ON_ERROR;
	ON_ERROR;
	DELETE(packet);
	ON_ERROR;
	printk("warning: DHCP request failed. Retry in 5 seconds.");
	return 5000;
}

static void dhcpClientTask(void *arg){
	DHCPClient *dhcp = *(DHCPClient**)arg;
	printk("DHCP client started\n");
	uint32_t id0 = ((uint32_t)&arg);
	uint32_t i;
	for(i = 0; 1; i = (i + 1) % 10){
		uintptr_t leaseTime = dhcpTransaction(dhcp, id0 + i);
		sleep(leaseTime * (1000 / 2));
	}
	systemCall_terminate();
}
