// SPDX-License-Identifier: GPL-2.0
#define BLK_SNAP_SECTION "-snapimage"
#include "snapimage.h"
#include "cbt_map.h"
#include "tracker.h"

#include <linux/cdrom.h>
#include <linux/blk-mq.h>
//#include <linux/hdreg.h>


#define SNAPIMAGE_MAX_DEVICES 2048

int snapimage_major;
unsigned long *snapimage_minors;
DEFINE_SPINLOCK(snapimage_minors_lock);

LIST_HEAD(snap_images);
DECLARE_RWSEM(snap_images_lock);

DECLARE_RWSEM(snap_image_destroy_lock);

struct snapimage {
	struct list_head link;
	dev_t image_dev;
	sector_t capacity;


	struct diff_area *diff_area;
	struct cbt_map *cbt_map;

	//struct mutex open_locker;
	//struct block_device *open_bdev;
	//size_t open_cnt;
};


static const struct blk_mq_ops image_mq_ops = {
	.queue_rq       = snapimage_queue_rq,
};

const struct block_device_operations snapimage_ops = {
	.owner = THIS_MODULE,
	.open = snapimage_open,
	.ioctl = snapimage_ioctl,
	.release = snapimage_close,
};

static int snapimage_get_capasity(dev_t dev_id, sector_t *capacity)
{
	struct block_device *bdev;

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Failed to open device. errno=%ld\n", PTR_ERR(bdev));
		return PTR_ERR(bdev);
	}

	*capacity = part_nr_sects_read(diff_area->bdev->bd_part);

	blkdev_put(bdev);
	return 0;
}

static int _snapimage_create(dev_t original_dev_id)
{
	int res = 0;
	struct snapimage *image = NULL;
	struct gendisk *disk = NULL;
	struct diff_area *diff_area = NULL;
	int minor;
	struct blk_dev_info original_dev_info;

	pr_info("Create snapshot image for device [%d:%d]\n",
	        MAJOR(original_dev_id), MINOR(original_dev_id));

	res = blk_dev_get_info(original_dev_id, &original_dev_info);
	if (res) {
		pr_err("Failed to obtain original device info\n");
		return res;
	}

	res = tracker_find_by_dev_id(orig_dev_id, &tracker);
	if (res) {
		pr_err("Unable to create snapshot image: cannot find tracker for device [%d:%d]\n",
		       MAJOR(orig_dev_id), MINOR(orig_dev_id));
		return res;
	}

	image = kzalloc(sizeof(struct snapimage), GFP_KERNEL);
	if (image == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&image->link);

	
	spin_lock(&snapimage_minors_lock);
	minor = bitmap_find_free_region(snapimage_minors, SNAPIMAGE_MAX_DEVICES, 0);
	spin_unlock(&snapimage_minors_lock);

	if (minor < 0) {
		pr_err("Failed to allocate minor for snapshot image device. errno=%d\n",
		       minor);
		goto fail;
	}

	image->rq_processor = NULL;
	image->capacity = original_dev_info.count_sect;

		image->snapdev = defer_io_get_resource(tracker->snapdev);
		image->cbt_map = cbt_map_get_resource(tracker->cbt_map);
		image->orig_dev_id = orig_dev_id;

		image->image_dev = MKDEV(snapimage_major, minor);
		pr_info("Snapshot image device id [%d:%d]\n", MAJOR(image->image_dev),
			MINOR(image->image_dev));

		atomic_set(&image->own_cnt, 0);

		mutex_init(&image->open_locker);
		image->open_bdev = NULL;
		image->open_cnt = 0;

		image->queue = blk_alloc_queue(NUMA_NO_NODE);
		if (image->queue == NULL) {
			res = -ENOMEM;
			break;
		}
		image->queue->queuedata = image;

		blk_queue_max_segment_size(image->queue, 1024 * PAGE_SIZE);

	/**
	 * The actual access to the original device is performed in whole
	 * chunks, so the size of the blocks does not depend on anything.
	 */
	blk_queue_physical_block_size(image->queue, 512);
	blk_queue_logical_block_size(image->queue, 512);

		disk = alloc_disk(1); //only one partition on disk
		if (disk == NULL) {
			pr_err("Failed to allocate disk for snapshot image device\n");
			res = -ENOMEM;
			break;
		}
		image->disk = disk;

		if (snprintf(disk->disk_name, DISK_NAME_LEN, "%s%d", SNAP_IMAGE_NAME, minor) < 0) {
			pr_err("Unable to set disk name for snapshot image device: invalid minor %d\n",
			       minor);
			res = -EINVAL;
			break;
		}

		pr_info("Snapshot image disk name [%s]", disk->disk_name);

		disk->flags |= GENHD_FL_NO_PART_SCAN;
		disk->flags |= GENHD_FL_REMOVABLE;

		disk->major = snapimage_major;
		disk->minors = 1; // one disk have only one partition.
		disk->first_minor = minor;

		disk->private_data = image;

		disk->fops = &snapimage_ops;
		disk->queue = image->queue;

		set_capacity(disk, image->capacity);
		pr_info("Snapshot image device capacity %lld bytes",
			(u64)from_sectors(image->capacity));


		add_disk(image->disk);

	

	
	down_write(&snap_images_lock);
	list_add_tail(&image->link, &snap_images);
	up_write(&snap_images_lock);
	return 0;
fail:
	_snapimage_destroy(image);
	_snapimage_free(image);

	kfree(image);
	image = NULL;
	
	return res;
}

