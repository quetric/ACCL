# /*******************************************************************************
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

PLATFORM ?= xilinx_u280_xdma_201920_3
XSA := $(strip $(patsubst %.xpfm, % , $(shell basename $(PLATFORM))))
DEBUG ?= none
PROFILE ?= none
MODE ?= tcp
FREQUENCY = 250
CCL_XO=../../kernels/cclo/ccl_offload.xo

VNX=xup_vitis_network_example
NETLAYERDIR = $(VNX)/NetLayers
CMACDIR     = $(VNX)/Ethernet
NETLAYERHLS = $(NETLAYERDIR)/100G-fpga-network-stack-core

FW_SOURCES = $(shell find fw -name '*.c') $(shell find fw -name '*.h') $(shell find fw -name '*.tcl')

#At XACC ETHZ: ETH_IF 0 goes through network switch, 1 is direct connect
ETH_IF=0
CMAC_UDP_XO=$(VNX)/Ethernet/_x.$(PLATFORM)/cmac_$(ETH_IF).xo
UDP_XO=$(VNX)/NetLayers/_x.$(PLATFORM)/networklayer.xo
TCP_DUMMY_XO=../../kernels/plugins/dummy_tcp_stack/dummy_tcp_stack.xo
TCP_XO=Vitis_with_100Gbps_TCP-IP/_x.hw.$(XSA)/network_krnl.xo
CMAC_TCP_XO=Vitis_with_100Gbps_TCP-IP/_x.hw.$(XSA)/cmac_krnl.xo
XCLBIN=$(BUILD_DIR)/ccl_offload.xclbin

