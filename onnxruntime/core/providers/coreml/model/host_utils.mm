// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/coreml/model/host_utils.h"

#import <Foundation/Foundation.h>

namespace onnxruntime {
namespace coreml {
namespace util {

bool HasRequiredBaseOS() {
  // This may look strange, but it is required "@available(macOS ....)" to safe-guard some code
  // otherwise the compiler will spit -Wunsupported-availability-guard
  if (HAS_COREML3_OR_LATER)
    return true;
  else
    return false;
}

int32_t CoreMLVersion() {
  if (HAS_COREML7_OR_LATER)
    return 7;
  if (HAS_COREML6_OR_LATER)
    return 6;
  if (HAS_COREML5_OR_LATER)
    return 5;
  if (HAS_COREML4_OR_LATER)
    return 4;
  if (HAS_COREML3_OR_LATER)
    return 3;

  return -1;
}

std::string GetTemporaryDirectoryPath() {
  // Get temporary directory.
  NSURL* temporary_directory_url = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
  return std::string([[temporary_directory_url path] UTF8String]);
}

std::string GetTemporaryFilePath() {
  // Get temporary directory.
  NSURL* temporary_directory_url = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
  // Generate a Unique file name to use.
  NSString* temporary_filename = [[NSProcessInfo processInfo] globallyUniqueString];
  // Create URL to that file.
  NSURL* temporary_file_url = [temporary_directory_url URLByAppendingPathComponent:temporary_filename];

  return std::string([[temporary_file_url path] UTF8String]);
}

}  // namespace util
}  // namespace coreml
}  // namespace onnxruntime