int snapimage_create_for(dev_t *p_dev, int count)
{
	int res = 0;
	int inx = 0;

	for (; inx < count; ++inx) {
		res = _snapimage_create(p_dev[inx]);
		if (res) {
			pr_err("Failed to create snapshot image for original device [%d:%d]\n",
			       MAJOR(p_dev[inx]), MINOR(p_dev[inx]));
			break;
		}
	}
	if (res)
		if (inx > 0)
			_snapimage_destroy_for(p_dev, inx - 1);
	return res;
}

int snapimage_init(void)
{
	int res = 0;

	res = register_blkdev(snapimage_major, SNAP_IMAGE_NAME);
	if (res >= 0) {
		snapimage_major = res;
		pr_info("Snapshot image block device major %d was registered\n", snapimage_major);
		res = 0;

		spin_lock(&snapimage_minors_lock);
		snapimage_minors = bitmap_zalloc(SNAPIMAGE_MAX_DEVICES, GFP_KERNEL);
		spin_unlock(&snapimage_minors_lock);

		if (snapimage_minors == NULL)
			pr_err("Failed to initialize bitmap of minors\n");
	} else
		pr_err("Failed to register snapshot image block device. errno=%d\n", res);

	return res;
}

void snapimage_done(void)
{
	down_write(&snap_image_destroy_lock);
	while (true) {
		struct snapimage *image = NULL;

		down_write(&snap_images_lock);
		if (!list_empty(&snap_images)) {
			image = list_entry(snap_images.next, struct snapimage, link);

			list_del(&image->link);
		}
		up_write(&snap_images_lock);

		if (image == NULL)
			break;

		pr_err("Snapshot image for device was unexpectedly removed [%d:%d]\n",
		       MAJOR(image->orig_dev_id), MINOR(image->orig_dev_id));

		_snapimage_destroy(image);
		_snapimage_free(image);

		kfree(image);
		image = NULL;
	}

	spin_lock(&snapimage_minors_lock);
	bitmap_free(snapimage_minors);
	snapimage_minors = NULL;
	spin_unlock(&snapimage_minors_lock);

	if (!list_empty(&snap_images))
		pr_err("Failed to release snapshot images container\n");

	unregister_blkdev(snapimage_major, SNAP_IMAGE_NAME);
	pr_info("Snapshot image block device [%d] was unregistered\n", snapimage_major);

	up_write(&snap_image_destroy_lock);
}

