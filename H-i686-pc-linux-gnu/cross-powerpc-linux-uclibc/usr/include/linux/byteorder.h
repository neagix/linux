#ifndef _LINUX_BYTEORDER_H
#define _LINUX_BYTEORDER_H

#include <linux/types.h>
#include <linux/swab.h>

#if defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)
# error Fix asm/byteorder.h to define one endianness
#endif

#if !defined(__LITTLE_ENDIAN) && !defined(__BIG_ENDIAN)
# error Fix asm/byteorder.h to define arch endianness
#endif

#ifdef __LITTLE_ENDIAN
# undef __LITTLE_ENDIAN
# define __LITTLE_ENDIAN 1234
#endif

#ifdef __BIG_ENDIAN
# undef __BIG_ENDIAN
# define __BIG_ENDIAN 4321
#endif

#if defined(__LITTLE_ENDIAN) && !defined(__LITTLE_ENDIAN_BITFIELD)
# define __LITTLE_ENDIAN_BITFIELD
#endif

#if defined(__BIG_ENDIAN) && !defined(__BIG_ENDIAN_BITFIELD)
# define __BIG_ENDIAN_BITFIELD
#endif

#ifdef __LITTLE_ENDIAN
# define __le16_to_cpu(x) ((__u16)(__le16)(x))
# define __le32_to_cpu(x) ((__u32)(__le32)(x))
# define __le64_to_cpu(x) ((__u64)(__le64)(x))
# define __cpu_to_le16(x) ((__le16)(__u16)(x))
# define __cpu_to_le32(x) ((__le32)(__u32)(x))
# define __cpu_to_le64(x) ((__le64)(__u64)(x))

# define __be16_to_cpu(x) __swab16((__u16)(__be16)(x))
# define __be32_to_cpu(x) __swab32((__u32)(__be32)(x))
# define __be64_to_cpu(x) __swab64((__u64)(__be64)(x))
# define __cpu_to_be16(x) ((__be16)__swab16(x))
# define __cpu_to_be32(x) ((__be32)__swab32(x))
# define __cpu_to_be64(x) ((__be64)__swab64(x))
#endif

#ifdef __BIG_ENDIAN
# define __be16_to_cpu(x) ((__u16)(__be16)(x))
# define __be32_to_cpu(x) ((__u32)(__be32)(x))
# define __be64_to_cpu(x) ((__u64)(__be64)(x))
# define __cpu_to_be16(x) ((__be16)(__u16)(x))
# define __cpu_to_be32(x) ((__be32)(__u32)(x))
# define __cpu_to_be64(x) ((__be64)(__u64)(x))

# define __le16_to_cpu(x) __swab16((__u16)(__le16)(x))
# define __le32_to_cpu(x) __swab32((__u32)(__le32)(x))
# define __le64_to_cpu(x) __swab64((__u64)(__le64)(x))
# define __cpu_to_le16(x) ((__le16)__swab16(x))
# define __cpu_to_le32(x) ((__le32)__swab32(x))
# define __cpu_to_le64(x) ((__le64)__swab64(x))
#endif

/*
 * These helpers could be phased out over time as the base version
 * handles constant folding.
 */
#define __constant_htonl(x) __cpu_to_be32(x)
#define __constant_ntohl(x) __be32_to_cpu(x)
#define __constant_htons(x) __cpu_to_be16(x)
#define __constant_ntohs(x) __be16_to_cpu(x)

#define __constant_le16_to_cpu(x) __le16_to_cpu(x)
#define __constant_le32_to_cpu(x) __le32_to_cpu(x)
#define __constant_le64_to_cpu(x) __le64_to_cpu(x)
#define __constant_be16_to_cpu(x) __be16_to_cpu(x)
#define __constant_be32_to_cpu(x) __be32_to_cpu(x)
#define __constant_be64_to_cpu(x) __be64_to_cpu(x)

