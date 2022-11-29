/*******************************************************************************
#  Copyright (C) 2022 Xilinx, Inc
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
#
*******************************************************************************/

#include <accl.hpp>
#include <cstdlib>
#include <experimental/xrt_ip.h>
#include <fstream>
#include <functional>
#include <json/json.h>
#include <mpi.h>
#include <random>
#include <roce/cmac.hpp>
#include <roce/hivenet.hpp>
#include <sstream>
#include <tclap/CmdLine.h>
#include <vector>
#include <vnx/cmac.hpp>
#include <vnx/mac.hpp>
#include <vnx/networklayer.hpp>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>

using namespace ACCL;

// Set the tolerance for compressed datatypes high enough, since we do currently
// not replicate the float32 -> float16 conversion for our reference results
#define FLOAT16RTOL 0.005
#define FLOAT16ATOL 0.05

int rank, size;
unsigned failed_tests;
unsigned skipped_tests;

struct options_t {
  int start_port;
  unsigned int rxbuf_size;
  unsigned int count;
  unsigned int nruns;
  unsigned int device_index;
  bool test_xrt_simulator;
  bool debug;
  bool hardware;
  bool axis3;
  bool udp;
  bool tcp;
  bool roce;
  std::string xclbin;
  std::string config_file;
  std::vector<std::string> ips;
};

void test_debug(std::string message, options_t &options) {
  if (options.debug) {
    std::cerr << message << std::endl;
  }
}

template <typename T>
void report_incorrect_item(int idx, T res, T ref, options_t &options) {
  test_debug(std::to_string(idx + 1) + "th item is incorrect! (" +
                 std::to_string(res) + " != " + std::to_string(ref) + ")" +
                 "\n",
             options);
}

void check_usage(int argc, char *argv[]) {}

std::string prepend_process() {
  return "[process " + std::to_string(rank) + "] ";
}

template <typename T>
bool is_close(T a, T b, double rtol = 1e-5, double atol = 1e-8) {
  // std::cout << abs(a - b) << " <= " << (atol + rtol * abs(b)) << "? " <<
  // (abs(a - b) <= (atol + rtol * abs(b))) << std::endl;
  return abs(a - b) <= (atol + rtol * abs(b));
}

template <typename T> static void random_array(T *data, size_t count) {
  std::uniform_real_distribution<T> distribution(-1000, 1000);
  std::mt19937 engine;
  auto generator = std::bind(distribution, engine);
  for (size_t i = 0; i < count; ++i) {
    data[i] = generator();
  }
}

template <typename T> std::unique_ptr<T> random_array(size_t count) {
  std::unique_ptr<T> data(new T[count]);
  random_array(data.get(), count);
  return data;
}

void test_copy(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start copy test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  accl.copy(*op_buf, *res_buf, count);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = (*op_buf)[i];
    float res = (*res_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << errors << " errors!" << std::endl;
  } else {
    std::cout << "Test succesfull!" << std::endl;
  }
}

void test_copy_stream(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start copy stream test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Copy data from buffer to stream", options);
  accl.copy_to_stream(*op_buf, count, false);
  test_debug("Copy data from stream to buffer", options);
  accl.copy_from_stream(*res_buf, count, false);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = (*op_buf)[i];
    float res = (*res_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << errors << " errors!" << std::endl;
  } else {
    std::cout << "Test succesfull!" << std::endl;
  }
}

void test_copy_p2p(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start copy p2p test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  std::unique_ptr<ACCL::Buffer<float>> p2p_buf;
  try {
    p2p_buf = accl.create_buffer_p2p<float>(count, dataType::float32);
  } catch (const std::bad_alloc &e) {
    std::cout << "Can't allocate p2p buffer (" << e.what() << "). "
              << "This probably means p2p is disabled on the FPGA.\n"
              << "Skipping p2p test..." << std::endl;
    skipped_tests += 1;
    return;
  }
  random_array(op_buf->buffer(), count);

  accl.copy(*op_buf, *p2p_buf, count);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = (*op_buf)[i];
    float res = (*p2p_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << errors << " errors!" << std::endl;
  } else {
    std::cout << "Test succesfull!" << std::endl;
  }
}

void test_combine_sum(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start combine SUM test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf1 = accl.create_buffer<float>(count, dataType::float32);
  auto op_buf2 = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf1->buffer(), count);
  random_array(op_buf2->buffer(), count);

  accl.combine(count, reduceFunction::SUM, *op_buf1, *op_buf2, *res_buf);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = (*op_buf1)[i] + (*op_buf2)[i];
    float res = (*res_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << errors << " errors!" << std::endl;
  } else {
    std::cout << "Test succesfull!" << std::endl;
  }
}

void test_combine_max(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start combine MAX test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf1 = accl.create_buffer<float>(count, dataType::float32);
  auto op_buf2 = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf1->buffer(), count);
  random_array(op_buf2->buffer(), count);

  accl.combine(count, reduceFunction::MAX, *op_buf1, *op_buf2, *res_buf);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = ((*op_buf1)[i] > (*op_buf2)[i]) ? (*op_buf1)[i] : (*op_buf2)[i];
    float res = (*res_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << errors << " errors!" << std::endl;
  } else {
    std::cout << "Test succesfull!" << std::endl;
  }
}

