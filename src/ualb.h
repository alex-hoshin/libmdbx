/*
 * Copyright 2015-2017 Leonid Yuriev <leo@yuriev.ru>
 * and other libmdbx authors: please see AUTHORS file.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "./defs.h"

/*----------------------------------------------------------------------------*/
/* Byteorder */

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) ||           \
    !defined(__ORDER_BIG_ENDIAN__)

#if defined(HAVE_ENDIAN_H)
#include <endian.h>
#elif defined(HAVE_SYS_PARAM_H)
#include <sys/param.h> /* for endianness */
#elif defined(HAVE_NETINET_IN_H) && defined(HAVE_RESOLV_H)
#include <netinet/in.h>
#include <resolv.h> /* defines BYTE_ORDER on HPUX and Solaris */
#endif

#if defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)
#define __ORDER_LITTLE_ENDIAN__ __LITTLE_ENDIAN
#define __ORDER_BIG_ENDIAN__ __BIG_ENDIAN
#define __BYTE_ORDER__ __BYTE_ORDER
#else
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#if defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN) ||                   \
    defined(__ARMEL__) || defined(__THUMBEL__) || defined(__AARCH64EL__) ||    \
    defined(__MIPSEL__) || defined(_MIPSEL) || defined(__MIPSEL) ||            \
    defined(__i386) || defined(__x86_64__) || defined(_M_IX86) ||              \
    defined(_M_X64) || defined(i386) || defined(_X86_) || defined(__i386__) || \
    defined(_X86_64_) || defined(_M_ARM) || defined(_M_ARM64) ||               \
    defined(__e2k__)
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN) || defined(__ARMEB__) || \
    defined(__THUMBEB__) || defined(__AARCH64EB__) || defined(__MIPSEB__) ||   \
    defined(_MIPSEB) || defined(__MIPSEB) || defined(_M_IA64)
#define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
#else
#error __BYTE_ORDER__ should be defined.
#endif
#endif
#endif /* __BYTE_ORDER__ || __ORDER_LITTLE_ENDIAN__ || __ORDER_BIG_ENDIAN__ */

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ &&                               \
    __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error Unsupported byte order.
#endif

/*----------------------------------------------------------------------------*/

static inline uint16_t le16_to_cpu(uint16_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap16(v);
#endif
  return v;
}

