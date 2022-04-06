// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME "-tracker: " fmt
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/sched/mm.h>
#include <linux/blk_snap.h>
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
#include "memory_checker.h"
#endif
#include "params.h"
#include "tracker.h"
#include "cbt_map.h"
#include "diff_area.h"

struct tracked_device {
	struct list_head link;
	dev_t dev_id;
};

LIST_HEAD(tracked_device_list);
DEFINE_SPINLOCK(tracked_device_lock);
static refcount_t trackers_counter = REFCOUNT_INIT(1);

struct tracker_release_worker {
	struct work_struct work;
	struct list_head list;
	spinlock_t lock;
};
static struct tracker_release_worker tracker_release_worker;

void tracker_free(struct tracker *tracker)
{
	might_sleep();

	pr_debug("Free tracker for device [%u:%u].\n", MAJOR(tracker->dev_id),
		 MINOR(tracker->dev_id));

	diff_area_put(tracker->diff_area);
	cbt_map_put(tracker->cbt_map);

	percpu_free_rwsem(&tracker->submit_lock);
	kfree(tracker);
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
	memory_object_dec(memory_object_tracker);
#endif
	refcount_dec(&trackers_counter);
}

static inline void tracker_get(struct tracker *tracker)
{
	bdev_filter_get(&tracker->flt);
}

struct tracker *tracker_get_by_dev(struct block_device *bdev)
{
	struct bdev_filter *flt;

	flt = bdev_filter_get_by_altitude(bdev, bdev_filter_alt_blksnap);
	if (likely(flt))
		return container_of(flt, struct tracker, flt);

	return NULL;
}

static enum bdev_filter_result tracker_submit_bio_cb(struct bio *bio,
		struct bdev_filter *flt)
{
	enum bdev_filter_result ret = bdev_filter_pass;
	struct tracker *tracker = container_of(flt, struct tracker, flt);
	int err;
	sector_t sector;
	sector_t count;
	unsigned int current_flag;

	if (bio->bi_opf & REQ_NOWAIT) {
		if (!percpu_down_read_trylock(&tracker->submit_lock)) {
			bio_wouldblock_error(bio);
			return bdev_filter_skip;
		}
	} else
		percpu_down_read(&tracker->submit_lock);

	if (!op_is_write(bio_op(bio)))
		goto out;

	if (!bio->bi_iter.bi_size)
		goto out;

	sector = bio->bi_iter.bi_sector;
	count = (sector_t)(round_up(bio->bi_iter.bi_size, SECTOR_SIZE) /
			   SECTOR_SIZE);

	current_flag = memalloc_noio_save();
	err = cbt_map_set(tracker->cbt_map, sector, count);
	memalloc_noio_restore(current_flag);
	if (unlikely(err))
		goto out;

	if (!atomic_read(&tracker->snapshot_is_taken))
		goto out;

	if (diff_area_is_corrupted(tracker->diff_area))
		goto out;

	current_flag = memalloc_noio_save();
	err = diff_area_copy(tracker->diff_area, sector, count,
			     !!(bio->bi_opf & REQ_NOWAIT));
	memalloc_noio_restore(current_flag);

	if (unlikely(err)) {
		if (err == -EAGAIN) {
			bio_wouldblock_error(bio);
			ret = bdev_filter_skip;
		} else
			pr_err("Failed to copy data to diff storage with error %d\n", abs(err));

		goto out;
	}

	/*
	 * If a new bio was created during the handling and the original bio
	 * must be processed synchronously (flag REQ_SYNC), then new bios must
	 * be sent and returned to complete the processing of the original bio.
	 */
	if (!bio_list_empty(current->bio_list) && (bio->bi_opf & REQ_SYNC))
		ret = bdev_filter_repeat;
out:
	percpu_up_read(&tracker->submit_lock);
	return ret;
}