#define __constant_cpu_to_le16(x) __cpu_to_le16(x)
#define __constant_cpu_to_le32(x) __cpu_to_le32(x)
#define __constant_cpu_to_le64(x) __cpu_to_le64(x)
#define __constant_cpu_to_be16(x) __cpu_to_be16(x)
#define __constant_cpu_to_be32(x) __cpu_to_be32(x)
#define __constant_cpu_to_be64(x) __cpu_to_be64(x)

static inline void __le16_to_cpus(__u16 *p)
{
#ifdef __BIG_ENDIAN
	__swab16s(p);
#endif
}

static inline void __cpu_to_le16s(__u16 *p)
{
#ifdef __BIG_ENDIAN
	__swab16s(p);
#endif
}

static inline void __le32_to_cpus(__u32 *p)
{
#ifdef __BIG_ENDIAN
	__swab32s(p);
#endif
}

static inline void __cpu_to_le32s(__u32 *p)
{
#ifdef __BIG_ENDIAN
	__swab32s(p);
#endif
}

static inline void __le64_to_cpus(__u64 *p)
{
#ifdef __BIG_ENDIAN
	__swab64s(p);
#endif
}

static inline void __cpu_to_le64s(__u64 *p)
{
#ifdef __BIG_ENDIAN
	__swab64s(p);
#endif
}

static inline void __be16_to_cpus(__u16 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab16s(p);
#endif
}

static inline void __cpu_to_be16s(__u16 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab16s(p);
#endif
}

static inline void __be32_to_cpus(__u32 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab32s(p);
#endif
}

static inline void __cpu_to_be32s(__u32 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab32s(p);
#endif
}

static inline void __be64_to_cpus(__u64 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab64s(p);
#endif
}

static inline void __cpu_to_be64s(__u64 *p)
{
#ifdef __LITTLE_ENDIAN
	__swab64s(p);
#endif
}

static inline __u16 __le16_to_cpup(const __le16 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__u16)*p;
#else
	return __swab16p((__u16 *)p);
#endif
}

static inline __u32 __le32_to_cpup(const __le32 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__u32)*p;
#else
	return __swab32p((__u32 *)p);
#endif
}

static inline __u64 __le64_to_cpup(const __le64 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__u64)*p;
#else
	return __swab64p((__u64 *)p);
#endif
}

static inline __le16 __cpu_to_le16p(const __u16 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__le16)*p;
#else
	return (__le16)__swab16p(p);
#endif
}

static inline __le32 __cpu_to_le32p(const __u32 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__le32)*p;
#else
	return (__le32)__swab32p(p);
#endif
}

static inline __le64 __cpu_to_le64p(const __u64 *p)
{
#ifdef __LITTLE_ENDIAN
	return (__le64)*p;
#else
	return (__le64)__swab64p(p);
#endif
}

static inline __u16 __be16_to_cpup(const __be16 *p)
{
#ifdef __BIG_ENDIAN
	return (__u16)*p;
#else
	return __swab16p((__u16 *)p);
#endif
}

static inline __u32 __be32_to_cpup(const __be32 *p)
{
#ifdef __BIG_ENDIAN
	return (__u32)*p;
#else
	return __swab32p((__u32 *)p);
#endif
}

static inline __u64 __be64_to_cpup(const __be64 *p)
{
#ifdef __BIG_ENDIAN
	return (__u64)*p;
#else
	return __swab64p((__u64 *)p);
#endif
}

static inline __be16 __cpu_to_be16p(const __u16 *p)
{
#ifdef __BIG_ENDIAN
	return (__be16)*p;
#else
	return (__be16)__swab16p(p);
#endif
}

static inline __be32 __cpu_to_be32p(const __u32 *p)
{
#ifdef __BIG_ENDIAN
	return (__be32)*p;
#else
	return (__be32)__swab32p(p);
#endif
}

static inline __be64 __cpu_to_be64p(const __u64 *p)
{
#ifdef __BIG_ENDIAN
	return (__be64)*p;
#else
	return (__be64)__swab64p(p);
#endif
}

#endif /* _LINUX_BYTEORDER_H */
