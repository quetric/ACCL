#include "ap_axi_sdata.h"
#include "hls_stream.h"
#include "ap_int.h"

using namespace hls;
using namespace std;

#define DWIDTH512 512
#define DWIDTH256 256
#define DWIDTH128 128
#define DWIDTH64 64
#define DWIDTH32 32
#define DWIDTH16 16
#define DWIDTH8 8

typedef ap_axiu<DWIDTH512, 0, 0, 0> pkt512;
typedef ap_axiu<DWIDTH256, 0, 0, 0> pkt256;
typedef ap_axiu<DWIDTH128, 0, 0, 0> pkt128;
typedef ap_axiu<DWIDTH64, 0, 0, 0> pkt64;
typedef ap_axiu<DWIDTH32, 0, 0, 0> pkt32;
typedef ap_axiu<DWIDTH16, 0, 0, 0> pkt16;
typedef ap_axiu<DWIDTH8, 0, 0, 0> pkt8;

void tcp_openConReq(	
			stream<ap_uint<32> > & cmd,
			stream<pkt64>& m_axis_tcp_open_connection)
{
#pragma HLS INTERFACE axis register both port=cmd
#pragma HLS INTERFACE axis register both port=m_axis_tcp_open_connection
#pragma HLS INTERFACE ap_ctrl_none port=return
	
	unsigned int ip = cmd.read();
	int port = cmd.read();	

	pkt64 openConnection_pkt;
    openConnection_pkt.data(31,0) = ip;
    openConnection_pkt.data(47,32) = port;
    m_axis_tcp_open_connection.write(openConnection_pkt);
    
}


