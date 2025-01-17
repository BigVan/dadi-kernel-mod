#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "lsmt.h"

/* This is a structure stores information about the underlying device
 * Param:
 *  dev : Underlying device
 *  start: Starting sector number of the device
 */
struct lsmt_dm_target {
	struct dm_dev *dev[256];
	struct lsmt_file *lsmt;
	struct vfile *bf[256];
	unsigned int nr;
};

/* This is map function of basic target. This function gets called whenever you
 * get a new bio request.The working of map function is to map a particular bio
 * request to the underlying device. The request that we receive is submitted to
 * out device so  bio->bi_bdev points to our device. We should point to the
 * bio-> bi_dev field to bdev of underlying device. Here in this function, we
 * can have other processing like changing sector number of bio request,
 * splitting bio etc.
 *
 * Param :
 *  ti : It is the dm_target structure representing our basic target
 *  bio : The block I/O request from upper layer
 *  map_context : Its mapping context of target.
 *
 * Return values from target map function:
 *  DM_MAPIO_SUBMITTED :  Your target has submitted the bio request to
 * underlying request. DM_MAPIO_REMAPPED  :  Bio request is remapped, Device
 * mapper should submit bio. DM_MAPIO_REQUEUE   :  Some problem has happened
 * with the mapping of bio, So requeue the bio request. So the bio will be
 * submitted to the map function.
 */

static int lsmt_target_map(struct dm_target *ti, struct bio *bio)
{
	struct lsmt_dm_target *mdt = (struct lsmt_dm_target *)ti->private;
	// printk(KERN_CRIT "\n<<in function lsmt_target_map \n");

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		// pr_info("sec: %lld vcnt: %d\n", bio->bi_iter.bi_sector,
		//         bio->bi_vcnt);
		return mdt->lsmt->vfile.op->bio_remap((struct vfile *)mdt->lsmt,
						      bio, mdt->dev, mdt->nr);
	}
	return DM_MAPIO_KILL;
}

static int lsmt_target_end_io(struct dm_target *ti, struct bio *bio,
			      blk_status_t *error)
{
	//     struct lsmt_dm_target *mdt = (struct lsmt_dm_target *)ti->private;
	return DM_ENDIO_DONE;
}

// special helper
// access blockdev data by sync
// copy to buffer
static ssize_t sync_read_blkdev(struct block_device *dev, void *buf,
				size_t count, loff_t offset)
{
	void *mem = NULL;
	struct page *pg = NULL;
	struct bio *bio = NULL;
	loff_t left = offset & PAGE_MASK;
	loff_t right = (offset + count + PAGE_SIZE - 1) & PAGE_MASK;
	loff_t i = 0;
	size_t sg_len = 0;
	ssize_t ret = 0;
	for (i = left; i < right; i += PAGE_SIZE) {
		pg = alloc_page(GFP_KERNEL);
		get_page(pg);
		bio = bio_alloc(GFP_NOIO, 1);
		if (!pg || !bio) {
			ret = -EIO;
			goto out;
		}
		bio_set_dev(bio, dev);
		bio_add_page(bio, pg, PAGE_SIZE, 0);
		bio->bi_iter.bi_sector = i >> SECTOR_SHIFT;
		bio_set_op_attrs(bio, REQ_OP_READ, 0);
		sg_len = (count < (i + PAGE_SIZE - offset)) ?
				 count :
				 (i + PAGE_SIZE - offset);
		pr_info("sg_len=%ld\n", sg_len);
		submit_bio_wait(bio);
		mem = kmap_atomic(pg);
		memcpy(buf, mem + (offset - i), sg_len);
		buf += sg_len;
		offset += sg_len;
		ret += sg_len;
		count -= sg_len;
		kunmap_atomic(mem);
		bio_put(bio);
		put_page(pg);
	}
out:
	return ret;
}

struct blkdev_as_vfile {
	struct vfile vfile;
	struct block_device *dev;
};

static size_t blkdev_len(struct vfile *ctx)
{
	struct blkdev_as_vfile *bf = (struct blkdev_as_vfile *)ctx;
	return get_capacity(bf->dev->bd_disk) << SECTOR_SHIFT;
}

static ssize_t blkdev_pread(struct vfile *ctx, void *buf, size_t count,
			    loff_t offset)
{
	struct blkdev_as_vfile *bf = (struct blkdev_as_vfile *)ctx;
	return sync_read_blkdev(bf->dev, buf, count, offset);
}

static void blkdev_close(struct vfile *ctx)
{
	// kfree(ctx);
	return;
}

static struct vfile_op blkdev_op = {
	.len = blkdev_len,
	.pread = blkdev_pread,
	.close = blkdev_close,
};

static struct blkdev_as_vfile *open_blkdev_as_vfile(struct block_device *blk)
{
	struct blkdev_as_vfile *ret =
		kzalloc(sizeof(struct blkdev_as_vfile), GFP_KERNEL);
	if (!ret)
		return NULL;
	ret->vfile.op = &blkdev_op;
	ret->dev = blk;
	return ret;
}

/* This is Constructor Function of basic target
 *  Constructor gets called when we create some device of type 'lsmt_target'.
 *  So it will get called when we execute command 'dmsetup create'
 *  This  function gets called for each device over which you want to create
 * basic target. Here it is just a basic target so it will take only one device
 * so it will get called once.
 */