void test_sendrcv_bo(ACCL::ACCL &accl, xrt::device &dev, options_t &options) {
  std::cout << "Start send recv test bo..." << std::endl;
  unsigned int count = options.count;

  // Initialize bo
  float *data =
      static_cast<float *>(std::aligned_alloc(4096, count * sizeof(float)));
  float *validation_data =
      static_cast<float *>(std::aligned_alloc(4096, count * sizeof(float)));
  random_array(data, count);

  xrt::bo send_bo(dev, data, count * sizeof(float), accl.devicemem());
  xrt::bo recv_bo(dev, validation_data, count * sizeof(float),
                  accl.devicemem());
  auto op_buf = accl.create_buffer<float>(send_bo, count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(recv_bo, count, dataType::float32);
  int next_rank = (rank + 1) % size;
  int prev_rank = (rank + size - 1) % size;

  test_debug("Syncing buffers...", options);
  send_bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(next_rank) + "...",
             options);
  accl.send(*op_buf, count, next_rank, 0, GLOBAL_COMM, true);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.recv(*op_buf, count, prev_rank, 0, GLOBAL_COMM, true);

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.send(*op_buf, count, prev_rank, 1, GLOBAL_COMM, true);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(next_rank) + "...",
             options);
  accl.recv(*op_buf, count, next_rank, 1, GLOBAL_COMM, true);

  accl.copy(*op_buf, *res_buf, count, true, true);

  recv_bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float ref = validation_data[i];
    float res = data[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }

  std::free(data);
  std::free(validation_data);
}

