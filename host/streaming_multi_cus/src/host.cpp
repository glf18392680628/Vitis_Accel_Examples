/**
* Copyright (C) 2020 Xilinx, Inc
*
* Licensed under the Apache License, Version 2.0 (the "License"). You may
* not use this file except in compliance with the License. A copy of the
* License is located at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
* License for the specific language governing permissions and limitations
* under the License.
*/

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// This extension file is required for stream APIs
#include "CL/cl_ext_xilinx.h"
// This file is required for OpenCL C++ wrapper APIs
#include "xcl2.hpp"

// Declaration of custom stream APIs that binds to Xilinx Streaming APIs.
decltype(&clCreateStream) xcl::Stream::createStream = nullptr;
decltype(&clReleaseStream) xcl::Stream::releaseStream = nullptr;
decltype(&clReadStream) xcl::Stream::readStream = nullptr;
decltype(&clWriteStream) xcl::Stream::writeStream = nullptr;
decltype(&clPollStreams) xcl::Stream::pollStreams = nullptr;

auto constexpr c_test_size = 256 * 1024 * 1024; // 256 MB data
auto constexpr NCU = 4;

////////////////////RESET FUNCTION//////////////////////////////////
int reset(int *a, int *b, int *sw_results, int *hw_results, unsigned int size) {
  // Fill the input vectors with data
  std::generate(a, a + size, std::rand);
  std::generate(b, b + size, std::rand);
  for (size_t i = 0; i < size; i++) {
    hw_results[i] = 0;
    sw_results[i] = a[i] + b[i];
  }
  return 0;
}
///////////////////VERIFY FUNCTION///////////////////////////////////
bool verify(int *sw_results, int *hw_results, int size) {
  bool match = true;
  for (int i = 0; i < size; i++) {
    if (sw_results[i] != hw_results[i]) {
      match = false;
      break;
    }
  }
  std::cout << "TEST " << (match ? "PASSED" : "FAILED") << std::endl;
  return match;
}
////////MAIN FUNCTION//////////
int main(int argc, char **argv) {
  unsigned int size = c_test_size;

  if (xcl::is_hw_emulation()) {
    size = 4096; // 4KB for HW emulation
  } else if (xcl::is_emulation()) {
    size = 2 * 1024 * 1024; // 2MB for sw emulation
  }

  // I/O Data Vectors
  std::vector<int, aligned_allocator<int>> h_a(size);
  std::vector<int, aligned_allocator<int>> h_b(size);
  std::vector<int, aligned_allocator<int>> hw_results(size);
  std::vector<int> sw_results(size);

  reset(h_a.data(), h_b.data(), sw_results.data(), hw_results.data(), size);

  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <XCLBIN File>" << std::endl;
    return EXIT_FAILURE;
  }

  auto binaryFile = argv[1];
  std::cout << "\n Vector Addition of elements " << size << std::endl;

  // Bytes per CU Stream
  int vector_size_bytes = sizeof(int) * size / NCU;

  // OpenCL Host Code Begins
  cl_int err;
  std::string cu_id;
  std::string krnl_name = "krnl_stream_vadd";
  std::vector<cl::Kernel> krnls(NCU);
  int no_of_elem = size / NCU;
  cl::CommandQueue q;
  cl::Context context;
  cl::Device device;
  // get_xil_devices() is a utility API which will find the xilinx
  // platforms and will return list of devices connected to Xilinx platform
  auto devices = xcl::get_xil_devices();

  // read_binary_file() is a utility API which will load the binaryFile
  // and will return the pointer to file buffer.
  auto fileBuf = xcl::read_binary_file(binaryFile);
  cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
  bool valid_device = false;
  for (unsigned int i = 0; i < devices.size(); i++) {
    device = devices[i];
    // Creating Context and Command Queue for selected Device
    OCL_CHECK(err, context = cl::Context(device, NULL, NULL, NULL, &err));
    OCL_CHECK(err,
              q = cl::CommandQueue(context, device,
                                   CL_QUEUE_PROFILING_ENABLE |
                                       CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE,
                                   &err));

    std::cout << "Trying to program device[" << i
              << "]: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
    cl::Program program(context, {device}, bins, NULL, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Failed to program device[" << i << "] with xclbin file!\n";
    } else {
      std::cout << "Device[" << i << "]: program successful!\n";
      // Creating Kernel
      for (int i = 0; i < NCU; i++) {
        cu_id = std::to_string(i + 1);
        auto krnl_name_full = krnl_name + ":{" + "krnl_stream_vadd_" + cu_id + "}";
        printf("Creating a kernel [%s] for CU(%d)\n", krnl_name_full.c_str(),
               i);
        OCL_CHECK(err,
                  krnls[i] = cl::Kernel(program, krnl_name_full.c_str(), &err));
      }

      valid_device = true;
      break; // we break because we found a valid device
    }
  }
  if (!valid_device) {
    std::cout << "Failed to program any device found, exit!\n";
    exit(EXIT_FAILURE);
  }

  auto platform_id = device.getInfo<CL_DEVICE_PLATFORM>(&err);

  // Initialization of streaming class is needed before using it.
  xcl::Stream::init(platform_id);

  // Streams
  std::vector<cl_stream> h2c_stream_a(NCU);
  std::vector<cl_stream> h2c_stream_b(NCU);
  std::vector<cl_stream> c2h_stream(NCU);

  cl_int ret;

  for (int i = 0; i < NCU; i++) {
    // Device Connection specification of the stream through extension pointer
    cl_mem_ext_ptr_t ext;
    ext.param = krnls[i].get();
    ext.obj = NULL;
    // The .flag should be used to denote the kernel argument
    // Create write stream for argument 0 and 1 of kernel
    std::cout << "\n Creating Stream for CU: " << i;
    ext.flags = 0;
    OCL_CHECK(ret,
              h2c_stream_a[i] = xcl::Stream::createStream(
                  device.get(), XCL_STREAM_READ_ONLY, CL_STREAM, &ext, &ret));
    ext.flags = 1;
    OCL_CHECK(ret,
              h2c_stream_b[i] = xcl::Stream::createStream(
                  device.get(), XCL_STREAM_READ_ONLY, CL_STREAM, &ext, &ret));

    // Create read stream for argument 2 of kernel
    ext.flags = 2;
    OCL_CHECK(ret,
              c2h_stream[i] = xcl::Stream::createStream(
                  device.get(), XCL_STREAM_WRITE_ONLY, CL_STREAM, &ext, &ret));
  }

  // Launch the Kernel
  for (int i = 0; i < NCU; i++) {
    std::cout << "\n Enqueuing Kernel " << i;
    OCL_CHECK(err, err = q.enqueueTask(krnls[i]));
  }

  // Initiating the READ and WRITE transfer
  for (int i = 0; i < NCU; i++) {
    cl_stream_xfer_req rd_req{0};
    cl_stream_xfer_req wr_req{0};

    rd_req.flags = CL_STREAM_EOT | CL_STREAM_NONBLOCKING;
    wr_req.flags = CL_STREAM_EOT | CL_STREAM_NONBLOCKING;

    auto write_tag_a = "write_a_" + std::to_string(i);
    wr_req.priv_data = (void *)write_tag_a.c_str();

    std::cout << "\n Writing Stream h2c_stream_a[" << i << "]";
    OCL_CHECK(ret, xcl::Stream::writeStream(h2c_stream_a[i],
                                            (h_a.data() + i * no_of_elem),
                                            vector_size_bytes, &wr_req, &ret));

    auto write_tag_b = "write_b_" + std::to_string(i);
    wr_req.priv_data = (void *)write_tag_b.c_str();

    std::cout << "\n Writing Stream h2c_stream_b[" << i << "]";
    OCL_CHECK(ret, xcl::Stream::writeStream(h2c_stream_b[i],
                                            (h_b.data() + i * no_of_elem),
                                            vector_size_bytes, &wr_req, &ret));

    auto read_tag = "read_" + std::to_string(i);
    rd_req.priv_data = (void *)read_tag.c_str();

    std::cout << "\n Reading Stream c2h_stream[" << i << "]";
    OCL_CHECK(ret, xcl::Stream::readStream(c2h_stream[i],
                                           (hw_results.data() + i * no_of_elem),
                                           vector_size_bytes, &rd_req, &ret));
  }

  // Sync for the async streaming
  int num_compl = 3 * NCU;

  // Checking the request completions
  cl_streams_poll_req_completions *poll_req;
  poll_req = (cl_streams_poll_req_completions *)malloc(
      sizeof(cl_streams_poll_req_completions) * num_compl);
  memset(poll_req, 0, sizeof(cl_streams_poll_req_completions) * num_compl);
  printf("\n clPollStreams for (%d) events (CU: %d, axis_in: 2, axis_out: 1)\n",
         num_compl, NCU);
  OCL_CHECK(ret, xcl::Stream::pollStreams(device.get(), poll_req, num_compl,
                                          num_compl, &num_compl, 50000, &ret));

  // Compare the device results with software results
  bool match = verify(sw_results.data(), hw_results.data(), size);

  // Releasing all OpenCL objects
  q.finish();
  for (int i = 0; i < NCU; i++) {
    xcl::Stream::releaseStream(c2h_stream[i]);
    xcl::Stream::releaseStream(h2c_stream_a[i]);
    xcl::Stream::releaseStream(h2c_stream_b[i]);
  }
  return (match ? EXIT_SUCCESS : EXIT_FAILURE);
}