static void tracker_release_work(struct work_struct *work)
{
	struct tracker *tracker = NULL;
	struct tracker_release_worker *tracker_release =
		container_of(work, struct tracker_release_worker, work);

	do {
		spin_lock(&tracker_release->lock);
		tracker = list_first_entry_or_null(&tracker_release->list,
						   struct tracker, link);
		if (tracker)
			list_del(&tracker->link);
		spin_unlock(&tracker_release->lock);

		if (tracker)
			tracker_free(tracker);
	} while(tracker);
}

static void tracker_detach_cb(struct kref *kref)
{
	struct bdev_filter *flt = container_of(kref, struct bdev_filter, kref);
	struct tracker *tracker = container_of(flt, struct tracker, flt);

	spin_lock(&tracker_release_worker.lock);
	list_add_tail(&tracker->link, &tracker_release_worker.list);
	spin_unlock(&tracker_release_worker.lock);

	queue_work(system_wq, &tracker_release_worker.work);
}

static const struct bdev_filter_operations tracker_fops = {
	.submit_bio_cb = tracker_submit_bio_cb,
	.detach_cb = tracker_detach_cb
};

static int tracker_filter_attach(struct block_device *bdev,
				 struct tracker *tracker)
{
	int ret;
	bool is_frozen = false;

	pr_debug("Tracker attach filter\n");

	if (freeze_bdev(bdev))
		pr_err("Failed to freeze device [%u:%u]\n", MAJOR(bdev->bd_dev),
		       MINOR(bdev->bd_dev));
	else {
		is_frozen = true;
		pr_debug("Device [%u:%u] was frozen\n", MAJOR(bdev->bd_dev),
			 MINOR(bdev->bd_dev));
	}

