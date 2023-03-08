#include <linux/module.h>
#include <linux/version.h> /* LINUX_VERSION_CODE  */
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include "rdma_library.h"
#define RBLK_NAME "rblk"
#define RBLK_MINORS 1
#define KERNEL_SECTOR_SIZE 512
#define NSECTORS (1 << 21)		   /*suppose our drive size is 1G*/
#define BUFFER_NSECTORES (1 << 13) /* 4M/512B */
#define HARDSECT_SIZE KERNEL_SECTOR_SIZE
MODULE_LICENSE("Dual BSD/GPL");

static int rblk_major = 0;
module_param(rblk_major, int, 0);
MODULE_PARM_DESC(rblk_major, "device major number");

static short rblk_remote_port = RDMA_DEFAULT_PORT;
module_param(rblk_remote_port, short, 0);
MODULE_PARM_DESC(rblk_remote_port, "remote port to connect to");

static char *rblk_remote_addr = RDMA_DEFAULT_SERVER_ADDR;
module_param(rblk_remote_addr, charp, 0);
MODULE_PARM_DESC(rblk_remote_addr, "remote address to connect to");

static int rblk_remote_npage = RDMA_DEFAULT_REMOTE_NPAGE;
module_param(rblk_remote_npage, int, 0);
MODULE_PARM_DESC(rblk_remote_npage, "remote pages number");

struct rblk_device
{
	/* For mutual exclusion. */
	spinlock_t lock; /* remember to initialize this*/
	int users;
	/* For block device*/
	struct gendisk *gdisk;
	struct request_queue *queue;
	/* For rdma ability*/
	u8 *kbuff;	  /* local buffer, default size is 4M*/
	int buf_size; /* buffer size in bytes*/
	int capacity; /* device capacity in bytes*/
	int slot;
	rdma_ctx_t rdma_ctx;
};

struct rblk_device *device;

static int dump_slot(struct rblk_device *dev)
{
	if (dev->slot < 0 || dev->slot >= dev->capacity / dev->buf_size)
	{
		return -1;
	}
	do_rdma(dev->kbuff, dev->buf_size, dev->slot * dev->buf_size, dev->rdma_ctx, 1);
	return 0;
}

static int load_slot(struct rblk_device *dev, int target_slot)
{
	if (target_slot < 0 || target_slot >= dev->capacity / dev->buf_size)
	{
		return -1;
	}
	do_rdma(dev->kbuff, dev->buf_size, target_slot * dev->buf_size, dev->rdma_ctx, 0);
	dev->slot = target_slot;
	return 0;
}

/*
 * Handle an I/O request.
 */