#if 0
static int _snapimage_open(struct block_device *bdev, fmode_t mode)
{
	int res = 0;

	if (bdev->bd_disk == NULL) {
		pr_err("Unable to open snapshot image: bd_disk is NULL. Device [%d:%d]\n",
		       MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
		pr_err("Block device object %p\n", bdev);
		return -ENODEV;
	}

	down_read(&snap_image_destroy_lock);
	do {
		struct snapimage *image = bdev->bd_disk->private_data;

		if (image == NULL) {
			pr_err("Unable to open snapshot image: private data is not initialized. Block device object %p\n",
			       bdev);
			res = -ENODEV;
			break;
		}

		mutex_lock(&image->open_locker);
		{
			if (image->open_cnt == 0)
				image->open_bdev = bdev;

			image->open_cnt++;
		}
		mutex_unlock(&image->open_locker);
	} while (false);
	up_read(&snap_image_destroy_lock);
	return res;
}

static inline uint64_t do_div_inline(uint64_t division, uint32_t divisor)
{
	do_div(division, divisor);
	return division;
}

static int _snapimage_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	int res = 0;
	sector_t quotient;

	down_read(&snap_image_destroy_lock);
	do {
		struct snapimage *image = bdev->bd_disk->private_data;

		if (image == NULL) {
			pr_err("Unable to open snapshot image: private data is not initialized. Block device object %p\n",
			       bdev);
			res = -ENODEV;
			break;
		}

		pr_info("Getting geo for snapshot image device [%d:%d]\n", MAJOR(image->image_dev),
			MINOR(image->image_dev));

		geo->start = 0;
		if (image->capacity > 63) {
			geo->sectors = 63;
			quotient = do_div_inline(image->capacity + (63 - 1), 63);

			if (quotient > 255ULL) {
				geo->heads = 255;
				geo->cylinders =
					(unsigned short)do_div_inline(quotient + (255 - 1), 255);
			} else {
				geo->heads = (unsigned char)quotient;
				geo->cylinders = 1;
			}
		} else {
			geo->sectors = (unsigned char)image->capacity;
			geo->cylinders = 1;
			geo->heads = 1;
		}

		pr_info("Image device geo: capacity=%lld, heads=%d, cylinders=%d, sectors=%d\n",
			image->capacity, geo->heads, geo->cylinders, geo->sectors);
	} while (false);
	up_read(&snap_image_destroy_lock);

	return res;
}

static void _snapimage_close(struct gendisk *disk, fmode_t mode)
{
	if (disk->private_data != NULL) {
		down_read(&snap_image_destroy_lock);
		do {
			struct snapimage *image = disk->private_data;

			mutex_lock(&image->open_locker);
			{
				if (image->open_cnt > 0)
					image->open_cnt--;

				if (image->open_cnt == 0)
					image->open_bdev = NULL;
			}
			mutex_unlock(&image->open_locker);
		} while (false);
		up_read(&snap_image_destroy_lock);
	} else
		pr_err("Unable to close snapshot image: private data is not initialized\n");
}

static int _snapimage_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg)
{
	int res = -ENOTTY;

	down_read(&snap_image_destroy_lock);
	{
		struct snapimage *image = bdev->bd_disk->private_data;

		switch (cmd) {
			/*
			 * The only command we need to interpret is HDIO_GETGEO, since
			 * we can't partition the drive otherwise.  We have no real
			 * geometry, of course, so make something up.
			 */
		case HDIO_GETGEO: {
			unsigned long len;
			struct hd_geometry geo;

			res = _snapimage_getgeo(bdev, &geo);

			len = copy_to_user((void *)arg, &geo, sizeof(geo));
			if (len != 0)
				res = -EFAULT;
			else
				res = 0;
		} break;
		case CDROM_GET_CAPABILITY: //0x5331  / * get capabilities * /
		{
			struct gendisk *disk = bdev->bd_disk;

			if (bdev->bd_disk && (disk->flags & GENHD_FL_CD))
				res = 0;
			else
				res = -EINVAL;
		} break;

		default:
			pr_info("Snapshot image ioctl receive unsupported command\n");
			pr_info("Device [%d:%d], command 0x%x, arg 0x%lx\n",
				MAJOR(image->image_dev), MINOR(image->image_dev), cmd, arg);

			res = -ENOTTY; /* unknown command */
		}
	}
	up_read(&snap_image_destroy_lock);
	return res;
}


