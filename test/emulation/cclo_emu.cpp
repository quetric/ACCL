/*******************************************************************************
#  Copyright (C) 2021 Xilinx, Inc
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# *******************************************************************************/

#include "streamdefines.h"
#include "Stream.h"
#include "Simulation.h"
#include "Axi.h"
#include <pthread.h>
#include <thread>
#include <iostream>
#include <sstream>
#include <fstream>
#include "ap_int.h"
#include <stdint.h>
#include "reduce_sum.h"
#include "eth_intf.h"
#include "dummy_tcp_stack.h"
#include "stream_segmenter.h"
#include "rxbuf_offload.h"
#include "dma_mover.h"
#include "ccl_offload_control.h"
#include <zmqpp/zmqpp.hpp>
#include <string>
#include <jsoncpp/json/json.h>
#include <chrono>
#include <numeric>
#include <mpi.h>
#include "zmq_intf.h"

using namespace std;
using namespace hlslib;

void dma_read(vector<char> &mem, Stream<ap_uint<104> > &cmd, Stream<ap_uint<32> > &sts, Stream<stream_word > &rdata){
    axi::Command<64, 23> command = axi::Command<64, 23>(cmd.Pop());
    axi::Status status;
    stream_word tmp;
    stringstream ss;
    ss << "DMA Read: Command popped. length: " << command.length << " offset: " << command.address << "\n";
    cout << ss.str();
    int byte_count = 0;
    while(byte_count < command.length){
        tmp.keep = 0;
        for(int i=0; i<64 && byte_count < command.length; i++){
            tmp.data(8*(i+1)-1, 8*i) = mem.at(command.address+byte_count);
            tmp.keep(i,i) = 1;
            byte_count++;
        }
        tmp.last = (byte_count >= command.length);
        rdata.Push(tmp);
    }
    status.okay = 1;
    status.tag = command.tag;
    sts.Push(status);
    ss.str(string());
    ss << "DMA Read: Status pushed" << "\n";
    cout << ss.str();
}

void dma_write(vector<char> &mem, Stream<ap_uint<104> > &cmd, Stream<ap_uint<32> > &sts, Stream<stream_word > &wdata){
    axi::Command<64, 23> command = axi::Command<64, 23>(cmd.Pop());
    axi::Status status;
    stream_word tmp;
    stringstream ss;
    ss << "DMA Write: Command popped. length: " << command.length << " offset: " << command.address << "\n";
    cout << ss.str();
    int byte_count = 0;
    while(byte_count<command.length){
        tmp = wdata.Pop();
        for(int i=0; i<64; i++){
            if(tmp.keep(i,i) == 1){
                mem.at(command.address+byte_count) = tmp.data(8*(i+1)-1, 8*i);
                byte_count++;
            }
        }
        //end of packet
        if(tmp.last){
            status.endOfPacket = 1;
            break;
        }
    }
    //TODO: flush?
    status.okay = 1;
    status.tag = command.tag;
    status.bytesReceived = byte_count;
    sts.Push(status);
    ss.str(string());
    ss << "DMA Write: Status pushed endOfPacket=" << status.endOfPacket << " btt=" << status.bytesReceived << "\n";
    cout << ss.str();
}

template <unsigned int INW, unsigned int OUTW, unsigned int DESTW>
void dwc(Stream<ap_axiu<INW, 0, 0, DESTW> > &in, Stream<ap_axiu<OUTW, 0, 0, DESTW> > &out){
    ap_axiu<INW, 0, 0, DESTW> inword;
    ap_axiu<OUTW, 0, 0, DESTW> outword;

    //3 scenarios:
    //1:N (up) conversion - read N times from input, write 1 times to output
    //N:1 (down) conversion - read 1 times from input, write N times to output
    //N:M conversion - up-conversion to least common multiple, then down-conversion
    if(INW < OUTW && OUTW%INW == 0){
        //1:N case
        outword.keep = 0;
        outword.last = 0;
        outword.data = 0;
        outword.dest = inword.dest;
        for(int i=0; i<OUTW/INW; i++){
            inword = in.Pop();
            outword.data((i+1)*INW-1,i*INW) = inword.data;
            outword.keep((i+1)*INW/8-1,i*INW/8) = inword.keep;
            outword.last = 1;
            if((inword.last == 1) || (inword.keep(INW/8-1,INW/8-1) != 1)) break;
        }
        out.Push(outword);
    } else if(INW > OUTW && INW%OUTW == 0){
        //N:1 case
        inword = in.Pop();
        outword.dest = inword.dest;
        for(int i=0; i<INW/OUTW; i++){
            outword.data = inword.data((i+1)*OUTW-1,i*OUTW);
            outword.keep = inword.keep((i+1)*OUTW/8-1,i*OUTW/8);
            //last if actually at last input read or if any previous input read is incomplete
            outword.last = (i==(INW/OUTW-1)) || (outword.keep(OUTW/8-1,OUTW/8-1) != 1);
            out.Push(outword);
            if(outword.last == 1) break;
        }
    } else{
        unsigned const int inter_width = lcm(INW, OUTW);
        Stream<axi::Stream<ap_axiu<inter_width, 0, 0, 0> > > inter;
        dwc<INW, inter_width>(in, inter);
        dwc<inter_width, OUTW>(inter, out);
    }
}

