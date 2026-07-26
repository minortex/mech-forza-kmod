/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MECHREVO_EC_H
#define _UAPI_MECHREVO_EC_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * value is the byte read/written. For UPDATE_BITS, mask selects the bits
 * replaced by value and value is returned as the resulting EC byte.
 */
struct mechrevo_ec_io {
	__u16 addr;
	__u8 value;
	__u8 mask;
};

#define MECHREVO_EC_IOC_MAGIC	'M'
#define MECHREVO_EC_IOC_READ \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x00, struct mechrevo_ec_io)
#define MECHREVO_EC_IOC_WRITE \
	_IOW(MECHREVO_EC_IOC_MAGIC, 0x01, struct mechrevo_ec_io)
#define MECHREVO_EC_IOC_UPDATE_BITS \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x02, struct mechrevo_ec_io)

#endif /* _UAPI_MECHREVO_EC_H */