void test_sendrcv(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start send recv test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  int next_rank = (rank + 1) % size;
  int prev_rank = (rank + size - 1) % size;

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(next_rank) + "...",
             options);
  accl.send(*op_buf, count, next_rank, 0);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.recv(*res_buf, count, prev_rank, 0);

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.send(*res_buf, count, prev_rank, 1);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(next_rank) + "...",
             options);
  accl.recv(*res_buf, count, next_rank, 1);

  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_sendrcv_stream(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start streaming send recv test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  int next_rank = (rank + 1) % size;
  int prev_rank = (rank + size - 1) % size;

  test_debug("Sending data on " + std::to_string(rank) + " to " +
             std::to_string(next_rank) + "...", options);
  accl.send(*op_buf, count, next_rank, 0);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
             std::to_string(prev_rank) + "...", options);
  accl.recv(dataType::float32, count, prev_rank, 0, GLOBAL_COMM);

  test_debug("Sending data on " + std::to_string(rank) + " to " +
             std::to_string(prev_rank) + "...", options);
  accl.send(dataType::float32, count, prev_rank, 1, GLOBAL_COMM);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
             std::to_string(next_rank) + "...", options);
  accl.recv(*res_buf, count, next_rank, 1);

  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_stream_put(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start stream put test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  int next_rank = (rank + 1) % size;
  int prev_rank = (rank + size - 1) % size;

  test_debug("Sending data on " + std::to_string(rank) + " to stream 0 on " +
             std::to_string(next_rank) + "...", options);
  accl.stream_put(*op_buf, count, next_rank, 9);

  test_debug("Sending data on " + std::to_string(rank) + " from stream to " +
             std::to_string(prev_rank) + "...", options);
  accl.send(dataType::float32, count, prev_rank, 1, GLOBAL_COMM);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
             std::to_string(next_rank) + "...", options);
  accl.recv(*res_buf, count, next_rank, 1);

  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_sendrcv_compressed(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start send recv compression test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  int next_rank = (rank + 1) % size;
  int prev_rank = (rank + size - 1) % size;

  int errors = 0;

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(next_rank) + "...",
             options);
  accl.send(*op_buf, count, next_rank, 0, GLOBAL_COMM, false,
            dataType::float16);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.recv(*res_buf, count, prev_rank, 0, GLOBAL_COMM, false,
            dataType::float16);

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i];
    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  test_debug("Sending data on " + std::to_string(rank) + " to " +
                 std::to_string(prev_rank) + "...",
             options);
  accl.send(*op_buf, count, prev_rank, 1, GLOBAL_COMM, false,
            dataType::float16);

  test_debug("Receiving data on " + std::to_string(rank) + " from " +
                 std::to_string(next_rank) + "...",
             options);
  accl.recv(*res_buf, count, next_rank, 1, GLOBAL_COMM, false,
            dataType::float16);

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i];
    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_bcast(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start bcast test with root " + std::to_string(root) + " ..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  if (rank == root) {
    test_debug("Broadcasting data from " + std::to_string(rank) + "...",
               options);
    accl.bcast(*op_buf, count, root);
  } else {
    test_debug("Getting broadcast data from " + std::to_string(root) + "...",
               options);
    accl.bcast(*res_buf, count, root);
  }

  if (rank != root) {
    int errors = 0;
    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i];
      if (res != ref) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_bcast_compressed(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start bcast compression test with root " +
                   std::to_string(root) + " ..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  if (rank == root) {
    test_debug("Broadcasting data from " + std::to_string(rank) + "...",
               options);
    accl.bcast(*op_buf, count, root, GLOBAL_COMM, false, false,
               dataType::float16);
  } else {
    test_debug("Getting broadcast data from " + std::to_string(root) + "...",
               options);
    accl.bcast(*res_buf, count, root, GLOBAL_COMM, false, false,
               dataType::float16);
  }

  if (rank != root) {
    int errors = 0;
    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i];
      if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_scatter(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start scatter test with root " + std::to_string(root) + " ..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count * size, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count * size);

  test_debug("Scatter data from " + std::to_string(rank) + "...", options);
  accl.scatter(*op_buf, *res_buf, count, root);

  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i + rank * count];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_scatter_compressed(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start scatter compression test with root " +
                   std::to_string(root) + " ..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count * size, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count * size);

  test_debug("Scatter data from " + std::to_string(rank) + "...", options);
  accl.scatter(*op_buf, *res_buf, count, root, GLOBAL_COMM, false, false,
               dataType::float16);

  int errors = 0;
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i + rank * count];
    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_gather(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start gather test with root " + std::to_string(root) + "..."
            << std::endl;
  unsigned int count = options.count;
  std::unique_ptr<float> host_op_buf = random_array<float>(count * size);
  auto op_buf = accl.create_buffer(host_op_buf.get() + count * rank, count,
                                   dataType::float32);
  std::unique_ptr<ACCL::Buffer<float>> res_buf;
  if (rank == root) {
    res_buf = accl.create_buffer<float>(count * size, dataType::float32);
  } else {
    res_buf = std::unique_ptr<ACCL::Buffer<float>>(nullptr);
  }

  test_debug("Gather data from " + std::to_string(rank) + "...", options);
  accl.gather(*op_buf, *res_buf, count, root);

  if (rank == root) {
    int errors = 0;
    for (unsigned int i = 0; i < count * size; ++i) {
      float res = (*res_buf)[i];
      float ref = host_op_buf.get()[i];
      if (res != ref) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_gather_compressed(ACCL::ACCL &accl, options_t &options, int root) {
  std::cout << "Start gather compression test with root " +
                   std::to_string(root) + "..."
            << std::endl;
  unsigned int count = options.count;
  std::unique_ptr<float> host_op_buf = random_array<float>(count * size);
  auto op_buf = accl.create_buffer(host_op_buf.get() + count * rank, count,
                                   dataType::float32);
  std::unique_ptr<ACCL::Buffer<float>> res_buf;
  if (rank == root) {
    res_buf = accl.create_buffer<float>(count * size, dataType::float32);
  } else {
    res_buf = std::unique_ptr<ACCL::Buffer<float>>(nullptr);
  }

  test_debug("Gather data from " + std::to_string(rank) + "...", options);
  accl.gather(*op_buf, *res_buf, count, root, GLOBAL_COMM, false, false,
              dataType::float16);

  if (rank == root) {
    int errors = 0;
    for (unsigned int i = 0; i < count * size; ++i) {
      float res = (*res_buf)[i];
      float ref = host_op_buf.get()[i];
      if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_allgather(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start allgather test..." << std::endl;
  unsigned int count = options.count;
  std::unique_ptr<float> host_op_buf = random_array<float>(count * size);
  auto op_buf = accl.create_buffer(host_op_buf.get() + count * rank, count,
                                   dataType::float32);
  auto res_buf = accl.create_buffer<float>(count * size, dataType::float32);

  test_debug("Gathering data...", options);
  accl.allgather(*op_buf, *res_buf, count);

  int errors = 0;
  for (unsigned int i = 0; i < count * size; ++i) {
    float res = (*res_buf)[i];
    float ref = host_op_buf.get()[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_allgather_compressed(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start allgather compression test..." << std::endl;
  unsigned int count = options.count;
  std::unique_ptr<float> host_op_buf = random_array<float>(count * size);
  auto op_buf = accl.create_buffer(host_op_buf.get() + count * rank, count,
                                   dataType::float32);
  auto res_buf = accl.create_buffer<float>(count * size, dataType::float32);

  test_debug("Gathering data...", options);
  accl.allgather(*op_buf, *res_buf, count, GLOBAL_COMM, false, false,
                 dataType::float16);

  int errors = 0;
  for (unsigned int i = 0; i < count * size; ++i) {
    float res = (*res_buf)[i];
    float ref = host_op_buf.get()[i];
    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_allgather_comms(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start allgather test with communicators..." << std::endl;
  unsigned int count = options.count;
  std::unique_ptr<float> host_op_buf(new float[count * size]);
  auto op_buf = accl.create_buffer(host_op_buf.get(), count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count * size, dataType::float32);

  for (unsigned int i = 0; i < count * size; i++) {
    host_op_buf.get()[i] = rank + i;
  }
  std::fill(res_buf->buffer(), res_buf->buffer() + count * size, 0);

  test_debug("Setting up communicators...", options);
  auto group = accl.get_comm_group(GLOBAL_COMM);
  unsigned int own_rank = accl.get_comm_rank(GLOBAL_COMM);
  unsigned int split = group.size() / 2;
  test_debug("Split is " + std::to_string(split), options);
  std::vector<rank_t>::iterator new_group_start;
  std::vector<rank_t>::iterator new_group_end;
  unsigned int new_rank = own_rank;
  bool is_in_lower_part = own_rank < split;
  if (is_in_lower_part) {
    new_group_start = group.begin();
    new_group_end = group.begin() + split;
  } else {
    new_group_start = group.begin() + split;
    new_group_end = group.end();
    new_rank = own_rank - split;
  }
  std::vector<rank_t> new_group(new_group_start, new_group_end);
  communicatorId new_comm = accl.create_communicator(new_group, new_rank);
  test_debug(accl.dump_communicator(), options);
  test_debug("Gathering data... count=" + std::to_string(count) +
                 ", comm=" + std::to_string(new_comm),
             options);
  accl.allgather(*op_buf, *res_buf, count, new_comm);
  test_debug("Validate data...", options);

  unsigned int data_split =
      is_in_lower_part ? count * split : count * size - count * split;
  int errors = 0;
  for (unsigned int i = 0; i < count * size; ++i) {
    float res = (*res_buf)[i];
    float ref;
    if (i < data_split) {
      ref = is_in_lower_part ? 0 : split;
      ref += (i / count) + (i % count);
    } else {
      ref = 0.0;
    }
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_multicomm(ACCL::ACCL &accl, options_t &options) {
  std::cout << "Start multi communicator test..." << std::endl;
  unsigned int count = options.count;
  auto group = accl.get_comm_group(GLOBAL_COMM);
  unsigned int own_rank = accl.get_comm_rank(GLOBAL_COMM);
  int errors = 0;
  if (group.size() < 4) {
    return;
  }
  if (own_rank == 1 || own_rank > 3) {
    return;
  }
  std::vector<rank_t> new_group;
  new_group.push_back(group[0]);
  new_group.push_back(group[2]);
  new_group.push_back(group[3]);
  unsigned int new_rank = (own_rank == 0) ? 0 : own_rank - 1;
  communicatorId new_comm = accl.create_communicator(new_group, new_rank);
  test_debug(accl.dump_communicator(), options);
  std::unique_ptr<float> host_op_buf = random_array<float>(count);
  auto op_buf = accl.create_buffer(host_op_buf.get(), count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  // start with a send/recv between ranks 0 and 2 (0 and 1 in the new
  // communicator)
  if (new_rank == 0) {
    accl.send(*op_buf, count, 1, 0, new_comm);
    accl.recv(*res_buf, count, 1, 1, new_comm);
    test_debug("Second recv completed", options);
    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = host_op_buf.get()[i];
      if (res != ref) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }
  } else if (new_rank == 1) {
    accl.recv(*res_buf, count, 0, 0, new_comm);
    test_debug("First recv completed", options);
    accl.send(*op_buf, count, 0, 1, new_comm);
  }
  std::cout << "Send/Recv with comms succeeded" << std::endl;
  // do an all-reduce on the new communicator
  for (unsigned int i = 0; i < count; ++i) {
    host_op_buf.get()[i] = i;
  }
  accl.allreduce(*op_buf, *res_buf, count, ACCL::reduceFunction::SUM, new_comm);
  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = 3 * host_op_buf.get()[i];
    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }
  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_reduce(ACCL::ACCL &accl, options_t &options, int root,
                 reduceFunction function) {
  std::cout << "Start reduce test with root " + std::to_string(root) +
                   " and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Reduce data to " + std::to_string(root) + "...", options);
  accl.reduce(*op_buf, *res_buf, count, root, function);

  if (rank == root) {
    int errors = 0;

    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i] * size;

      if (res != ref) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_reduce_compressed(ACCL::ACCL &accl, options_t &options, int root,
                            reduceFunction function) {
  std::cout << "Start reduce compression test with root " +
                   std::to_string(root) + " and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Reduce data to " + std::to_string(root) + "...", options);
  accl.reduce(*op_buf, *res_buf, count, root, function, GLOBAL_COMM, false,
              false, dataType::float16);

  if (rank == root) {
    int errors = 0;

    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i] * size;

      if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_reduce_stream2mem(ACCL::ACCL &accl, options_t &options, int root,
                            reduceFunction function) {
  std::cout << "Start stream to mem reduce test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Loading stream on rank" + std::to_string(rank) + "...", options);
  accl.copy_to_stream(*op_buf, count, false);
  test_debug("Reduce data to " + std::to_string(root) + "...", options);
  accl.reduce(dataType::float32, *res_buf, count, root, function);

  if (rank == root) {
    int errors = 0;

    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i] * size;

      if (res != ref) {
        report_incorrect_item<float>(i, res, ref, options);
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_reduce_mem2stream(ACCL::ACCL &accl, options_t &options, int root,
                            reduceFunction function) {
  std::cout << "Start mem to stream reduce test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  random_array(res_buf->buffer(), count);
  res_buf->sync_to_device();

  test_debug("Reduce data to " + std::to_string(root) + "...", options);
  accl.reduce(*op_buf, dataType::float32, count, root, function);

  if (rank == root) {
    int errors = 0;

    test_debug("Unloading stream on rank" + std::to_string(rank) + "...",
               options);
    accl.copy_from_stream(*res_buf, count, false);

    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i] * size;

      if (res != ref) {
        std::cout << std::to_string(i + 1) + "th item is incorrect! (" +
                         std::to_string(res) + " != " + std::to_string(ref) +
                         ")"
                  << std::endl;
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_reduce_stream2stream(ACCL::ACCL &accl, options_t &options, int root,
                               reduceFunction function) {
  std::cout << "Start stream to stream reduce test..." << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);
  random_array(res_buf->buffer(), count);
  res_buf->sync_to_device();

  test_debug("Loading stream on rank" + std::to_string(rank) + "...", options);
  accl.copy_to_stream(*op_buf, count, false);
  test_debug("Reduce data to " + std::to_string(root) + "...", options);
  accl.reduce(dataType::float32, dataType::float32, count, root, function);

  if (rank == root) {
    int errors = 0;

    test_debug("Unloading stream on rank" + std::to_string(rank) + "...",
               options);
    accl.copy_from_stream(*res_buf, count, false);

    for (unsigned int i = 0; i < count; ++i) {
      float res = (*res_buf)[i];
      float ref = (*op_buf)[i] * size;

      if (res != ref) {
        std::cout << std::to_string(i + 1) + "th item is incorrect! (" +
                         std::to_string(res) + " != " + std::to_string(ref) +
                         ")"
                  << std::endl;
        errors += 1;
      }
    }

    if (errors > 0) {
      std::cout << std::to_string(errors) + " errors!" << std::endl;
      failed_tests++;
    } else {
      std::cout << "Test is successful!" << std::endl;
    }
  }
}

void test_reduce_scatter(ACCL::ACCL &accl, options_t &options,
                         reduceFunction function) {
  std::cout << "Start reduce scatter test and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count * size, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count * size);

  test_debug("Reducing data...", options);
  accl.reduce_scatter(*op_buf, *res_buf, count, function);

  int errors = 0;
  int rank_prod = 0;

  for (int i = 0; i < rank; ++i) {
    rank_prod += i;
  }

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i + rank * count] * size;

    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_reduce_scatter_compressed(ACCL::ACCL &accl, options_t &options,
                                    reduceFunction function) {
  std::cout << "Start reduce scatter compression test and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count * size, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count * size);

  test_debug("Reducing data...", options);
  accl.reduce_scatter(*op_buf, *res_buf, count, function, GLOBAL_COMM, false,
                      false, dataType::float16);

  int errors = 0;
  int rank_prod = 0;

  for (int i = 0; i < rank; ++i) {
    rank_prod += i;
  }

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i + rank * count] * size;

    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_allreduce(ACCL::ACCL &accl, options_t &options,
                    reduceFunction function) {
  std::cout << "Start allreduce test and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Reducing data...", options);
  accl.allreduce(*op_buf, *res_buf, count, function);

  int errors = 0;

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i] * size;

    if (res != ref) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }

  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_allreduce_compressed(ACCL::ACCL &accl, options_t &options,
                               reduceFunction function) {
  std::cout << "Start allreduce compression test and reduce function " +
                   std::to_string(static_cast<int>(function)) + "..."
            << std::endl;
  unsigned int count = options.count;
  auto op_buf = accl.create_buffer<float>(count, dataType::float32);
  auto res_buf = accl.create_buffer<float>(count, dataType::float32);
  random_array(op_buf->buffer(), count);

  test_debug("Reducing data...", options);
  accl.allreduce(*op_buf, *res_buf, count, function, GLOBAL_COMM, false, false,
                 dataType::float16);

  int errors = 0;

  for (unsigned int i = 0; i < count; ++i) {
    float res = (*res_buf)[i];
    float ref = (*op_buf)[i] * size;

    if (!is_close(res, ref, FLOAT16RTOL, FLOAT16ATOL)) {
      report_incorrect_item<float>(i, res, ref, options);
      errors += 1;
    }
  }
  if (errors > 0) {
    std::cout << std::to_string(errors) + " errors!" << std::endl;
    failed_tests++;
  } else {
    std::cout << "Test is successful!" << std::endl;
  }
}

void test_barrier(ACCL::ACCL &accl) {
  std::cout << "Start barrier test " << std::endl;
  accl.barrier();
  std::cout << "Test is successful!" << std::endl;
}

bool check_arp(vnx::Networklayer &network_layer, std::vector<rank_t> &ranks,
               options_t &options) {
  std::map<unsigned, bool> ranks_checked;
  for (unsigned i = 0; i < static_cast<unsigned>(size); ++i) {
    ranks_checked[i] = false;
  }

  bool sanity_check = true;
  const std::map<int, std::pair<std::string, std::string>> arp =
      network_layer.read_arp_table(size);

  std::ostringstream ss_arp;
  ss_arp << "ARP table:";

  for (const std::pair<const int, std::pair<std::string, std::string>> &elem :
       arp) {
    const unsigned index = elem.first;
    const std::pair<std::string, std::string> &entry = elem.second;
    const std::string &mac = entry.first;
    const std::string &ip = entry.second;
    ss_arp << "\n(" << index << ") " << mac << ": " << ip;

    for (unsigned i = 0; i < static_cast<unsigned>(size); ++i) {
      if (ranks[i].ip == ip) {
        if (ranks_checked[i]) {
          std::cout << "Double entry for " << ip << " in arp table!"
                    << std::endl;
          sanity_check = false;
        } else {
          ranks_checked[i] = true;
        }
      }
    }
  }

  test_debug(ss_arp.str(), options);

  if (!sanity_check) {
    return false;
  }

  unsigned hosts = 0;
  for (unsigned i = 0; i < static_cast<unsigned>(size); ++i) {
    if (ranks_checked[i]) {
      hosts += 1;
    }
  }
  if (hosts < static_cast<unsigned>(size) - 1) {
    std::cout << "Found only " << hosts << " hosts out of " << size - 1 << "!"
              << std::endl;
    return false;
  }

  return true;
}

void configure_vnx(vnx::CMAC &cmac, vnx::Networklayer &network_layer,
                   std::vector<rank_t> &ranks, options_t &options) {
  if (ranks.size() > vnx::max_sockets_size) {
    throw std::runtime_error("Too many ranks. VNX supports up to " +
                             std::to_string(vnx::max_sockets_size) +
                             " sockets.");
  }

  std::cout << "Testing UDP link status: ";

  const auto link_status = cmac.link_status();

  if (link_status.at("rx_status")) {
    std::cout << "Link successful!" << std::endl;
  } else {
    std::cout << "No link found." << std::endl;
  }

  std::ostringstream ss;

  ss << "Link interface 1 : {";
  for (const auto &elem : link_status) {
    ss << elem.first << ": " << elem.second << ", ";
  }
  ss << "}" << std::endl;
  test_debug(ss.str(), options);

  if (!link_status.at("rx_status")) {
    // Give time for other ranks to setup link.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    exit(1);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  std::cout << "Populating socket table..." << std::endl;

  network_layer.update_ip_address(ranks[rank].ip);
  for (size_t i = 0; i < ranks.size(); ++i) {
    if (i == static_cast<size_t>(rank)) {
      continue;
    }

    network_layer.configure_socket(i, ranks[i].ip, ranks[i].port,
                                   ranks[rank].port, true);
  }

  network_layer.populate_socket_table();

  std::cout << "Starting ARP discovery..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(4));
  MPI_Barrier(MPI_COMM_WORLD);
  network_layer.arp_discovery();
  std::cout << "Finishing ARP discovery..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  MPI_Barrier(MPI_COMM_WORLD);
  network_layer.arp_discovery();
  std::cout << "ARP discovery finished!" << std::endl;

  if (!check_arp(network_layer, ranks, options)) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    exit(1);
  }

  MPI_Barrier(MPI_COMM_WORLD);
}

void configure_tcp(BaseBuffer &tx_buf_network, BaseBuffer &rx_buf_network,
                   xrt::kernel &network_krnl, std::vector<rank_t> &ranks,
                   options_t &options) {
  std::cout << "Configure TCP Network Kernel" << std::endl;
  tx_buf_network.sync_to_device();
  rx_buf_network.sync_to_device();

  uint local_fpga_ip = ip_encode(ranks[rank].ip);
  std::cout << "rank: " << rank << " FPGA IP: " << std::hex << local_fpga_ip
            << std::endl;

  network_krnl(local_fpga_ip, static_cast<uint32_t>(rank), local_fpga_ip,
               *(tx_buf_network.bo()), *(rx_buf_network.bo()));

  uint32_t ip_reg = network_krnl.read_register(0x010);
  uint32_t board_reg = network_krnl.read_register(0x018);
  std::cout << std::hex << "ip_reg: " << ip_reg
            << " board_reg IP: " << board_reg << std::endl;
}

void configure_roce(roce::CMAC &cmac, roce::Hivenet &hivenet,
                    std::vector<rank_t> &ranks, options_t &options) {
  uint32_t subnet_e = ip_encode(ranks[rank].ip) & 0xFFFFFF00;
  std::string subnet = ip_decode(subnet_e);
  uint32_t local_id = hivenet.get_local_id();
  std::string internal_ip = ip_decode(subnet_e + local_id);

  if (ranks[rank].ip != internal_ip) {
    throw std::runtime_error(
      "IP address set (" + ranks[rank].ip + ") mismatches with internal " +
      "hivenet IP (" + internal_ip + "). The internal ip is determined by " +
      "adding the rank (" + std::to_string(rank) + ") to the subnet (" +
      subnet + ").");
  }

  hivenet.set_ip_subnet(subnet);
  hivenet.set_mac_subnet(0x347844332211);
  cmac.set_rs_fec(true);

  MPI_Barrier(MPI_COMM_WORLD);
  std::cout << "Testing RoCE link status: ";

  const auto link_status = cmac.link_status();

  if (link_status.at("rx_status")) {
    std::cout << "Link successful!" << std::endl;
  } else {
    std::cout << "No link found." << std::endl;
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if (!link_status.at("rx_status")) {
    throw std::runtime_error("No link on ethernet.");
  }
  MPI_Barrier(MPI_COMM_WORLD);
}

std::vector<std::string> get_ips(std::string config_file, bool local) {
  std::vector<std::string> ips;
  if (config_file == "") {
    for (int i = 0; i < size; ++i) {
      if (local) {
        ips.emplace_back("127.0.0.1");
      } else {
        ips.emplace_back("10.10.10." + std::to_string(i));
      }
    }
  } else {
    Json::Value config;
    std::ifstream config_file_stream(config_file);
    config_file_stream >> config;
    Json::Value ip_config = config["ips"];
    for (int i = 0; i < size; ++i) {
      std::string ip = ip_config[i].asString();
      if (ip == "") {
        throw std::runtime_error("No ip for rank " + std::to_string(i) +
                                 " in config file.");
      }
      ips.push_back(ip);
    }
  }

  return ips;
}

int start_test(options_t options) {
  std::vector<rank_t> ranks = {};
  failed_tests = 0;
  skipped_tests = 0;
  options.ips =
      get_ips(options.config_file, !options.hardware || options.axis3);
  for (int i = 0; i < size; ++i) {
    rank_t new_rank = {options.ips[i], options.start_port + i, i,
                       options.rxbuf_size};
    ranks.emplace_back(new_rank);
  }

  std::unique_ptr<ACCL::ACCL> accl;
  std::unique_ptr<ACCL::BaseBuffer> tx_buf_network;
  std::unique_ptr<ACCL::BaseBuffer> rx_buf_network;

  xrt::device device;

  networkProtocol protocol;

  if (options.hardware || options.test_xrt_simulator) {
    device = xrt::device(options.device_index);
  }

  if (options.hardware) {
    std::string cclo_id;
    if (options.axis3) {
      cclo_id = std::to_string(rank);
    } else {
      cclo_id = "0";
    }
    auto xclbin_uuid = device.load_xclbin(options.xclbin);
    auto cclo_ip = xrt::ip(device, xclbin_uuid,
                           "ccl_offload:{ccl_offload_" + cclo_id + "}");
    auto hostctrl_ip = xrt::kernel(device, xclbin_uuid,
                                   "hostctrl:{hostctrl_" + cclo_id + "_0}",
                                   xrt::kernel::cu_access_mode::exclusive);

    int devicemem;
    std::vector<int> rxbufmem;
    int networkmem;
    if (options.axis3) {
      devicemem = rank * 6;
      rxbufmem = {rank * 6 + 1};
      networkmem = rank * 6 + 2;
    } else {
      devicemem = 0;
      rxbufmem = {1};
      networkmem = 6;
    }

    protocol = options.tcp ? networkProtocol::TCP : networkProtocol::UDP;

    if (options.udp) {
      auto cmac = vnx::CMAC(xrt::ip(device, xclbin_uuid, "cmac_0:{cmac_0}"));
      auto network_layer = vnx::Networklayer(
          xrt::ip(device, xclbin_uuid, "networklayer:{networklayer_0}"));

      configure_vnx(cmac, network_layer, ranks, options);
    } else if (options.tcp) {
      tx_buf_network = std::unique_ptr<BaseBuffer>(new FPGABuffer<int8_t>(
          64 * 1024 * 1024, dataType::int8, device, networkmem));
      rx_buf_network = std::unique_ptr<BaseBuffer>(new FPGABuffer<int8_t>(
          64 * 1024 * 1024, dataType::int8, device, networkmem));
      auto network_krnl =
          xrt::kernel(device, xclbin_uuid, "network_krnl:{network_krnl_0}",
                      xrt::kernel::cu_access_mode::exclusive);
      configure_tcp(*tx_buf_network, *rx_buf_network, network_krnl, ranks,
                    options);
    } else if (options.roce) {
      auto cmac = roce::CMAC(xrt::ip(device, xclbin_uuid, "cmac_0:{cmac_0}"));
      auto hivenet = roce::Hivenet(
          xrt::ip(device, xclbin_uuid, "HiveNet_kernel_0:{networklayer_0}"),
          rank);

      configure_roce(cmac, hivenet, ranks, options);
    }

    accl = std::make_unique<ACCL::ACCL>(ranks, rank, device, cclo_ip,
                                        hostctrl_ip, devicemem, rxbufmem,
                                        protocol, 16, options.rxbuf_size);
  } else {
    protocol = options.udp || options.roce ? networkProtocol::UDP
                                           : networkProtocol::TCP;
    accl = std::make_unique<ACCL::ACCL>(ranks, rank, options.start_port, device,
                                        protocol, 16, options.rxbuf_size);
  }
  if (protocol == networkProtocol::TCP) {
    MPI_Barrier(MPI_COMM_WORLD);
    accl->open_port();
    MPI_Barrier(MPI_COMM_WORLD);
    accl->open_con();
  }

  accl->set_timeout(1e6);

  // barrier here to make sure all the devices are configured before testing
  MPI_Barrier(MPI_COMM_WORLD);
  accl->nop();
  MPI_Barrier(MPI_COMM_WORLD);
  test_barrier(*accl);
  MPI_Barrier(MPI_COMM_WORLD);
  test_copy(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_copy_stream(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_copy_p2p(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_combine_sum(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_combine_max(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_sendrcv(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_sendrcv_stream(*accl, options);
  if (options.test_xrt_simulator) {
    MPI_Barrier(MPI_COMM_WORLD);
    test_sendrcv_bo(*accl, device, options);
  } else {
    std::cout << "Skipping xrt::bo test. We are not running on hardware and "
                 "XCL emulation is disabled. Make sure XILINX_VITIS and "
                 "XCL_EMULATION_MODE are set."
              << std::endl;
    ++skipped_tests;
  }
  MPI_Barrier(MPI_COMM_WORLD);
  test_sendrcv_compressed(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_stream_put(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_allgather(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_allgather_compressed(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_allreduce(*accl, options, reduceFunction::SUM);
  MPI_Barrier(MPI_COMM_WORLD);
  test_allreduce_compressed(*accl, options, reduceFunction::SUM);
  MPI_Barrier(MPI_COMM_WORLD);
  test_reduce_scatter(*accl, options, reduceFunction::SUM);
  MPI_Barrier(MPI_COMM_WORLD);
  // test_reduce_scatter_compressed(*accl, options, reduceFunction::SUM);
  // MPI_Barrier(MPI_COMM_WORLD);
  test_multicomm(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);
  test_allgather_comms(*accl, options);
  MPI_Barrier(MPI_COMM_WORLD);

  for (int root = 0; root < size; ++root) {
    test_bcast(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_bcast_compressed(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_scatter(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_scatter_compressed(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_gather(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_gather_compressed(*accl, options, root);
    MPI_Barrier(MPI_COMM_WORLD);
    test_reduce(*accl, options, root, reduceFunction::SUM);
    MPI_Barrier(MPI_COMM_WORLD);
    test_reduce_compressed(*accl, options, root, reduceFunction::SUM);
    MPI_Barrier(MPI_COMM_WORLD);
    test_reduce_stream2mem(*accl, options, root, reduceFunction::SUM);
    MPI_Barrier(MPI_COMM_WORLD);
    test_reduce_mem2stream(*accl, options, root, reduceFunction::SUM);
    MPI_Barrier(MPI_COMM_WORLD);
    test_reduce_stream2stream(*accl, options, root, reduceFunction::SUM);
    MPI_Barrier(MPI_COMM_WORLD);
  }

  std::cout << failed_tests << " tests failed on rank " << rank;
  if (skipped_tests > 0) {
    std::cout << " (skipped " << skipped_tests << " tests)";
  }
  std::cout << "." << std::endl;
  MPI_Barrier(MPI_COMM_WORLD);
  return failed_tests;
}

bool xrt_simulator_ready(const options_t &opts) {
  if (opts.hardware) {
    return true;
  }

  const char *vitis = std::getenv("XILINX_VITIS");

  if (vitis == nullptr) {
    return false;
  }

  const char *emu = std::getenv("XCL_EMULATION_MODE");
  if (emu == nullptr) {
    return false;
  }

  return std::string(emu) == "sw_emu" || std::string(emu) == "hw_emu";
}

options_t parse_options(int argc, char *argv[]) {
  TCLAP::CmdLine cmd("Test ACCL C++ driver");
  TCLAP::ValueArg<unsigned int> nruns_arg("n", "nruns",
                                          "How many times to run each test",
                                          false, 1, "positive integer");
  cmd.add(nruns_arg);
  TCLAP::ValueArg<uint16_t> start_port_arg(
      "p", "start-port", "Start of range of ports usable for sim", false, 5500,
      "positive integer");
  cmd.add(start_port_arg);
  TCLAP::ValueArg<unsigned int> count_arg(
      "s", "count", "How many items per test", false, 16, "positive integer");
  cmd.add(count_arg);
  TCLAP::ValueArg<unsigned int> bufsize_arg("b", "rxbuf-size",
                                            "How many KB per RX buffer", false,
                                            1, "positive integer");
  cmd.add(bufsize_arg);
  TCLAP::SwitchArg debug_arg("d", "debug", "Enable debug mode", cmd, false);
  TCLAP::SwitchArg hardware_arg("f", "hardware", "enable hardware mode", cmd,
                                false);
  TCLAP::SwitchArg axis3_arg("a", "axis3", "Use axis3 hardware setup", cmd,
                             false);
  TCLAP::SwitchArg udp_arg("u", "udp", "Use UDP hardware setup", cmd, false);
  TCLAP::SwitchArg tcp_arg("t", "tcp", "Use TCP hardware setup", cmd, false);
  TCLAP::SwitchArg roce_arg("r", "roce", "Use RoCE hardware setup", cmd, false);
  TCLAP::ValueArg<std::string> xclbin_arg(
      "x", "xclbin", "xclbin of accl driver if hardware mode is used", false,
      "accl.xclbin", "file");
  cmd.add(xclbin_arg);
  TCLAP::ValueArg<uint16_t> device_index_arg(
      "i", "device-index", "device index of FPGA if hardware mode is used",
      false, 0, "positive integer");
  cmd.add(device_index_arg);
  TCLAP::ValueArg<std::string> config_arg("c", "config",
                                          "Config file containing IP mapping",
                                          false, "", "JSON file");
  cmd.add(config_arg);

  try {
    cmd.parse(argc, argv);
    if (hardware_arg.getValue()) {
      if (axis3_arg.getValue() + udp_arg.getValue() + tcp_arg.getValue() +
              roce_arg.getValue() !=
          1) {
        throw std::runtime_error("When using hardware, specify one of axis3, "
                                 "tcp, udp, or roce mode, but not both.");
      }
    }
  } catch (std::exception &e) {
    if (rank == 0) {
      std::cout << "Error: " << e.what() << std::endl;
    }

    MPI_Finalize();
    exit(1);
  }

  options_t opts;
  opts.start_port = start_port_arg.getValue();
  opts.count = count_arg.getValue();
  opts.rxbuf_size = bufsize_arg.getValue() * 1024; // convert to bytes
  opts.nruns = nruns_arg.getValue();
  opts.debug = debug_arg.getValue();
  opts.hardware = hardware_arg.getValue();
  opts.axis3 = axis3_arg.getValue();
  opts.udp = udp_arg.getValue();
  opts.tcp = tcp_arg.getValue();
  opts.roce = roce_arg.getValue();
  opts.device_index = device_index_arg.getValue();
  opts.xclbin = xclbin_arg.getValue();
  opts.test_xrt_simulator = xrt_simulator_ready(opts);
  opts.config_file = config_arg.getValue();
  return opts;
}

int main(int argc, char *argv[]) {
  MPI_Init(&argc, &argv);

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  options_t options = parse_options(argc, argv);

  std::ostringstream stream;
  stream << prepend_process() << "rank " << rank << " size " << size
         << std::endl;
  std::cout << stream.str();

  int errors = start_test(options);

  MPI_Finalize();
  return errors;
}
