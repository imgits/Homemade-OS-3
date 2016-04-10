#include"std.h"

typedef union{
	uint32_t value;
	uint8_t bytes[4];
}IPV4Address;

enum IPDataProtocol{
	IP_DATA_PROTOCOL_ICMP = 1,
	IP_DATA_PROTOCOL_TCP = 6,
	IP_DATA_PROTOCOL_UDP = 17,
	IP_DATA_PROTOCOL_TEST253 = 253,
	IP_DATA_PROTOCOL_TEST254 = 254
};

// big endian and most significant bit comes first
typedef struct{
	uint8_t headerLength: 4;
	uint8_t version: 4;
	uint8_t congestionNotification: 2;
	uint8_t differentiateService: 6;
	uint16_t totalLength;
	uint16_t identification;
	uint8_t fragmentOffsetHigh: 5;
	uint8_t flags: 3;
	uint8_t fragmentOffsetLow;
	uint8_t timeToLive;
	uint8_t protocol;
	uint16_t headerChecksum;
	IPV4Address source;
	IPV4Address destination;
	//uint8_t options[];
	uint8_t payload[];
}IPV4Header;
/*
example
45 00 00 3c 1a 02 00 00 80 01
2e 6e c0 a8 38 01 c0 a8 38 ff
*/
#define MAX_IP_PACKET_SIZE ((1 << 16) - 1)

// does not check dataLength
void initIPV4Header(
	IPV4Header *h, uint16_t dataLength, IPV4Address srcAddress, IPV4Address dstAddress,
	enum IPDataProtocol dataProtocol
);

uintptr_t getIPDataSize(const IPV4Header *h);

// return little endian number
uint16_t calculatePseudoIPChecksum(const IPV4Header *h);

// one receive queue & task for every socket
typedef struct IPSocket{
	IPV4Address source;
	IPV4Address destination;

	struct RWIPQueue *receive;
}IPSocket;

int initIPSocket(IPSocket *socket, unsigned *src);
void destroyIPSocket(IPSocket *socket);

void initUDP(void);