void arithmetic(Stream<stream_word > &op0, Stream<stream_word > &op1, Stream<stream_word > &res){
    Stream<ap_axiu<2*DATA_WIDTH,0,0,DEST_WIDTH> > op_int("arith_op");
    stream_word tmp_op0;
    stream_word tmp_op1;
    ap_axiu<2*DATA_WIDTH,0,0,DEST_WIDTH> tmp_op;
    stream_word tmp_res;

    //load op stream
    do {
        tmp_op0 = op0.Pop();
        tmp_op1 = op1.Pop();
        tmp_op.data(511,0) = tmp_op0.data;
        tmp_op.keep(63,0) = tmp_op0.keep;
        tmp_op.data(1023,512) = tmp_op1.data;
        tmp_op.keep(127,64) = tmp_op1.keep;
        tmp_op.last = tmp_op0.last;
        op_int.write(tmp_op);
    } while(tmp_op0.last == 0);
    cout << "Arith packet received" << endl;
    //call arith
    switch(tmp_op0.dest){
        case 0:
            reduce_sum_float(op_int, res);
            break;
        case 1:
            reduce_sum_double(op_int, res);
            break;
        case 2:
            reduce_sum_int32_t(op_int, res);
            break;
        case 3:
            reduce_sum_int64_t(op_int, res);
            break;
        //half precision is problematic, no default support in C++
        // case 4:
        //     stream_add<512, half>(op_int, res_int);
        //     break;
    }
    //load result stream
    cout << "Arith packet processed" << endl;
}

void compression(Stream<stream_word> &op0, Stream<stream_word> &res){ 
    stream_word tmp_op0;
    stream_word tmp_res;

    tmp_op0 = op0.Pop();
    cout << "Running compression lane with TDEST=" << tmp_op0.dest << endl;
    switch(tmp_op0.dest){
        case 0:
            res.Push(tmp_op0);
            break;
        case 1://downcast
            for(int i=0; i<16; i++){
                tmp_res.data(16*(i+1)-1,16*i) = tmp_op0.data(32*(i+1)-1,32*i+16);
            }
            tmp_op0 = op0.Pop();
            for(int i=0; i<16; i++){
                tmp_res.data(16*(i+16+1)-1,16*(i+16)) = tmp_op0.data(32*(i+1)-1,32*i+16);
            }
            res.Push(tmp_res);
            break;
        case 2://upcast
            tmp_res.data = 0;
            for(int i=0; i<16; i++){
                tmp_res.data(32*(i+1)-1,32*i+16) = tmp_op0.data(16*(i+1)-1,16*i);
            }
            res.Push(tmp_res);
            tmp_res.data = 0;
            for(int i=0; i<16; i++){
                tmp_res.data(32*(i+1)-1,32*i) = tmp_op0.data(16*(i+16+1)-1,16*(i+16));
            }
            res.Push(tmp_res);
            break;
    }
}

//emulate an AXI Stream Switch with TDEST routing
template <unsigned int NSLAVES, unsigned int NMASTERS>
void axis_switch( Stream<stream_word> s[NSLAVES], Stream<stream_word> m[NMASTERS]){
    stream_word word;
    for(int i=0; i<NSLAVES; i++){
        if(!s[i].IsEmpty()){
            do{
                word = s[i].Pop();
                int d = min(NMASTERS-1, (unsigned int)word.dest);
                m[d].Push(word);
                stringstream ss;
                ss << "Switch arbitrate: S" << i << " -> M" << d << "(" << (unsigned int)word.dest << ")" << "\n";
                cout << ss.str();
            } while(word.last == 0);
        }
    }
}