blk_qc_t _snapimage_submit_bio(struct bio *bio)
{
	blk_qc_t ret = 0;
#if defined(HAVE_BI_BDISK)
	struct gendisk *disk = bio->bi_disk;
#elif defined(HAVE_BI_BDEV)
	struct gendisk *disk = bio->bi_bdev->bd_disk;
#endif
	struct diff_area *diff_area;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct diff_area_image_context image_ctx;
	
	if (!disk->private_data) {
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	/* non-blocking requests are not supported */
	if (bio->bi_opf & REQ_NOWAIT) {
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	diff_area_image_context_init(&image_ctx, disk->private_data, op_is_write(bio_op(bio)));

	bio_for_each_segment(bvec, bio, iter) {
		blk_status_t status;
		
		status = diff_area_image_page(&image_ctx,
			                      bvec.bv_page, bvec.bv_offset,
			                      iter.bi_sector, bvec.bv_len);
		if (status) {
			bio->bi_status = status;
			break;
		}

	}
	diff_area_image_context_done(&image_ctx);

	//bio->bi_iter
	//bio->bi_io_vec
	//bio->bi_end_io

	bio_endio(bio);

	return BLK_QC_T_NONE;
}

const struct block_device_operations snapimage_ops = {
	.owner = THIS_MODULE,
	//.submit_bio = _snapimage_submit_bio,
	//.rw_page = _snapimage_rw_page,
	.open = _snapimage_open,
	.ioctl = _snapimage_ioctl,
	.release = _snapimage_close,
};

static inline int _snapimage_request_read(struct snapimage *image,
					  struct blk_redirect_bio *rq_redir)
{
	return snapstore_device_read(image->snapdev, rq_redir);
}

static int _snapimage_request_write(struct snapimage *image, struct blk_redirect_bio *rq_redir)
{
	struct snapstore_device *snapdev;
	struct cbt_map *cbt_map;
	int res = 0;

	if (unlikely((image->snapdev == NULL) || (image->cbt_map == NULL))) {
		pr_err("Invalid snapshot image structure");
		return -EINVAL;
	}


	snapdev = image->snapdev;
	cbt_map = image->cbt_map;

	if (snapstore_device_is_corrupted(snapdev))
		return -ENODATA;

	if (!bio_has_data(rq_redir->bio)) {
		pr_warn("Snapshot image receive empty block IO. flags=%u\n",
			rq_redir->bio->bi_flags);

		blk_redirect_complete(rq_redir, 0);
		return 0;
	}

	if (cbt_map != NULL) {
		sector_t ofs = rq_redir->bio->bi_iter.bi_sector;
		sector_t cnt = bio_sectors(rq_redir->bio);

		res = cbt_map_set_both(cbt_map, ofs, cnt);
		if (res != 0)
			pr_err("Unable to write data to snapshot image: failed to set CBT map. errno=%d\n",
			       res);
	}

	res = snapstore_device_write(snapdev, rq_redir);

	if (res != 0) {
		pr_err("Failed to write data to snapshot image\n");
		return res;
	}

	return res;
}
/*
static void _snapimage_processing(struct snapimage *image)
{
	int res = 0;
	struct blk_redirect_bio *rq_redir;

	rq_redir = redirect_bio_queue_get_first(&image->image_queue);

	if (bio_data_dir(rq_redir->bio) == READ) {
		res = _snapimage_request_read(image, rq_redir);
		if (res != 0)
			pr_err("Failed to read data from snapshot image. errno=%d\n", res);

	} else {
		res = _snapimage_request_write(image, rq_redir);
		if (res != 0)
			pr_err("Failed to write data to snapshot image. errno=%d\n", res);
	}

	if (res != 0)
		blk_redirect_complete(rq_redir, res);
}
*/
/*
static int _snapimage_processor_waiting(struct snapimage *image)
{
	int res = 0;

	if (redirect_bio_queue_empty(image->image_queue)) {
		res = wait_event_interruptible_timeout(
			image->rq_proc_event,
			(!redirect_bio_queue_empty(image->image_queue) || kthread_should_stop()),
			5 * HZ);
		if (res > 0)
			res = 0;
		else if (!res)
			res = -ETIME;
	}
	return res;
}*/
/*
static int _snapimage_processor_thread(void *data)
{
	struct snapimage *image = data;

	pr_info("Snapshot image thread for device [%d:%d] start\n", MAJOR(image->image_dev),
		MINOR(image->image_dev));

	add_disk(image->disk);

	//priority
	set_user_nice(current, -20); //MIN_NICE

	while (!kthread_should_stop()) {
		int res = _snapimage_processor_waiting(image);

		if (!res) {
			if (!redirect_bio_queue_empty(image->image_queue))
				_snapimage_processing(image);
		} else if (res != -ETIME) {
			pr_err("Failed to wait snapshot image thread queue. errno=%d\n", res);
			return res;
		}
		schedule();
	}
	pr_info("Snapshot image disk delete\n");
	del_gendisk(image->disk);

	while (!redirect_bio_queue_empty(image->image_queue))
		_snapimage_processing(image);

	pr_info("Snapshot image thread for device [%d:%d] complete", MAJOR(image->image_dev),
		MINOR(image->image_dev));
	return 0;
}

static inline void _snapimage_bio_complete(struct bio *bio, int err)
{
	if (err)
		bio->bi_status = BLK_STS_IOERR;
	else
		bio->bi_status = BLK_STS_OK;

	bio_endio(bio);
}

static void _snapimage_bio_complete_cb(void *complete_param, struct bio *bio, int err)
{
	struct snapimage *image = (struct snapimage *)complete_param;

	_snapimage_bio_complete(bio, err);

	if (redirect_bio_queue_unactive(image->image_queue))
		wake_up_interruptible(&image->rq_complete_event);

	atomic_dec(&image->own_cnt);
}


blk_qc_t _snapimage_submit_bio(struct bio *bio)
{
	blk_qc_t result = 0;
	struct request_queue *q = bio->bi_disk->queue;
	struct snapimage *image = q->queuedata;

	if (unlikely(blk_mq_queue_stopped(q))) {
		pr_info("Failed to make snapshot image request. Queue already is not active.");
		pr_info("Queue flags=%lx\n", q->queue_flags);

		_snapimage_bio_complete(bio, -ENODEV);

		return result;
	}

	atomic_inc(&image->own_cnt);
	do {
		int res;
		struct blk_redirect_bio *rq_redir;

		if (false == atomic_read(&(image->image_queue.active_state))) {
			_snapimage_bio_complete(bio, -ENODEV);
			break;
		}

		if (snapstore_device_is_corrupted(image->snapdev)) {
			_snapimage_bio_complete(bio, -ENODATA);
			break;
		}


		res = _snapimage_throttling(image->defer_io);
		if (res != 0) {
			pr_err("Failed to throttle snapshot image device. errno=%d\n", res);
			_snapimage_bio_complete(bio, res);
			break;
		}

		rq_redir = redirect_bio_queue_new(&image->image_queue);
		if (rq_redir == NULL) {
			pr_err("Unable to make snapshot image request: failed to allocate redirect bio structure\n");
			_snapimage_bio_complete(bio, -ENOMEM);
			break;
		}
		rq_redir->bio = bio;
		rq_redir->complete_cb = _snapimage_bio_complete_cb;
		rq_redir->complete_param = (void *)image;
		atomic_inc(&image->own_cnt);

		res = redirect_bio_queue_push_back(&image->image_queue, rq_redir);
		if (!res)
			wake_up(&image->rq_proc_event);
		else {
			redirect_bio_queue_free(rq_redir);
			_snapimage_bio_complete(bio, -EIO);

			if (redirect_bio_queue_unactive(image->image_queue))
				wake_up_interruptible(&image->rq_complete_event);
		}

	} while (false);
	atomic_dec(&image->own_cnt);

	return result;
}
*/

struct blk_dev_info {
	size_t blk_size;
	sector_t start_sect;
	sector_t count_sect;

	unsigned int io_min;
	unsigned int physical_block_size;
	unsigned short logical_block_size;
};

static int _blk_dev_get_info(struct block_device *blk_dev, struct blk_dev_info *pdev_info)
{
	sector_t SectorStart;
	sector_t SectorsCapacity;

	if (blk_dev->bd_part)
		SectorsCapacity = blk_dev->bd_part->nr_sects;
	else if (blk_dev->bd_disk)
		SectorsCapacity = get_capacity(blk_dev->bd_disk);
	else
		return -EINVAL;

	SectorStart = get_start_sect(blk_dev);

	pdev_info->physical_block_size = blk_dev->bd_disk->queue->limits.physical_block_size;
	pdev_info->logical_block_size = blk_dev->bd_disk->queue->limits.logical_block_size;
	pdev_info->io_min = blk_dev->bd_disk->queue->limits.io_min;

	pdev_info->blk_size = block_size(blk_dev);
	pdev_info->start_sect = SectorStart;
	pdev_info->count_sect = SectorsCapacity;
	return 0;
}

static int blk_dev_get_info(dev_t dev_id, struct blk_dev_info *pdev_info)
{
	int result = 0;
	struct block_device *blk_dev;

	result = blk_dev_open(dev_id, &blk_dev);
	if (result != 0) {
		pr_err("Failed to open device [%d:%d]\n", MAJOR(dev_id), MINOR(dev_id));
		return result;
	}

	result = _blk_dev_get_info(blk_dev, pdev_info);
	if (result != 0)
		pr_err("Failed to identify block device [%d:%d]\n", MAJOR(dev_id), MINOR(dev_id));

	blk_dev_close(blk_dev);

	return result;
}

static inline void _snapimage_free(struct snapimage *image)
{
	snapstore_device_put_resource(image->snapdev);
	image->snapdev = NULL;

	cbt_map_put_resource(image->cbt_map);
	image->cbt_map = NULL;
}

static void _snapimage_stop(struct snapimage *image)
{
	struct request_queue *q = image->queue;

	/*
	if (image->rq_processor != NULL) {
		if (redirect_bio_queue_active(&image->image_queue, false)) {
			struct request_queue *q = image->queue;

			pr_info("Snapshot image request processing stop\n");

			if (!blk_queue_stopped(q)) {
				blk_sync_queue(q);
				blk_mq_stop_hw_queues(q);
			}
		}

		pr_info("Snapshot image thread stop\n");
		kthread_stop(image->rq_processor);
		image->rq_processor = NULL;

		while (!redirect_bio_queue_unactive(image->image_queue))
			wait_event_interruptible(image->rq_complete_event,
						 redirect_bio_queue_unactive(image->image_queue));
	}
	*/
	pr_info("Snapshot image request processing stop\n");
	blk_mq_freeze_queue(q);
	blk_mq_quiesce_queue(q);

	pr_info("Snapshot image disk delete\n");
	del_gendisk(image->disk);
}

static void _snapimage_destroy(struct snapimage *image)
{
	if (image->rq_processor != NULL)
		_snapimage_stop(image);

	if (image->queue) {
		pr_info("Snapshot image queue cleanup\n");
		blk_cleanup_queue(image->queue);
		image->queue = NULL;
	}

	if (image->disk != NULL) {
		struct gendisk *disk;

		disk = image->disk;
		image->disk = NULL;

		pr_info("Snapshot image disk structure release\n");

		disk->private_data = NULL;
		put_disk(disk);
	}

	spin_lock(&snapimage_minors_lock);
	bitmap_clear(snapimage_minors, MINOR(image->image_dev), 1u);
	spin_unlock(&snapimage_minors_lock);
}

static int _snapimage_create(dev_t orig_dev_id)
{
	int res = 0;
	struct snapimage *image = NULL;
	struct gendisk *disk = NULL;
	struct diff_area *diff_area = NULL;
	int minor;
	struct blk_dev_info original_dev_info;

	pr_info("Create snapshot image for device [%d:%d]\n", MAJOR(orig_dev_id),
		MINOR(orig_dev_id));

	res = blk_dev_get_info(orig_dev_id, &original_dev_info);
	if (res != 0) {
		pr_err("Failed to obtain original device info\n");
		return res;
	}

	res = tracker_find_by_dev_id(orig_dev_id, &tracker);
	if (res != 0) {
		pr_err("Unable to create snapshot image: cannot find tracker for device [%d:%d]\n",
		       MAJOR(orig_dev_id), MINOR(orig_dev_id));
		return res;
	}

	image = kzalloc(sizeof(struct snapimage), GFP_KERNEL);
	if (image == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&image->link);

	do {
		spin_lock(&snapimage_minors_lock);
		minor = bitmap_find_free_region(snapimage_minors, SNAPIMAGE_MAX_DEVICES, 0);
		spin_unlock(&snapimage_minors_lock);

		if (minor < 0) {
			pr_err("Failed to allocate minor for snapshot image device. errno=%d\n",
			       0 - minor);
			break;
		}

		image->rq_processor = NULL;

		image->capacity = original_dev_info.count_sect;

		image->snapdev = defer_io_get_resource(tracker->snapdev);
		image->cbt_map = cbt_map_get_resource(tracker->cbt_map);
		image->orig_dev_id = orig_dev_id;

		image->image_dev = MKDEV(snapimage_major, minor);
		pr_info("Snapshot image device id [%d:%d]\n", MAJOR(image->image_dev),
			MINOR(image->image_dev));

		atomic_set(&image->own_cnt, 0);

		mutex_init(&image->open_locker);
		image->open_bdev = NULL;
		image->open_cnt = 0;

		image->queue = blk_alloc_queue(NUMA_NO_NODE);
		if (image->queue == NULL) {
			res = -ENOMEM;
			break;
		}
		image->queue->queuedata = image;

		blk_queue_max_segment_size(image->queue, 1024 * PAGE_SIZE);

		{
			unsigned int physical_block_size = original_dev_info.physical_block_size;
			unsigned short logical_block_size = original_dev_info.logical_block_size;

			pr_info("Snapshot image physical block size %d\n", physical_block_size);
			pr_info("Snapshot image logical block size %d\n", logical_block_size);

			blk_queue_physical_block_size(image->queue, physical_block_size);
			blk_queue_logical_block_size(image->queue, logical_block_size);
		}
		disk = alloc_disk(1); //only one partition on disk
		if (disk == NULL) {
			pr_err("Failed to allocate disk for snapshot image device\n");
			res = -ENOMEM;
			break;
		}
		image->disk = disk;

		if (snprintf(disk->disk_name, DISK_NAME_LEN, "%s%d", SNAP_IMAGE_NAME, minor) < 0) {
			pr_err("Unable to set disk name for snapshot image device: invalid minor %d\n",
			       minor);
			res = -EINVAL;
			break;
		}

		pr_info("Snapshot image disk name [%s]", disk->disk_name);

		disk->flags |= GENHD_FL_NO_PART_SCAN;
		disk->flags |= GENHD_FL_REMOVABLE;

		disk->major = snapimage_major;
		disk->minors = 1; // one disk have only one partition.
		disk->first_minor = minor;

		disk->private_data = image;

		disk->fops = &snapimage_ops;
		disk->queue = image->queue;

		set_capacity(disk, image->capacity);
		pr_info("Snapshot image device capacity %lld bytes",
			(u64)from_sectors(image->capacity));


		add_disk(image->disk);

	} while (false);

	if (!res) {
		down_write(&snap_images_lock);
		list_add_tail(&image->link, &snap_images);
		up_write(&snap_images_lock);
	} else {
		_snapimage_destroy(image);
		_snapimage_free(image);

		kfree(image);
		image = NULL;
	}
	return res;
}

static struct snapimage *snapimage_find(dev_t orig_dev_id)
{
	struct snapimage *image = NULL;

	down_read(&snap_images_lock);
	if (!list_empty(&snap_images)) {
		struct list_head *_list_head;

		list_for_each(_list_head, &snap_images) {
			struct snapimage *_image = list_entry(_list_head, struct snapimage, link);

			if (_image->orig_dev_id == orig_dev_id) {
				image = _image;
				break;
			}
		}
	}
	up_read(&snap_images_lock);

	return image;
}

void snapimage_stop(dev_t orig_dev_id)
{
	struct snapimage *image;

	pr_info("Snapshot image processing stop for original device [%d:%d]\n", MAJOR(orig_dev_id),
		MINOR(orig_dev_id));

	down_read(&snap_image_destroy_lock);

	image = snapimage_find(orig_dev_id);
	if (image != NULL)
		_snapimage_stop(image);
	else
		pr_err("Snapshot image [%d:%d] not found\n", MAJOR(orig_dev_id),
		       MINOR(orig_dev_id));

	up_read(&snap_image_destroy_lock);
}

void snapimage_destroy(dev_t orig_dev_id)
{
	struct snapimage *image = NULL;

	pr_info("Destroy snapshot image for device [%d:%d]\n", MAJOR(orig_dev_id),
		MINOR(orig_dev_id));

	down_write(&snap_images_lock);
	if (!list_empty(&snap_images)) {
		struct list_head *_list_head;

		list_for_each(_list_head, &snap_images) {
			struct snapimage *_image = list_entry(_list_head, struct snapimage, link);

			if (_image->orig_dev_id == orig_dev_id) {
				image = _image;
				list_del(&image->link);
				break;
			}
		}
	}
	up_write(&snap_images_lock);

	if (image != NULL) {
		down_write(&snap_image_destroy_lock);

		_snapimage_destroy(image);
		_snapimage_free(image);

		kfree(image);
		image = NULL;

		up_write(&snap_image_destroy_lock);
	} else
		pr_err("Snapshot image [%d:%d] not found\n", MAJOR(orig_dev_id),
		       MINOR(orig_dev_id));
}

static void _snapimage_destroy_for(dev_t *p_dev, int count)
{
	int inx = 0;

	for (; inx < count; ++inx)
		snapimage_destroy(p_dev[inx]);
}

int snapimage_create_for(dev_t *p_dev, int count)
{
	int res = 0;
	int inx = 0;

	for (; inx < count; ++inx) {
		res = _snapimage_create(p_dev[inx]);
		if (res != 0) {
			pr_err("Failed to create snapshot image for original device [%d:%d]\n",
			       MAJOR(p_dev[inx]), MINOR(p_dev[inx]));
			break;
		}
	}
	if (res != 0)
		if (inx > 0)
			_snapimage_destroy_for(p_dev, inx - 1);
	return res;
}

int snapimage_init(void)
{
	int res = 0;

	res = register_blkdev(snapimage_major, SNAP_IMAGE_NAME);
	if (res >= 0) {
		snapimage_major = res;
		pr_info("Snapshot image block device major %d was registered\n", snapimage_major);
		res = 0;

		spin_lock(&snapimage_minors_lock);
		snapimage_minors = bitmap_zalloc(SNAPIMAGE_MAX_DEVICES, GFP_KERNEL);
		spin_unlock(&snapimage_minors_lock);

		if (snapimage_minors == NULL)
			pr_err("Failed to initialize bitmap of minors\n");
	} else
		pr_err("Failed to register snapshot image block device. errno=%d\n", res);

	return res;
}

void snapimage_done(void)
{
	down_write(&snap_image_destroy_lock);
	while (true) {
		struct snapimage *image = NULL;

		down_write(&snap_images_lock);
		if (!list_empty(&snap_images)) {
			image = list_entry(snap_images.next, struct snapimage, link);

			list_del(&image->link);
		}
		up_write(&snap_images_lock);

		if (image == NULL)
			break;

		pr_err("Snapshot image for device was unexpectedly removed [%d:%d]\n",
		       MAJOR(image->orig_dev_id), MINOR(image->orig_dev_id));

		_snapimage_destroy(image);
		_snapimage_free(image);

		kfree(image);
		image = NULL;
	}

	spin_lock(&snapimage_minors_lock);
	bitmap_free(snapimage_minors);
	snapimage_minors = NULL;
	spin_unlock(&snapimage_minors_lock);

	if (!list_empty(&snap_images))
		pr_err("Failed to release snapshot images container\n");

	unregister_blkdev(snapimage_major, SNAP_IMAGE_NAME);
	pr_info("Snapshot image block device [%d] was unregistered\n", snapimage_major);

	up_write(&snap_image_destroy_lock);
}

int snapimage_collect_images(int count, struct image_info_s *p_user_image_info, int *p_real_count)
{
	int res = 0;
	int real_count = 0;

	down_read(&snap_images_lock);
	if (!list_empty(&snap_images)) {
		struct list_head *_list_head;

		list_for_each(_list_head, &snap_images)
			real_count++;
	}
	up_read(&snap_images_lock);
	*p_real_count = real_count;

	if (count < real_count)
		res = -ENODATA;

	real_count = min(count, real_count);
	if (real_count > 0) {
		unsigned long len;
		struct image_info_s *p_kernel_image_info = NULL;
		size_t buff_size;

		buff_size = sizeof(struct image_info_s) * real_count;
		p_kernel_image_info = kzalloc(buff_size, GFP_KERNEL);
		if (p_kernel_image_info == NULL) {
			pr_err("Unable to collect snapshot images: not enough memory. size=%zu\n",
			       buff_size);
			return res = -ENOMEM;
		}

		down_read(&snap_image_destroy_lock);
		down_read(&snap_images_lock);

		if (!list_empty(&snap_images)) {
			size_t inx = 0;
			struct list_head *_list_head;

			list_for_each(_list_head, &snap_images) {
				struct snapimage *img =
					list_entry(_list_head, struct snapimage, link);

				real_count++;

				p_kernel_image_info[inx].original_dev_id.major =
					MAJOR(img->orig_dev_id);
				p_kernel_image_info[inx].original_dev_id.minor =
					MINOR(img->orig_dev_id);

				p_kernel_image_info[inx].snapshot_dev_id.major =
					MAJOR(img->image_dev);
				p_kernel_image_info[inx].snapshot_dev_id.minor =
					MINOR(img->image_dev);

				++inx;
				if (inx > real_count)
					break;
			}
		}

		up_read(&snap_images_lock);
		up_read(&snap_image_destroy_lock);

		len = copy_to_user(p_user_image_info, p_kernel_image_info, buff_size);
		if (len != 0) {
			pr_err("Unable to collect snapshot images: failed to copy data to user buffer\n");
			res = -ENODATA;
		}

		kfree(p_kernel_image_info);
	}

	return res;
}

int snapimage_mark_dirty_blocks(dev_t image_dev_id, struct block_range_s *block_ranges,
				unsigned int count)
{
	size_t inx = 0;
	int res = 0;

	pr_info("Marking [%d] dirty blocks for image device [%d:%d]\n", count, MAJOR(image_dev_id),
		MINOR(image_dev_id));

	down_read(&snap_image_destroy_lock);
	do {
		struct snapimage *image = snapimage_find(image_dev_id);

		if (image == NULL) {
			pr_err("Cannot find device [%d:%d]\n", MAJOR(image_dev_id),
			       MINOR(image_dev_id));
			res = -ENODEV;
			break;
		}

		for (inx = 0; inx < count; ++inx) {
			sector_t ofs = (sector_t)block_ranges[inx].ofs;
			sector_t cnt = (sector_t)block_ranges[inx].cnt;

			res = cbt_map_set_both(image->cbt_map, ofs, cnt);
			if (res != 0) {
				pr_err("Failed to set CBT table. errno=%d\n", res);
				break;
			}
		}
	} while (false);
	up_read(&snap_image_destroy_lock);

	return res;
}

#endif
