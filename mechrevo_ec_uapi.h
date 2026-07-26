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

#define MECHREVO_EC_BLOCK_MAX	128

/* Contiguous XRAM transfer. length bytes starting at addr are valid. */
struct mechrevo_ec_block {
	__u16 addr;
	__u16 length;
	__u8 data[MECHREVO_EC_BLOCK_MAX];
};

#define MECHREVO_EC_OP_READ		0
#define MECHREVO_EC_OP_WRITE		1
#define MECHREVO_EC_OP_UPDATE_BITS	2
#define MECHREVO_EC_XFER_MAX_OPS	128

/*
 * A transaction is executed under one device mutex. READ stores the result
 * in value. WRITE writes value. UPDATE_BITS replaces mask-selected bits with
 * value and returns the resulting byte in value.
 */
struct mechrevo_ec_op {
	__u16 addr;
	__u8 type;
	__u8 value;
	__u8 mask;
	__u8 reserved;
};

struct mechrevo_ec_xfer {
	__u16 count;
	__u16 reserved;
	struct mechrevo_ec_op ops[MECHREVO_EC_XFER_MAX_OPS];
};

#define MECHREVO_EC_IOC_MAGIC	'M'
#define MECHREVO_EC_IOC_READ \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x00, struct mechrevo_ec_io)
#define MECHREVO_EC_IOC_WRITE \
	_IOW(MECHREVO_EC_IOC_MAGIC, 0x01, struct mechrevo_ec_io)
#define MECHREVO_EC_IOC_UPDATE_BITS \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x02, struct mechrevo_ec_io)
#define MECHREVO_EC_IOC_READ_BLOCK \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x03, struct mechrevo_ec_block)
#define MECHREVO_EC_IOC_WRITE_BLOCK \
	_IOW(MECHREVO_EC_IOC_MAGIC, 0x04, struct mechrevo_ec_block)
#define MECHREVO_EC_IOC_XFER \
	_IOWR(MECHREVO_EC_IOC_MAGIC, 0x05, struct mechrevo_ec_xfer)

#endif /* _UAPI_MECHREVO_EC_H */
