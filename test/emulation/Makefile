
ACCL_REPO_ROOT=$(shell pwd)/../../
HLSLIB_INCLUDE=$(ACCL_REPO_ROOT)/hlslib/include/hlslib/xilinx/
CCLO_HLS_ROOT=$(ACCL_REPO_ROOT)/kernels/cclo/hls
REDUCTION_DIR=$(ACCL_REPO_ROOT)/kernels/plugins/reduce_sum
DUMMY_TCP_DIR=$(ACCL_REPO_ROOT)/kernels/plugins/dummy_tcp_stack
CCLO_ETH_DIR=$(CCLO_HLS_ROOT)/eth_intf
SEGMENTER_DIR=$(CCLO_HLS_ROOT)/segmenter
RXBUF_OFFLOAD_DIR=$(CCLO_HLS_ROOT)/rxbuf_offload
DMA_MOVER_DIR=$(CCLO_HLS_ROOT)/dma_mover
MB_FW_DIR=$(ACCL_REPO_ROOT)/kernels/cclo/fw/sw_apps/ccl_offload_control/src
ZMQ_INTF_DIR=$(ACCL_REPO_ROOT)/test/zmq

STACKTYPE ?= "udp"
NRANKS ?=1
START_PORT ?= 5500

#Additional defines, for example: -DZMQ_CALL_VERBOSE
EXTRA_DEFINES:=

MPI_INCLUDES=-I/usr/lib/x86_64-linux-gnu/openmpi/include/openmpi -I/usr/lib/x86_64-linux-gnu/openmpi/include
MPI_LIBPATHS=-L/usr/lib/x86_64-linux-gnu/openmpi/lib

INCLUDES=$(MPI_INCLUDES) -I$(HLSLIB_INCLUDE) -I$(XILINX_HLS)/include/ -I$(REDUCTION_DIR) -I$(CCLO_ETH_DIR) -I$(SEGMENTER_DIR) -I$(MB_FW_DIR) -I$(CCLO_HLS_ROOT) -I$(DMA_MOVER_DIR) -I$(RXBUF_OFFLOAD_DIR) -I$(DUMMY_TCP_DIR) -I$(ZMQ_INTF_DIR)
SOURCES=cclo_emu.cpp $(MB_FW_DIR)/ccl_offload_control.c $(REDUCTION_DIR)/reduce_sum.cpp $(CCLO_ETH_DIR)/*.cpp $(SEGMENTER_DIR)/*.cpp $(RXBUF_OFFLOAD_DIR)/*.cpp $(DMA_MOVER_DIR)/*.cpp $(DUMMY_TCP_DIR)/*.cpp $(ZMQ_INTF_DIR)/*.cpp

all: cclo_emu

cclo_emu: cclo_emu.cpp $(MB_FW_DIR)/ccl_offload_control.c
	g++ -std=c++17 -Wno-attributes -fdiagnostics-color=always -g -DMB_FW_EMULATION $(EXTRA_DEFINES) $(INCLUDES) $(SOURCES) $(MPI_LIBPATHS) -o $@ -lpthread -lzmqpp -lzmq -ljsoncpp -lmpi_cxx -lmpi

.PHONY: run
run: cclo_emu
	mpirun -np ${NRANKS} --tag-output ./cclo_emu ${STACKTYPE} ${START_PORT} 2>/dev/null

