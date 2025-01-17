#ifndef __LSMT_RO_H__
#define __LSMT_RO_H__

#include <linux/blk-mq.h>
#include <linux/kthread.h>
#include <linux/uuid.h>

#include "vfile.h"

struct zfile;

struct lsmt_ht {
	uint64_t magic0;
	uuid_t magic1;
	// offset 24, 28
	uint32_t size; //= sizeof(HeaderTrailer);
	uint32_t flags; //= 0;
	// offset 32, 40, 48
	uint64_t index_offset; // in bytes
	uint64_t index_size; // # of SegmentMappings
	uint64_t virtual_size; // in bytes
} __attribute__((packed));

struct segment {
	uint64_t offset : 50;
	uint32_t length : 14;
};

struct segment_mapping { /* 8 + 8 bytes */
	uint64_t offset : 50; // offset (0.5 PB if in sector)
	uint32_t length : 14;
	uint64_t moffset : 55; // mapped offset (2^64 B if in sector)
	uint32_t zeroed : 1; // indicating a zero-filled segment
	uint8_t tag;
};

struct lsmt_ro_index {
	const struct segment_mapping *pbegin;
	const struct segment_mapping *pend;
	struct segment_mapping *mapping;
};

struct lsmt_file {
	struct vfile vfile;
	struct vfile *fp;
	bool ownership;
	int nr;
	struct lsmt_ht ht;
	struct lsmt_ro_index index;
	struct bio_set bioset;
};

// lsmt_file functions...
// in `lsmt_file`, all data read by using `zfile_read`
struct lsmt_file *lsmt_open_ro(struct vfile *zf, bool ownership);

// TODO: load multiple layer index
// lsmt_file merge
// open multiple files and merge as one lsmt file output
// struct lsmt_file *lsmt_open_merge(struct vfile *lf, int n);

// TODO: lsmt_open_rw support
// struct lsmt_file* lsmt_open_rw(struct zfile* zf, int n, struct file* wfile);
bool is_lsmtfile(struct vfile *zf);

#endif