static int lsmt_target_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct lsmt_dm_target *mdt;
	struct mapped_device *md;
	struct gendisk *disk;
	const char *devname;
	struct dm_arg_set args = { .argc = argc, .argv = argv };
	struct dm_arg arg = { .min = 1,
			      .max = 255,
			      .error = "Layer number not valid" };
	int i;
	int r;

	printk(KERN_CRIT "\n >>in function lsmt_target_ctr \n");

	if (argc < 2) {
		printk(KERN_CRIT "\n Invalid no.of arguments.\n");
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	mdt = kmalloc(sizeof(struct lsmt_dm_target), GFP_KERNEL);

	if (mdt == NULL) {
		printk(KERN_CRIT "\n Mdt is null\n");
		ti->error = "dm-lsmt_target: Cannot allocate context";
		return -ENOMEM;
	}

	// dmt->lsmt = lsmt_open_ro(x, 1);

	// if (sscanf(argv[1], "%llu", &start) != 1) {
	//     ti->error = "dm-lsmt_target: Invalid device sector";
	//     goto bad;
	// }

	// mdt->start = (sector_t)start;

	/* dm_get_table_mode
     * Gives out you the Permissions of device mapper table.
     * This table is nothing but the table which gets created
     * when we execute dmsetup create. This is one of the
     * Data structure used by device mapper for keeping track of its devices.
     *
     * dm_get_device
     * The function sets the mdt->dev field to underlying device dev structure.
     */

	r = dm_read_arg_group(&arg, &args, &mdt->nr, &ti->error);
	if (r)
		return -EINVAL;

	if (mdt->nr != 1) {
		printk(KERN_CRIT "\n Merged lsmtfile not implemented");
		ti->error = "dm-lsmt_target: Merged lsmtfile not implemented";
		return -ENOSYS;
	}

	for (i = 0; i < mdt->nr; i++) {
		devname = dm_shift_arg(&args);
		printk(KERN_INFO "\nlsmt-md: load dev %s\n", devname);
		if (dm_get_device(ti, devname, dm_table_get_mode(ti->table),
				  &mdt->dev[i])) {
			ti->error = "dm-lsmt_target: Device lookup failed";
			goto bad;
		}

		if (!mdt->dev[i] || !mdt->dev[i]->bdev) {
			pr_warn("failed to get mdt dev or bdev\n");
			goto error_out;
		}
		mdt->bf[i] =
			(struct vfile *)open_blkdev_as_vfile(mdt->dev[i]->bdev);
	}

	// TODO: load multiple layer index
	mdt->lsmt = lsmt_open_ro((struct vfile *)mdt->bf[0], false);

	if (!mdt->lsmt) {
		pr_crit("Failed to open lsmt file");
		goto error_out;
	}

	pr_info("dm-ovbd: blk size is %lu\n",
		mdt->lsmt->vfile.op->len((struct vfile *)mdt->lsmt));

	// TODO: seems set disk RO and set capacity are useless
	md = dm_table_get_md(ti->table);
	disk = dm_disk(md);
	set_disk_ro(disk, 1);
	set_capacity(disk,
		     mdt->lsmt->vfile.op->len((struct vfile *)mdt->lsmt) >>
			     SECTOR_SHIFT);

	ti->private = mdt;

	printk(KERN_CRIT "\n>>out function lsmt_target_ctr \n");
	return 0;

error_out:
	for (i = 0; i < mdt->nr; i++) {
		if (mdt->bf[i])
			mdt->bf[i]->op->close((struct vfile *)mdt->bf[i]);
	}

	for (i = 0; i < mdt->nr; i++) {
		if (mdt->dev[i])
			dm_put_device(ti, mdt->dev[i]);
	}
bad:
	kfree(mdt);
	printk(KERN_CRIT "\n>>out function lsmt_target_ctr with error \n");
	return -EINVAL;
}

/*
 * This is destruction function
 * This gets called when we remove a device of type basic target. The function
 * gets called per device.
 */
static void lsmt_target_dtr(struct dm_target *ti)
{
	struct lsmt_dm_target *mdt = (struct lsmt_dm_target *)ti->private;
	unsigned int i = 0;
	printk(KERN_CRIT "\n<<in function lsmt_target_dtr \n");
	if (mdt->lsmt)
		mdt->lsmt->vfile.op->close((struct vfile *)mdt->lsmt);
	for (i = 0; i < mdt->nr; i++) {
		if (mdt->bf[i])
			mdt->bf[i]->op->close((struct vfile *)mdt->bf);
		dm_put_device(ti, mdt->dev[i]);
	}
	kfree(mdt);
	printk(KERN_CRIT "\n>>out function lsmt_target_dtr \n");
}

/*
 * This structure is fops for basic target.
 */
static struct target_type lsmt_target = {
	.features = 0,
	.name = "lsmt_target",
	.version = { 1, 0, 0 },
	.module = THIS_MODULE,
	.ctr = lsmt_target_ctr,
	.dtr = lsmt_target_dtr,
	.map = lsmt_target_map,
	.end_io = lsmt_target_end_io,
};

/*-------------------------------------------Module Functions
 * ---------------------------------*/

static int init_lsmt_target(void)
{
	int result;
	result = dm_register_target(&lsmt_target);
	if (result < 0)
		printk(KERN_CRIT "\n Error in registering target \n");
	return 0;
}

static void cleanup_lsmt_target(void)
{
	dm_unregister_target(&lsmt_target);
}

module_init(init_lsmt_target);
module_exit(cleanup_lsmt_target);
MODULE_LICENSE("GPL");