static void rblk_transfer(struct rblk_device *dev, unsigned long sector,
						  unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector * KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect * KERNEL_SECTOR_SIZE;
	int slot = sector / BUFFER_NSECTORES;

	if ((offset + nbytes) > dev->capacity)
	{
		// 0_o I hope we never arrive here.
		printk(KERN_ERR "Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	// this is a cross slot segment, hanlde it specially.
	if ((offset + nbytes) >= dev->buf_size)
	{
		dump_slot(dev);
		do_rdma(dev->kbuff, nbytes, offset, dev->rdma_ctx, write);
		return;
	}
	if (slot != dev->slot)
	{
		dump_slot(dev);
		load_slot(dev, slot);
	}
	if (write)
		memcpy(dev->kbuff + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->kbuff + offset, nbytes);
}

/*
 * Transfer a single BIO.
 */
static int sbull_xfer_bio(struct rblk_device *dev, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;
	char *buffer;

	printk(KERN_DEBUG "rblk: going to transfer a bio at sector:%lld\n", sector);
	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, iter)
	{

		buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
		rblk_transfer(dev, sector, (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE),
					  buffer, bio_data_dir(bio) == WRITE);
		sector += (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE);
		kunmap_atomic(buffer);
	}
	return 0; /* Always "succeed" */
}

/*
 * The direct make request version.
 */
static blk_qc_t rblk_make_request(struct request_queue *q, struct bio *bio)
{
	struct rblk_device *dev = bio->bi_disk->private_data;
	int status;
	status = sbull_xfer_bio(dev, bio);
	bio->bi_status = status;
	bio_endio(bio);
	return BLK_QC_T_NONE;
}

/*
 * Open and close.
 */

static int rblk_open(struct block_device *bdev, fmode_t mode)
{
	struct rblk_device *dev = bdev->bd_disk->private_data;
	spin_lock(&dev->lock);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void rblk_release(struct gendisk *disk, fmode_t mode)
{
	struct rblk_device *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;
	spin_unlock(&dev->lock);
}

struct block_device_operations rblk_ops = {
	.owner = THIS_MODULE,
	.open = rblk_open,
	.release = rblk_release};

static int setup_device(struct rblk_device *dev)
{
	spin_lock_init(&dev->lock);
	dev->queue = blk_alloc_queue(GFP_KERNEL);
	if (dev->queue == NULL)
	{
		return -ENOMEM; /*suppose it's because no memory*/
	}
	blk_queue_make_request(dev->queue, rblk_make_request);
	blk_queue_logical_block_size(dev->queue, HARDSECT_SIZE);
	/* Assign private data to queue structure. */
	dev->queue->queuedata = dev;

	/* Initialize the gendisk structure */
	dev->gdisk = alloc_disk(RBLK_MINORS);
	if (!dev->gdisk)
	{
		printk(KERN_ERR "alloc_disk fail.\n");
		// TODO: maybe need to free resources hold by dev->queue, although sbull did not handle this.
		blk_cleanup_queue(dev->queue);
		return -ENOMEM;
	}
	dev->gdisk->major = rblk_major;
	dev->gdisk->flags = GENHD_FL_NO_PART_SCAN;
	dev->gdisk->first_minor = 0;
	dev->gdisk->fops = &rblk_ops;
	dev->gdisk->queue = dev->queue;
	dev->gdisk->private_data = dev;
	snprintf(dev->gdisk->disk_name, 32, "rblk");
	set_capacity(dev->gdisk, NSECTORS * (HARDSECT_SIZE / KERNEL_SECTOR_SIZE));
	// TODO: maybe I need to consider will this overflow?
	dev->buf_size = BUFFER_NSECTORES * HARDSECT_SIZE;
	dev->capacity = NSECTORS * HARDSECT_SIZE;
	dev->slot = -1;
	dev->kbuff = kmalloc(BUFFER_NSECTORES * HARDSECT_SIZE, GFP_KERNEL);
	if (!dev->kbuff)
	{
		printk(KERN_ERR "allocate kbuff failure.\n");
		del_gendisk(dev->gdisk);
		return -ENOMEM;
	}
	dev->rdma_ctx = rdma_init(rblk_remote_npage, rblk_remote_addr, rblk_remote_port, dev->kbuff, dev->buf_size);
	if (!device->rdma_ctx || load_slot(dev, 0) != 0)
	{
		printk(KERN_DEBUG "rblk: rdma_init fail.\n");
		del_gendisk(dev->gdisk);
		kfree(dev->kbuff);
		return -ENOMEM;
	}
	// /* call this only if everything is ready!!! */
	add_disk(dev->gdisk);
	return 0;
}

static int __init rblk_init(void)
{
	int status = -EBUSY;
	rblk_major = register_blkdev(rblk_major, RBLK_NAME);
	if (rblk_major <= 0)
	{
		printk("unable to register rblk device");
		return -EBUSY;
	}
	device = kzalloc(sizeof(struct rblk_device), GFP_KERNEL);
	if (!device)
	{
		status = -ENOMEM;
		printk(KERN_DEBUG "rblk: kzalloca for device fail.\n");
		goto out_unregister;
	}

	status = setup_device(device);
	if (status)
	{
		printk(KERN_DEBUG "rblk: setup_device fail.\n");
		goto out_free_device;
	}
	printk(KERN_ALERT "Hello, world\n");
	return 0;
out_free_device:
	kfree(device);
out_unregister:
	unregister_blkdev(rblk_major, RBLK_NAME);
	return status;
}

static void delete_block_device(struct rblk_device *dev)
{
	if (dev->gdisk)
	{
		del_gendisk(dev->gdisk);
		put_disk(dev->gdisk);
	}
	if (dev->queue)
	{
		blk_cleanup_queue(dev->queue);
	}
	if (dev->kbuff)
	{
		kfree(dev->kbuff);
	}
}

static void __exit rblk_exit(void)
{
	if (device == NULL)
	{
		return;
	}
	rdma_exit(device->rdma_ctx);
	delete_block_device(device);
	kfree(device);
	unregister_blkdev(rblk_major, RBLK_NAME);
	printk(KERN_ALERT "Goodbye, cruel world\n");
}

module_init(rblk_init);
module_exit(rblk_exit);
