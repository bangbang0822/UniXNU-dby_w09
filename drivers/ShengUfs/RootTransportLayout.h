// SPDX-License-Identifier: BSD-2-Clause-Patent

#ifndef UFS_ROOT_TRANSPORT_LAYOUT_H
#define UFS_ROOT_TRANSPORT_LAYOUT_H
// Included inside a namespace by its consumers; the guard only keeps a second include from repeating it.
enum : uint32_t {
  kUcdOffset = 4096,
  kRspOffset = 512,
  kPrdtOffset = 1024,
  kDataOffset = 8192,
  kDataMax = 131072,
  kDmaBytes = kDataOffset + kDataMax,
};

#endif   /* UFS_ROOT_TRANSPORT_LAYOUT_H */