//emulate an 2:1 AXI Stream Switch i.e. a AXIS multiplexer
void axis_mux(Stream<stream_word> &s0, Stream<stream_word> &s1, Stream<stream_word> &m){
    stream_word word;
    if(!s0.IsEmpty()){
        do{
            word = s0.Pop();
            m.Push(word);
            stringstream ss;
            ss << "Switch mux: S0 -> M (" << (unsigned int)word.dest << ")" << "\n";
            cout << ss.str();
        } while(word.last == 0);
    }
    if(!s1.IsEmpty()){
        do{
            word = s1.Pop();
            m.Push(word);
            stringstream ss;
            ss << "Switch mux: S1 -> M (" << (unsigned int)word.dest << ")" << "\n";
            cout << ss.str();
        } while(word.last == 0);
    }
}

void dummy_external_kernel(Stream<stream_word> &in, Stream<stream_word> &out){
    stream_word tmp, tmp_no_tdest;
    tmp = in.Pop();
    stringstream ss;
    ss << "External Kernel Interface: Read TDEST=" << tmp.dest << "\n";
    cout << ss.str();
    tmp_no_tdest = {.data = tmp.data, .keep = tmp.keep, .last = tmp.last};
    out.Push(tmp_no_tdest);
}

void sim_bd(zmq_intf_context *ctx, bool use_tcp, unsigned int local_rank, unsigned int world_size) {
    vector<char> devicemem;

    Stream<ap_uint<32>, 32> host_cmd("host_cmd");
    Stream<ap_uint<32>, 32> host_sts("host_sts");

    Stream<stream_word, 32> krnl_to_accl_data;
    Stream<stream_word > accl_to_krnl_data;

    Stream<stream_word, 1024> eth_rx_data;
    Stream<stream_word, 1024> eth_tx_data;

    Stream<stream_word > arith_op0;
    Stream<stream_word > arith_op1;
    Stream<stream_word > arith_res;

    Stream<stream_word > clane0_op;
    Stream<stream_word > clane0_res;

    Stream<stream_word > clane1_op;
    Stream<stream_word > clane1_res;

    Stream<stream_word > clane2_op;
    Stream<stream_word > clane2_res;

    Stream<ap_uint<104>, 32> dma_write_cmd_int[2];
    Stream<ap_uint<104>, 32> dma_read_cmd_int[2];
    Stream<ap_uint<32>, 32> dma_write_sts_int[2];
    Stream<ap_uint<32>, 32> dma_read_sts_int[2];
    Stream<stream_word > dma_read_data[2];

    Stream<stream_word > switch_s[8];
    Stream<stream_word > switch_m[10];
    Stream<segmenter_cmd> seg_cmd[13];
    Stream<ap_uint<32> > seg_sts[13];
    Stream<stream_word > accl_to_krnl_seg;

    Stream<eth_header > eth_tx_cmd;
    Stream<ap_uint<32> > eth_tx_sts;
    Stream<eth_header > eth_rx_sts;
    Stream<eth_header > eth_rx_sts_sess;

    Stream<ap_uint<32>, 32> inflight_rxbuf;
    Stream<ap_uint<32>, 32> inflight_rxbuf_sess;
    
    Stream<ap_uint<104>, 32> enq2sess_dma_cmd;
    Stream<ap_uint<32>, 32> sess2deq_dma_sts;

    Stream<rxbuf_notification> eth_rx_notif;
    Stream<rxbuf_signature> eth_rx_seek_req;
    Stream<rxbuf_seek_result> eth_rx_seek_ack;
    Stream<ap_uint<32> > rxbuf_release_req;

    Stream<pkt16> eth_listen_port;
    Stream<pkt8> eth_port_status;
    Stream<pkt64> eth_open_connection;
    Stream<pkt128> eth_open_status;

    Stream<ap_uint<96> > cmd_txHandler;
    Stream<pkt32> eth_tx_meta;
    Stream<pkt64> eth_tx_status;

    Stream<pkt128> eth_notif;
    Stream<pkt32> eth_read_pkg;
    Stream<pkt16> eth_rx_meta;
    Stream<eth_notification> eth_notif_out;
    Stream<eth_notification> eth_notif_out_dpkt;

    Stream<stream_word, 1024> eth_tx_data_int;
    Stream<stream_word, 1024> eth_rx_data_int;
    Stream<stream_word> eth_tx_data_stack;
    Stream<stream_word> eth_rx_data_stack;

    unsigned int max_words_per_pkt = MAX_PACKETSIZE/DATAPATH_WIDTH_BYTES;

    // Dataflow functions running in parallel
    HLSLIB_DATAFLOW_INIT();
    //DMA0
    HLSLIB_FREERUNNING_FUNCTION(dma_write, devicemem, dma_write_cmd_int[0], dma_write_sts_int[0], switch_m[SWITCH_M_DMA0_WRITE]);
    HLSLIB_FREERUNNING_FUNCTION(dma_read, devicemem, dma_read_cmd_int[0], dma_read_sts_int[0], dma_read_data[0]);
    //DMA1
    HLSLIB_FREERUNNING_FUNCTION(dma_write, devicemem, dma_write_cmd_int[1], dma_write_sts_int[1], switch_m[SWITCH_M_DMA1_WRITE]);
    HLSLIB_FREERUNNING_FUNCTION(dma_read, devicemem, dma_read_cmd_int[1], dma_read_sts_int[1], dma_read_data[1]);
    //RX buffer handling offload
    if(!use_tcp){
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_enqueue, dma_write_cmd_int[0], inflight_rxbuf, cfgmem);
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_dequeue, dma_write_sts_int[0], eth_rx_sts, inflight_rxbuf, eth_rx_notif, cfgmem);
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_seek, eth_rx_notif, eth_rx_seek_req, eth_rx_seek_ack, rxbuf_release_req, cfgmem);
    } else{
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_enqueue, enq2sess_dma_cmd, inflight_rxbuf, cfgmem);
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_dequeue, sess2deq_dma_sts, eth_rx_sts_sess, inflight_rxbuf_sess, eth_rx_notif, cfgmem);
        HLSLIB_FREERUNNING_FUNCTION(rxbuf_seek, eth_rx_notif, eth_rx_seek_req, eth_rx_seek_ack, rxbuf_release_req, cfgmem);
        HLSLIB_FREERUNNING_FUNCTION(
            rxbuf_session, 
            enq2sess_dma_cmd, sess2deq_dma_sts,
            inflight_rxbuf, inflight_rxbuf_sess,
            dma_write_cmd_int[0], dma_write_sts_int[0],
            eth_notif_out_dpkt,
            eth_rx_sts, eth_rx_sts_sess
        );
    }
    //move offload
    HLSLIB_FREERUNNING_FUNCTION(
        dma_mover, cfgmem, cmd_fifos[CMD_DMA_MOVE], sts_fifos[STS_DMA_MOVE],
        eth_rx_seek_req, eth_rx_seek_ack, rxbuf_release_req,
        dma_read_cmd_int[0], dma_read_cmd_int[1], dma_write_cmd_int[1], 
        dma_read_sts_int[0], dma_read_sts_int[1], dma_write_sts_int[1], 
        eth_tx_cmd, eth_tx_sts,
        seg_cmd[0], seg_cmd[1], seg_cmd[2], seg_cmd[3], seg_cmd[4], seg_cmd[5], 
        seg_cmd[6], seg_cmd[7], seg_cmd[8], seg_cmd[9], 
        seg_cmd[10], seg_cmd[11], seg_cmd[12], seg_sts[3]
    );
    //SWITCH and segmenters
    HLSLIB_FREERUNNING_FUNCTION(axis_switch<8, 10>, switch_s, switch_m);
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, dma_read_data[0],             switch_s[SWITCH_S_DMA0_READ], seg_cmd[0],  seg_sts[0] );   //DMA0 read
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, dma_read_data[1],             switch_s[SWITCH_S_DMA1_READ], seg_cmd[1],  seg_sts[1] );   //DMA1 read
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, krnl_to_accl_data,            switch_s[SWITCH_S_EXT_KRNL],  seg_cmd[2],  seg_sts[2] );   //ext kernel in
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_EXT_KRNL],  accl_to_krnl_seg,             seg_cmd[3],  seg_sts[3] );   //ext kernel out
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_ARITH_OP0], arith_op0,                    seg_cmd[4],  seg_sts[4] );   //arith op0
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_ARITH_OP1], arith_op1,                    seg_cmd[5],  seg_sts[5] );   //arith op1
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, arith_res,                    switch_s[SWITCH_S_ARITH_RES], seg_cmd[6],  seg_sts[6] );   //arith result 
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_CLANE0], clane0_op,                    seg_cmd[7],  seg_sts[7] );   //clane0 op    
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, clane0_res,                   switch_s[SWITCH_S_CLANE0], seg_cmd[8],  seg_sts[8] );   //clane0 result
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_CLANE1], clane1_op,                    seg_cmd[9], seg_sts[9]);   //clane1 op    
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, clane1_res,                   switch_s[SWITCH_S_CLANE1], seg_cmd[10], seg_sts[10]);   //clane1 result
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, switch_m[SWITCH_M_CLANE2], clane2_op,                    seg_cmd[11], seg_sts[11]);   //clane2 op    
    HLSLIB_FREERUNNING_FUNCTION(stream_segmenter, clane2_res,                   switch_s[SWITCH_S_CLANE2], seg_cmd[12], seg_sts[12]);   //clane2 result
    HLSLIB_FREERUNNING_FUNCTION(axis_mux, accl_to_krnl_seg, switch_m[SWITCH_M_BYPASS], accl_to_krnl_data);
    //ARITH
    HLSLIB_FREERUNNING_FUNCTION(arithmetic, arith_op0, arith_op1, arith_res);
    //COMPRESS 0, 1, 2
    HLSLIB_FREERUNNING_FUNCTION(compression, clane0_op, clane0_res);
    HLSLIB_FREERUNNING_FUNCTION(compression, clane1_op, clane1_res);
    HLSLIB_FREERUNNING_FUNCTION(compression, clane2_op, clane2_res);
    //network PACK/DEPACK
    if(use_tcp){
        HLSLIB_FREERUNNING_FUNCTION(tcp_packetizer, switch_m[SWITCH_M_ETH_TX], eth_tx_data_int, eth_tx_cmd, cmd_txHandler, eth_tx_sts, max_words_per_pkt);
        HLSLIB_FREERUNNING_FUNCTION(tcp_depacketizer, eth_rx_data_int, switch_s[SWITCH_S_ETH_RX], eth_rx_sts, eth_notif_out, eth_notif_out_dpkt);
        HLSLIB_FREERUNNING_FUNCTION(tcp_rxHandler, eth_notif,  eth_read_pkg, eth_rx_meta,  eth_rx_data_stack, eth_rx_data_int, eth_notif_out);
        HLSLIB_FREERUNNING_FUNCTION(tcp_txHandler, eth_tx_data_int, cmd_txHandler, eth_tx_meta,  eth_tx_data_stack,  eth_tx_status);
        HLSLIB_FREERUNNING_FUNCTION(
            tcp_sessionHandler,
            cmd_fifos[CMD_NET_PORT], sts_fifos[STS_NET_PORT],
            cmd_fifos[CMD_NET_CON], sts_fifos[STS_NET_CON],
            eth_listen_port, eth_port_status,
            eth_open_connection, eth_open_status
        );
        //instantiate dummy TCP stack which responds to appropriate comm patterns
        HLSLIB_FREERUNNING_FUNCTION(
            network_krnl,
            eth_notif, eth_read_pkg,
            eth_rx_meta, eth_rx_data_stack,
            eth_tx_meta, eth_tx_data_stack, eth_tx_status,
            eth_open_connection, eth_open_status,
            eth_listen_port, eth_port_status,
            eth_rx_data, eth_tx_data
        );
    } else{
        HLSLIB_FREERUNNING_FUNCTION(udp_packetizer, switch_m[SWITCH_M_ETH_TX], eth_tx_data, eth_tx_cmd, eth_tx_sts, max_words_per_pkt);
        HLSLIB_FREERUNNING_FUNCTION(udp_depacketizer, eth_rx_data, switch_s[SWITCH_S_ETH_RX], eth_rx_sts);
    }
    //emulated external kernel
    HLSLIB_FREERUNNING_FUNCTION(dummy_external_kernel, accl_to_krnl_data, krnl_to_accl_data);
    //ZMQ to host process
    HLSLIB_FREERUNNING_FUNCTION(serve_zmq, ctx, cfgmem, devicemem, sts_fifos[CMD_CALL], cmd_fifos[STS_CALL]);
    //ZMQ to other nodes process(es)
    HLSLIB_FREERUNNING_FUNCTION(eth_endpoint_egress_port, ctx, eth_tx_data, local_rank, use_tcp && world_size > 1);
    HLSLIB_FREERUNNING_FUNCTION(eth_endpoint_ingress_port, ctx, eth_rx_data);
    //MICROBLAZE
    HLSLIB_DATAFLOW_FUNCTION(run_accl);
    HLSLIB_DATAFLOW_FINALIZE();

    this_thread::sleep_for(chrono::milliseconds(1000));
    cout << "Rank " << local_rank << " finished" << endl;
}

int main(int argc, char** argv){
    MPI_Init(NULL, NULL);      // initialize MPI environment

    int world_size; // number of processes
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    int local_rank; // the rank of the process
    MPI_Comm_rank(MPI_COMM_WORLD, &local_rank);

    string eth_type = argv[1];
    unsigned int starting_port = atoi(argv[2]);

    zmq_intf_context ctx = zmq_intf(starting_port, local_rank, world_size);
    sim_bd(&ctx, eth_type == "tcp", local_rank, world_size);
}