static inline uint32_t le32_to_cpu(uint32_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

static inline uint64_t le64_to_cpu(uint64_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap64(v);
#endif
  return v;
}

static inline uint16_t cpu_to_le16(uint16_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap16(v);
#endif
  return v;
}

static inline uint32_t cpu_to_le32(uint32_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

static inline uint64_t cpu_to_le64(uint64_t v) {
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  v = __builtin_bswap64(v);
#endif
  return v;
}

/*----------------------------------------------------------------------------*/

static inline bool is_aligned_uint16(const void *ptr) {
  return (1 & (uintptr_t)ptr) == 0;
}

static inline bool is_aligned_uint32(const void *ptr) {
  return (3 & (uintptr_t)ptr) == 0;
}

static inline bool is_aligned_uint64(const void *ptr) {
  return (7 & (uintptr_t)ptr) == 0;
}

/*----------------------------------------------------------------------------*/

static inline uint16_t get_le16_aligned(const uint16_t *ptr) {
  assert(is_aligned_uint16(ptr));
  return le16_to_cpu(*ptr);
}

static inline void set_le16_aligned(uint16_t *ptr, uint16_t v) {
  assert(is_aligned_uint16(ptr));
  *ptr = cpu_to_le16(v);
}

static inline uint32_t get_le32_aligned(const uint32_t *ptr) {
  assert(is_aligned_uint32(ptr));
  return le32_to_cpu(*ptr);
}

void set_le32_aligned(uint32_t *ptr, uint32_t v) {
  assert(is_aligned_uint32(ptr));
  *ptr = cpu_to_le32(v);
}

static inline uint64_t get_le64_aligned(const uint64_t *ptr) {
  assert(is_aligned_uint64(ptr));
  return le64_to_cpu(*ptr);
}

static inline void set_le64_aligned(uint64_t *ptr, uint64_t v) {
  assert(is_aligned_uint64(ptr));
  *ptr = cpu_to_le64(v);
}

/*----------------------------------------------------------------------------*/

#if UNALIGNED_OK

static inline uint16_t get_le16_unaligned(const void *ptr) {
  return le16_to_cpu(*(const uint16_t *)ptr);
}

static inline void set_le16_unaligned(void *ptr, uint16_t v) {
  *(uint16_t *)ptr = cpu_to_le16(v);
}

static inline uint32_t get_le32_unaligned(const void *ptr) {
  return le32_to_cpu(*(const uint32_t *)ptr);
}

static inline void set_le32_unaligned(void *ptr, uint32_t v) {
  *(uint32_t *)ptr = cpu_to_le32(v);
}

static inline uint32_t get_le32_aligned16(const void *ptr) {
  assert(is_aligned_uint16(ptr));
  return get_le32_unaligned(ptr);
}

static inline void set_le32_aligned16(void *ptr, uint32_t v) {
  assert(is_aligned_uint16(ptr));
  set_le32_unaligned(ptr, v);
}

static inline uint64_t get_le64_unaligned(const void *ptr) {
  return le64_to_cpu(*(const uint64_t *)ptr);
}

static inline void set_le64_unaligned(void *ptr, uint64_t v) {
  *(uint64_t *)ptr = cpu_to_le64(v);
}

static inline uint64_t get_le64_aligned16(const void *ptr) {
  assert(is_aligned_uint16(ptr));
  return get_le64_unaligned(ptr);
}

static inline void set_le64_aligned16(void *ptr, uint64_t v) {
  assert(is_aligned_uint16(ptr));
  set_le64_unaligned(ptr, v);
}

static inline uint64_t get_le64_aligned32(const void *ptr) {
  assert(is_aligned_uint32(ptr));
  return get_le64_unaligned(ptr);
}

static inline void set_le64_aligned32(void *ptr, uint64_t v) {
  assert(is_aligned_uint32(ptr));
  set_le64_unaligned(ptr, v);
}

#else /* UNALIGNED_OK */

static uint16_t get_le16_unaligned(const void *ptr) {
  const uint8_t *bytes = (const uint8_t *)ptr;
  return bytes[0] | (uint16_t)bytes[1] << 8;
}

static void set_le16_unaligned(void *ptr, uint16_t v) {
  uint8_t *bytes = (uint8_t *)ptr;
  bytes[0] = (uint8_t)v;
  bytes[1] = (uint8_t)(v >> 8);
}

static uint32_t get_le32_unaligned(const void *ptr) {
  const uint8_t *bytes = (const uint8_t *)ptr;
  uint32_t r = bytes[0];
  r |= (uint32_t)bytes[1] << 8;
  r |= (uint32_t)bytes[2] << 16;
  r |= (uint32_t)bytes[3] << 24;
  return r;
}

static void set_le32_unaligned(void *ptr, uint32_t v) {
  uint8_t *bytes = (uint8_t *)ptr;
  bytes[0] = (uint8_t)v;
  bytes[1] = (uint8_t)(v >> 8);
  bytes[2] = (uint8_t)(v >> 16);
  bytes[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32_aligned16(const void *ptr) {
  assert(is_aligned_uint16(ptr));
  const uint16_t *words = (const uint16_t *)ptr;
  return get_le16_aligned(words + 0) |
         (uint32_t)get_le16_aligned(words + 1) << 16;
}

static void set_le32_aligned16(void *ptr, uint32_t v) {
  assert(is_aligned_uint16(ptr));
  uint16_t *words = (uint16_t *)ptr;
  set_le16_aligned(words + 0, (uint16_t)v);
  set_le16_aligned(words + 1, (uint16_t)(v >> 16));
}

static uint64_t get_le64_unaligned(const void *ptr) {
  const uint8_t *bytes = (const uint8_t *)ptr;
  uint64_t r = bytes[0];
  r |= (uint64_t)bytes[1] << 8;
  r |= (uint64_t)bytes[2] << 16;
  r |= (uint64_t)bytes[3] << 24;
  r |= (uint64_t)bytes[4] << 32;
  r |= (uint64_t)bytes[5] << 40;
  r |= (uint64_t)bytes[6] << 48;
  r |= (uint64_t)bytes[7] << 56;
  return r;
}

static void set_le64_unaligned(void *ptr, uint64_t v) {
  uint8_t *bytes = (uint8_t *)ptr;
  bytes[0] = (uint8_t)v;
  bytes[1] = (uint8_t)(v >> 8);
  bytes[2] = (uint8_t)(v >> 16);
  bytes[3] = (uint8_t)(v >> 24);
  bytes[4] = (uint8_t)(v >> 32);
  bytes[5] = (uint8_t)(v >> 40);
  bytes[6] = (uint8_t)(v >> 48);
  bytes[7] = (uint8_t)(v >> 56);
}

static uint64_t get_le64_aligned16(const void *ptr) {
  assert(is_aligned_uint16(ptr));
  const uint16_t *words = (const uint16_t *)ptr;
  uint64_t r = get_le16_aligned(words + 0);
  r |= (uint64_t)get_le16_aligned(words + 1) << 16;
  r |= (uint64_t)get_le16_aligned(words + 2) << 32;
  r |= (uint64_t)get_le16_aligned(words + 3) << 48;
  return r;
}

static void set_le64_aligned16(void *ptr, uint64_t v) {
  assert(is_aligned_uint16(ptr));
  uint16_t *words = (uint16_t *)ptr;
  set_le16_aligned(words + 0, (uint16_t)v);
  set_le16_aligned(words + 1, (uint16_t)(v >> 16));
  set_le16_aligned(words + 2, (uint16_t)(v >> 32));
  set_le16_aligned(words + 3, (uint16_t)(v >> 48));
}

static uint64_t get_le64_aligned32(const void *ptr) {
  assert(is_aligned_uint32(ptr));
  const uint32_t *dwords = (const uint32_t *)ptr;
  return get_le32_aligned(dwords + 0) |
         (uint64_t)get_le32_aligned(dwords + 1) << 32;
}

static void set_le64_aligned32(void *ptr, uint64_t v) {
  assert(is_aligned_uint32(ptr));
  uint32_t *dwords = (uint32_t *)ptr;
  set_le32_aligned(dwords + 0, (uint32_t)v);
  set_le32_aligned(dwords + 1, (uint32_t)(v >> 32));
}

#endif /* UNALIGNED_OK */
