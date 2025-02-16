//
// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <cstdint>
#include <cstdio>
#include <memory>

#include <sys/mman.h>
#include <sys/stat.h>

#include <base/files/file_path.h>
#include <libsnapshot/cow_writer.h>
#include <gflags/gflags.h>

#include "update_engine/common/utils.h"
#include "update_engine/payload_consumer/file_descriptor.h"
#include "update_engine/payload_consumer/payload_metadata.h"
#include "update_engine/payload_generator/cow_size_estimator.h"
#include "update_engine/update_metadata.pb.h"

DEFINE_string(partitions,
              "",
              "Comma separated list of partitions to extract, leave empty for "
              "extracting all partitions");
DEFINE_int32(cow_version,
             0,
             "VABC Cow version to use. Default is to use what's specified in "
             "the OTA manifest");
DEFINE_string(vabc_compression_param,
              "",
              "Compression parameter for VABC. Default is use what's specified "
              "in OTA package");

namespace chromeos_update_engine {

bool ProcessPartition(
    const chromeos_update_engine::DeltaArchiveManifest& manifest,
    const chromeos_update_engine::PartitionUpdate& partition,
    const char* image_dir) {
  base::FilePath img_dir{image_dir};
  auto target_img = img_dir.Append(partition.partition_name() + ".img");
  auto output_cow = img_dir.Append(partition.partition_name() + ".cow");
  FileDescriptorPtr target_img_fd = std::make_shared<EintrSafeFileDescriptor>();
  if (!target_img_fd->Open(target_img.value().c_str(), O_RDONLY)) {
    PLOG(ERROR) << "Failed to open " << target_img.value();
    return false;
  }
  android::base::unique_fd output_fd{
      open(output_cow.value().c_str(), O_RDWR | O_CREAT, 0744)};
  if (output_fd < 0) {
    PLOG(ERROR) << "Failed to open " << output_cow.value();
    return false;
  }

  const auto& dap = manifest.dynamic_partition_metadata();

  android::snapshot::CowOptions options{
      .block_size = static_cast<uint32_t>(manifest.block_size()),
      .compression = dap.vabc_compression_param(),
      .batch_write = true,
      .op_count_max = static_cast<uint32_t>(
          partition.new_partition_info().size() / manifest.block_size())};
  if (!FLAGS_vabc_compression_param.empty()) {
    options.compression = FLAGS_vabc_compression_param;
  }
  auto cow_version = dap.cow_version();
  if (FLAGS_cow_version > 0) {
    cow_version = FLAGS_cow_version;
    LOG(INFO) << "Using user specified COW version " << cow_version;
  }
  auto cow_writer = android::snapshot::CreateCowWriter(
      cow_version, options, std::move(output_fd));
  TEST_AND_RETURN_FALSE(cow_writer);
  TEST_AND_RETURN_FALSE(CowDryRun(nullptr,
                                  target_img_fd,
                                  partition.operations(),
                                  partition.merge_operations(),
                                  manifest.block_size(),
                                  cow_writer.get(),
                                  partition.new_partition_info().size(),
                                  partition.old_partition_info().size(),
                                  false));
  TEST_AND_RETURN_FALSE(cow_writer->Finalize());
  return true;
}

}  // namespace chromeos_update_engine

using chromeos_update_engine::MetadataParseResult;
using chromeos_update_engine::PayloadMetadata;

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
      "A tool to extract device images from Android OTA packages");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto tokens = android::base::Tokenize(FLAGS_partitions, ",");
  const std::set<std::string> partitions(
      std::make_move_iterator(tokens.begin()),
      std::make_move_iterator(tokens.end()));

  if (argc != 3) {
    printf("Usage: %s <payload.bin> <extracted target_file>\n", argv[0]);
    return -1;
  }
  const char* payload_path = argv[1];
  const char* images_dir = argv[2];
  int payload_fd = open(payload_path, O_RDONLY);
  if (payload_fd < 0) {
    PLOG(ERROR) << "Failed to open payload file:";
    return 1;
  }
  chromeos_update_engine::ScopedFdCloser closer{&payload_fd};
  auto payload_size = chromeos_update_engine::utils::FileSize(payload_fd);
  if (payload_size <= 0) {
    PLOG(ERROR)
        << "Couldn't determine size of payload file, or payload file is empty";
    return 2;
  }

  PayloadMetadata payload_metadata;
  auto payload = static_cast<unsigned char*>(
      mmap(nullptr, payload_size, PROT_READ, MAP_PRIVATE, payload_fd, 0));

  // C++ dark magic to ensure that |payload| is properly deallocated once the
  // program exits.
  auto munmap_deleter = [payload_size](auto payload) {
    munmap(payload, payload_size);
  };
  std::unique_ptr<unsigned char, decltype(munmap_deleter)> munmapper{
      payload, munmap_deleter};

  if (payload == nullptr) {
    PLOG(ERROR) << "Failed to mmap() payload file";
    return 3;
  }
  if (payload_metadata.ParsePayloadHeader(payload, payload_size, nullptr) !=
      chromeos_update_engine::MetadataParseResult::kSuccess) {
    LOG(ERROR) << "Payload header parse failed!";
    return 4;
  }
  chromeos_update_engine::DeltaArchiveManifest manifest;
  if (!payload_metadata.GetManifest(payload, payload_size, &manifest)) {
    LOG(ERROR) << "Failed to parse manifest!";
    return 5;
  }

  size_t estimated_total_cow_size = 0;
  size_t actual_total_cow_size = 0;

  for (const auto& partition : manifest.partitions()) {
    if (partition.estimate_cow_size() == 0) {
      continue;
    }
    if (!partitions.empty() &&
        partitions.count(partition.partition_name()) == 0) {
      continue;
    }
    LOG(INFO) << partition.partition_name();
    if (!ProcessPartition(manifest, partition, images_dir)) {
      return 6;
    }
    base::FilePath img_dir{images_dir};
    const auto output_cow =
        img_dir.Append(partition.partition_name() + ".cow").value();
    const auto actual_cow_size =
        chromeos_update_engine::utils::FileSize(output_cow);
    LOG(INFO) << partition.partition_name()
              << ": estimated COW size is: " << partition.estimate_cow_size()
              << ", actual COW size is: " << actual_cow_size
              << ", estimated COW size is "
              << (actual_cow_size - partition.estimate_cow_size()) * 100.0f /
                     actual_cow_size
              << "% smaller";
    estimated_total_cow_size += partition.estimate_cow_size();
    actual_total_cow_size += actual_cow_size;
  }

  LOG(INFO) << "Total estimated COW size is: " << estimated_total_cow_size
            << ", Total actual COW size is: " << actual_total_cow_size
            << ", estimated COW size is "
            << (actual_total_cow_size - estimated_total_cow_size) * 100.0f /
                   actual_total_cow_size
            << "% smaller";
  return 0;
}