	ret = bdev_filter_attach(bdev, KBUILD_MODNAME, bdev_filter_alt_blksnap,
				 &tracker->flt);
	if (is_frozen) {
		if (thaw_bdev(bdev))
			pr_err("Failed to thaw device [%u:%u]\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
		else
			pr_debug("Device [%u:%u] was unfrozen\n",
				 MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
	}

	if (ret) {
		pr_err("Failed to attach tracker to device [%u:%u]\n",
		       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
		tracker_put(tracker);
	}

	return ret;
}

static int tracker_filter_detach(struct block_device *bdev)
{
	int ret;
	bool is_frozen = false;

	pr_debug("Tracker delete filter\n");
	if (freeze_bdev(bdev))
		pr_err("Failed to freeze device [%u:%u]\n", MAJOR(bdev->bd_dev),
		       MINOR(bdev->bd_dev));
	else {
		is_frozen = true;
		pr_debug("Device [%u:%u] was frozen\n", MAJOR(bdev->bd_dev),
			 MINOR(bdev->bd_dev));
	}

	ret = bdev_filter_detach(bdev, KBUILD_MODNAME, bdev_filter_alt_blksnap);

	if (is_frozen) {
		if (thaw_bdev(bdev))
			pr_err("Failed to thaw device [%u:%u]\n",
			       MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
		else
			pr_debug("Device [%u:%u] was unfrozen\n",
				 MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
	}

	if (ret)
		pr_err("Failed to detach filter from device [%u:%u]\n",
		       MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
	return ret;
}

static struct tracker *tracker_new(struct block_device *bdev)
{
	int ret;
	struct tracker *tracker = NULL;
	struct cbt_map *cbt_map;

	pr_debug("Creating tracker for device [%u:%u].\n", MAJOR(bdev->bd_dev),
		 MINOR(bdev->bd_dev));

	tracker = kzalloc(sizeof(struct tracker), GFP_KERNEL);
	if (tracker == NULL)
		return ERR_PTR(-ENOMEM);
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
	memory_object_inc(memory_object_tracker);
#endif
	refcount_inc(&trackers_counter);
	bdev_filter_init(&tracker->flt, &tracker_fops);
	INIT_LIST_HEAD(&tracker->link);
	percpu_init_rwsem(&tracker->submit_lock);
	atomic_set(&tracker->snapshot_is_taken, false);
	tracker->dev_id = bdev->bd_dev;

	pr_info("Create tracker for device [%u:%u]. Capacity 0x%llx sectors\n",
		MAJOR(tracker->dev_id), MINOR(tracker->dev_id),
		(unsigned long long)bdev_nr_sectors(bdev));

	cbt_map = cbt_map_create(bdev);
	if (!cbt_map) {
		pr_err("Failed to create tracker for device [%u:%u]\n",
		       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
		ret = -ENOMEM;
		goto fail;
	}
	tracker->cbt_map = cbt_map;

	ret = tracker_filter_attach(bdev, tracker);
	if (ret) {
		pr_err("Failed to attach tracker. errno=%d\n", abs(ret));
		goto fail;
	}
	/*
	 * The filter stores a pointer to the tracker.
	 * The tracker will not be released until its filter is released.
	 */

	pr_debug("New tracker for device [%u:%u] was created.\n",
		 MAJOR(tracker->dev_id), MINOR(tracker->dev_id));

	return tracker;
fail:
	tracker_put(tracker);
	return ERR_PTR(ret);
}

int tracker_take_snapshot(struct tracker *tracker)
{
	int ret = 0;
	bool cbt_reset_needed = false;
	sector_t capacity;

	if (tracker->cbt_map->is_corrupted) {
		cbt_reset_needed = true;
		pr_warn("Corrupted CBT table detected. CBT fault\n");
	}

	capacity = bdev_nr_sectors(tracker->diff_area->orig_bdev);
	if (tracker->cbt_map->device_capacity != capacity) {
		cbt_reset_needed = true;
		pr_warn("Device resize detected. CBT fault\n");
	}

	if (cbt_reset_needed) {
		ret = cbt_map_reset(tracker->cbt_map, capacity);
		if (ret) {
			pr_err("Failed to create tracker. errno=%d\n",
			       abs(ret));
			return ret;
		}
	}

	cbt_map_switch(tracker->cbt_map);
	atomic_set(&tracker->snapshot_is_taken, true);

	return 0;
}

void tracker_release_snapshot(struct tracker *tracker)
{
	if (!tracker)
		return;

	pr_debug("Tracker for device [%u:%u] release snapshot\n",
		 MAJOR(tracker->dev_id), MINOR(tracker->dev_id));

	atomic_set(&tracker->snapshot_is_taken, false);
}

int tracker_init(void)
{
	INIT_WORK(&tracker_release_worker.work, tracker_release_work);
	INIT_LIST_HEAD(&tracker_release_worker.list);
	spin_lock_init(&tracker_release_worker.lock);

	return 0;
}

/**
 * tracker_wait_for_release - Waiting for all trackers are released.
 *
 */
static void tracker_wait_for_release(void)
{
	long inx = 0;
	u64 start_waiting = jiffies_64;

	while (refcount_read(&trackers_counter) > 1) {
		schedule_timeout_interruptible(HZ);
		if (jiffies_64 > (start_waiting + 10*HZ)) {
			start_waiting = jiffies_64;
			inx++;

			if (inx <= 12)
				pr_warn("Waiting for trackers release\n");

			WARN_ONCE(inx > 12, "Failed to release trackers\n");
		}
	}
}

void tracker_done(void)
{
	struct tracked_device *tr_dev;

	pr_debug("Cleanup trackers\n");
	while (true) {
		spin_lock(&tracked_device_lock);
		tr_dev = list_first_entry_or_null(&tracked_device_list,
						  struct tracked_device, link);
		if (tr_dev)
			list_del(&tr_dev->link);
		spin_unlock(&tracked_device_lock);

		if (!tr_dev)
			break;

		tracker_remove(tr_dev->dev_id);
		kfree(tr_dev);
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
		memory_object_dec(memory_object_tracked_device);
#endif
	}

	tracker_wait_for_release();
}

struct tracker *tracker_create_or_get(dev_t dev_id)
{
	struct tracker *tracker;
	struct block_device *bdev;
	struct tracked_device *tr_dev;

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Cannot open device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		return ERR_PTR(PTR_ERR(bdev));
	}

	tracker = tracker_get_by_dev(bdev);
	if (tracker) {
		pr_debug("Device [%u:%u] is already under tracking\n",
			 MAJOR(dev_id), MINOR(dev_id));
		goto put_bdev;
	}

	tr_dev = kzalloc(sizeof(struct tracked_device), GFP_KERNEL);
	if (!tr_dev) {
		tracker = ERR_PTR(-ENOMEM);
		goto put_bdev;
	}
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
	memory_object_inc(memory_object_tracked_device);
#endif
	INIT_LIST_HEAD(&tr_dev->link);
	tr_dev->dev_id = dev_id;

	tracker = tracker_new(bdev);
	if (IS_ERR(tracker)) {
		pr_err("Failed to create tracker. errno=%d\n",
		       abs((int)PTR_ERR(tracker)));
		kfree(tr_dev);
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
		memory_object_dec(memory_object_tracked_device);
#endif
	} else {
		tracker_get(tracker);
		spin_lock(&tracked_device_lock);
		list_add_tail(&tr_dev->link, &tracked_device_list);
		spin_unlock(&tracked_device_lock);
	}
put_bdev:
	blkdev_put(bdev, 0);
	return tracker;
}

int tracker_remove(dev_t dev_id)
{
	int ret;
	struct tracker *tracker;
	struct block_device *bdev;

	pr_info("Removing device [%u:%u] from tracking\n", MAJOR(dev_id),
		MINOR(dev_id));

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Cannot open device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		return PTR_ERR(bdev);
	}

	tracker = tracker_get_by_dev(bdev);
	if (!tracker) {
		pr_err("Unable to remove device [%u:%u] from tracking: ",
		       MAJOR(dev_id), MINOR(dev_id));
		pr_err("tracker not found\n");
		ret = -ENODATA;
		goto put_bdev;
	}

	if (atomic_read(&tracker->snapshot_is_taken)) {
		pr_err("Tracker for device [%u:%u] is busy with a snapshot\n",
		       MAJOR(dev_id), MINOR(dev_id));
		ret = -EBUSY;
		goto put_tracker;
	}

	ret = tracker_filter_detach(bdev);
	if (ret)
		pr_err("Failed to remove tracker from device [%u:%u]\n",
		       MAJOR(dev_id), MINOR(dev_id));
	else {
		struct tracked_device *tr_dev = NULL;
		struct tracked_device *iter_tr_dev;

		spin_lock(&tracked_device_lock);
		list_for_each_entry(iter_tr_dev, &tracked_device_list, link) {
			if (iter_tr_dev->dev_id == dev_id) {
				list_del(&iter_tr_dev->link);
				tr_dev = iter_tr_dev;
				break;
			}
		}
		spin_unlock(&tracked_device_lock);

		kfree(tr_dev);
#ifdef CONFIG_BLK_SNAP_DEBUG_MEMORY_LEAK
		if (tr_dev)
			memory_object_dec(memory_object_tracked_device);
#endif
	}
put_tracker:
	tracker_put(tracker);
put_bdev:
	blkdev_put(bdev, 0);
	return ret;
}

int tracker_read_cbt_bitmap(dev_t dev_id, unsigned int offset, size_t length,
			    char __user *user_buff)
{
	int ret;
	struct tracker *tracker;
	struct block_device *bdev;

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Cannot open device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		return PTR_ERR(bdev);
	}

	tracker = tracker_get_by_dev(bdev);
	if (!tracker) {
		pr_err("Unable to read CBT bitmap for device [%u:%u]: ",
		       MAJOR(dev_id), MINOR(dev_id));
		pr_err("tracker not found\n");
		ret = -ENODATA;
		goto put_bdev;
	}

	if (!atomic_read(&tracker->snapshot_is_taken)) {
		pr_err("Unable to read CBT bitmap for device [%u:%u]: ",
		       MAJOR(dev_id), MINOR(dev_id));
		pr_err("device is not captured by snapshot\n");
		ret = -EPERM;
		goto put_tracker;
	}

	ret = cbt_map_read_to_user(tracker->cbt_map, user_buff, offset, length);

put_tracker:
	tracker_put(tracker);
put_bdev:
	blkdev_put(bdev, 0);
	return ret;
}

static inline void collect_cbt_info(dev_t dev_id,
				    struct blk_snap_cbt_info *cbt_info)
{
	struct block_device *bdev;
	struct tracker *tracker;

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Cannot open device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		return;
	}

	tracker = tracker_get_by_dev(bdev);
	if (!tracker)
		goto put_bdev;
	if (!tracker->cbt_map)
		goto put_tracker;
	cbt_info->device_capacity =
		(__u64)(tracker->cbt_map->device_capacity << SECTOR_SHIFT);
	cbt_info->blk_size = (__u32)cbt_map_blk_size(tracker->cbt_map);
	cbt_info->blk_count = (__u32)tracker->cbt_map->blk_count;
	cbt_info->snap_number = (__u8)tracker->cbt_map->snap_number_previous;
	uuid_copy(&cbt_info->generation_id, &tracker->cbt_map->generation_id);
put_tracker:
	tracker_put(tracker);
put_bdev:
	blkdev_put(bdev, 0);
}

int tracker_collect(int max_count, struct blk_snap_cbt_info *cbt_info,
		    int *pcount)
{
	int ret = 0;
	int count = 0;
	int iter = 0;
	struct tracked_device *tr_dev;

	if (!cbt_info) {
		/**
		 * Just calculate trackers list length.
		 */
		spin_lock(&tracked_device_lock);
		list_for_each_entry(tr_dev, &tracked_device_list, link)
			++count;
		spin_unlock(&tracked_device_lock);
		goto out;
	}

	spin_lock(&tracked_device_lock);
	list_for_each_entry(tr_dev, &tracked_device_list, link) {
		if (count >= max_count) {
			ret = -ENOBUFS;
			break;
		}

		cbt_info[count].dev_id.mj = MAJOR(tr_dev->dev_id);
		cbt_info[count].dev_id.mn = MINOR(tr_dev->dev_id);
		++count;
	}
	spin_unlock(&tracked_device_lock);

	if (ret)
		return ret;

	for (iter = 0; iter < count; iter++) {
		dev_t dev_id = MKDEV(cbt_info[iter].dev_id.mj,
				     cbt_info[iter].dev_id.mn);

		collect_cbt_info(dev_id, &cbt_info[iter]);
	}
out:
	*pcount = count;
	return 0;
}

int tracker_mark_dirty_blocks(dev_t dev_id,
			      struct blk_snap_block_range *block_ranges,
			      unsigned int count)
{
	int ret = 0;
	struct tracker *tracker;
	struct block_device *bdev;

	bdev = blkdev_get_by_dev(dev_id, 0, NULL);
	if (IS_ERR(bdev)) {
		pr_err("Cannot open device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		return PTR_ERR(bdev);
	}

	pr_debug("Marking [%d] dirty blocks for device [%u:%u]\n", count,
		 MAJOR(dev_id), MINOR(dev_id));

	tracker = tracker_get_by_dev(bdev);
	if (!tracker) {
		pr_err("Cannot find device [%u:%u]\n", MAJOR(dev_id),
		       MINOR(dev_id));
		ret = -ENODEV;
		goto put_bdev;
	}

	ret = cbt_map_mark_dirty_blocks(tracker->cbt_map, block_ranges, count);
	if (ret)
		pr_err("Failed to set CBT table. errno=%d\n", abs(ret));

	tracker_put(tracker);
put_bdev:
	blkdev_put(bdev, 0);
	return ret;
}