HOSTCTRL_XO=../../kernels/plugins/hostctrl/hostctrl.xo
SUM_XO=$(wildcard ../../kernels/plugins/reduce_sum/*.xo)
HP_UPCAST_XO=../../kernels/plugins/hp_fp_stream_conv/hp_fp_stream_conv.xo
HP_DOWNCAST_XO=../../kernels/plugins/fp_hp_stream_conv/fp_hp_stream_conv.xo
LOOPBACK_XO=../../kernels/plugins/loopback/loopback.xo

ARITHMETIC_XO = $(SUM_XO) $(HP_UPCAST_XO) $(HP_DOWNCAST_XO)

ifeq (all,$(PROFILE))
	PROFILE_OPTS := --profile.data all:all:all --profile.exec all:all:all --profile.stall all:all:all
else ifeq (data,$(PROFILE))
	PROFILE_OPTS := --profile.data all:all:all
else ifeq (exec,$(PROFILE))
	PROFILE_OPTS := --profile.exec all:all:all
else ifeq (stall,$(PROFILE))
	PROFILE_OPTS := --profile.stall all:all:all
else ifeq (none,$(PROFILE))
	PROFILE_OPTS :=
else
	$(error Unsupported PROFILE setting)
endif

ifeq (u250,$(findstring u250, $(PLATFORM)))
	FPGAPART=xcu250-figd2104-2L-e
	BOARD=u250
else ifeq (u280,$(findstring u280, $(PLATFORM)))
	FPGAPART=xcu280-fsvh2892-2L-e
	BOARD=u280
else
	$(error Unsupported PLATFORM)
endif

CONFIGFILE := config/link_config_$(BOARD)_$(MODE).ini
CONFIG := --config $(CONFIGFILE)
ifeq (vnx,$(MODE))
	CMAC_IP_FOLDER = $(shell readlink -f ./$(CMACDIR)/cmac)
	ifeq (u50,$(findstring u50, $(PLATFORM)))
		HLS_IP_FOLDER  = $(shell readlink -f ./$(NETLAYERHLS)/synthesis_results_HMB)
	endif
	ifeq (u200,$(findstring u200, $(PLATFORM)))
		HLS_IP_FOLDER  = $(shell readlink -f ./$(NETLAYERHLS)/synthesis_results_noHMB)
	endif
	ifeq (u250,$(findstring u250, $(PLATFORM)))
		HLS_IP_FOLDER  = $(shell readlink -f ./$(NETLAYERHLS)/synthesis_results_noHMB)
	endif
	ifeq (u280,$(findstring u280, $(PLATFORM)))
		HLS_IP_FOLDER  = $(shell readlink -f ./$(NETLAYERHLS)/synthesis_results_HMB)
	endif
	CONFIG += --connectivity.nk cmac_$(ETH_IF):1:cmac_inst
	CONFIG += --advanced.param compiler.userPostSysLinkOverlayTcl=$(shell pwd)/$(VNX)/post_sys_link.tcl
	NET_XO = $(CMAC_UDP_XO) $(UDP_XO)
	CONFIG += --user_ip_repo_paths $(CMAC_IP_FOLDER)
	CONFIG += --user_ip_repo_paths $(HLS_IP_FOLDER)
else ifeq (tri,$(MODE))
	ETH_IF := none
	NET_XO = $(TCP_DUMMY_XO)
	OTHER_XO = $(LOOPBACK_XO)
	CONFIG += --advanced.param compiler.userPostSysLinkOverlayTcl=$(shell pwd)/tcl/post_sys_link_tri.tcl
else ifeq (lb,$(MODE))
	ETH_IF := none
	NET_XO = $(TCP_DUMMY_XO)
	OTHER_XO = $(LOOPBACK_XO)
	CONFIG += --advanced.param compiler.userPostSysLinkOverlayTcl=$(shell pwd)/tcl/post_sys_link_lb.tcl
else ifeq (tcp, $(MODE))
	CMAC_TCP_XO=Vitis_with_100Gbps_TCP-IP/_x.hw.$(XSA)/cmac_krnl.xo
	NET_XO = $(TCP_XO) $(CMAC_TCP_XO)
	OTHER_XO = $(LOOPBACK_XO)
	CONFIG += --advanced.param compiler.userPostSysLinkOverlayTcl=$(shell pwd)/Vitis_with_100Gbps_TCP-IP/scripts/post_sys_link.tcl
	CONFIG += --user_ip_repo_paths Vitis_with_100Gbps_TCP-IP/build/fpga-network-stack/iprepo
else
	$(error Unsupported MODE)
endif

BUILD_DIR := link_$(MODE)_eth_$(ETH_IF)_debug_$(DEBUG)_$(XSA)

all: $(XCLBIN)

.PHONY: vnx
vnx: $(CMAC_UDP_XO) $(UDP_XO)

$(CMAC_UDP_XO) &:	
	git submodule update --init --recursive xup_vitis_network_example	
	$(MAKE) -C xup_vitis_network_example/Ethernet DEVICE=$(PLATFORM) INTERFACE=$(ETH_IF) all

$(UDP_XO):
	git submodule update --init --recursive xup_vitis_network_example
	$(MAKE) -C xup_vitis_network_example/NetLayers DEVICE=$(PLATFORM) all

$(CMAC_TCP_XO):
	git submodule update --init --recursive Vitis_with_100Gbps_TCP-IP
	$(MAKE) -C Vitis_with_100Gbps_TCP-IP/ cmac_krnl DEVICE=$(PLATFORM) TEMP_DIR=_x.hw.$(XSA)/ XSA=$(XSA)

$(TCP_DUMMY_XO):
	$(MAKE) -C ../../kernels/plugins/dummy_tcp_stack DEVICE=$(FPGAPART) all

.PHONY: network_krnl
network_krnl: $(TCP_XO)

$(TCP_XO):
	git submodule update --init --recursive Vitis_with_100Gbps_TCP-IP
	$(MAKE) -C Vitis_with_100Gbps_TCP-IP/ network_krnl DEVICE=$(PLATFORM) TEMP_DIR=_x.hw.$(XSA)/ XSA=$(XSA)

tcp_stack_ips:
	git submodule update --init --recursive Vitis_with_100Gbps_TCP-IP
	rm -rf Vitis_with_100Gbps_TCP-IP/build
	mkdir Vitis_with_100Gbps_TCP-IP/build
	cmake -S Vitis_with_100Gbps_TCP-IP/ -BVitis_with_100Gbps_TCP-IP/build/ -DFDEV_NAME=$(BOARD) -DVIVADO_HLS_ROOT_DIR=/proj/xbuilds/2020.1_released/installs/lin64/Vivado/2020.1 -DVIVADO_ROOT_DIR=/proj/xbuilds/2020.1_released/installs/lin64/Vivado/2020.1 -DTCP_STACK_EN=1 -DTCP_STACK_RX_DDR_BYPASS_EN=1  -DTCP_STACK_WINDOW_SCALING=0 
	make -C Vitis_with_100Gbps_TCP-IP/build installip

$(XCLBIN): $(NET_XO) $(CONFIGFILE)
	$(MAKE) -C ../../kernels PLATFORM=$(PLATFORM) DEBUG=$(DEBUG)
	v++ --link --platform $(PLATFORM) --kernel_frequency $(FREQUENCY) --save-temps --temp_dir $(BUILD_DIR) $(CONFIG) -o $@ $(CCL_XO) $(HOSTCTRL_XO) $(ARITHMETIC_XO) $(NET_XO) $(OTHER_XO)

distclean:
	git clean -xfd
