#include "common.h"

#include "snapimage.h"
#include "blk_util.h"
#include "defer_io.h"
#include "queue_spinlocking.h"
#include "cbt_map.h"
#include "tracker.h"

#include <asm/div64.h>
#include <linux/cdrom.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h> 
#include <linux/kthread.h>

#define SECTION "snapimage "
#include "log_format.h"

#define SNAPIMAGE_MAX_DEVICES 2048

static int g_snapimage_major = 0;
static unsigned long* g_snapimage_minors;
static DEFINE_SPINLOCK(g_snapimage_minors_lock);

LIST_HEAD(snap_images);
DECLARE_RWSEM(snap_images_lock);

struct rw_semaphore snap_image_destroy_lock;

typedef struct snapimage_s{
	struct list_head link;

	sector_t capacity;
	dev_t original_dev;

	defer_io_t* defer_io;
	cbt_map_t* cbt_map;

	dev_t image_dev;

	spinlock_t queue_lock;		// For exclusive access to our request queue
	struct request_queue* queue;
	struct gendisk* disk;

	atomic_t own_cnt;

	queue_sl_t rq_proc_queue;

	struct task_struct* rq_processor;

	wait_queue_head_t rq_proc_event;
	wait_queue_head_t rq_complete_event;

	atomic64_t state_received;

	atomic64_t state_inprocess;
	atomic64_t state_processed;

	volatile sector_t last_read_sector;
	volatile sector_t last_read_size;
	volatile sector_t last_write_sector;
	volatile sector_t last_write_size;

	struct mutex open_locker;
	struct block_device* open_bdev;
	volatile size_t open_cnt;
}snapimage_t;

int _snapimage_destroy( snapimage_t* image );

int _snapimage_open( struct block_device *bdev, fmode_t mode )
{
	int res = SUCCESS;

	//log_tr_format( "Snapshot image open device [%d:%d]. Block device object 0x%p", MAJOR( bdev->bd_dev ), MINOR( bdev->bd_dev ), bdev );
	if (bdev->bd_disk == NULL){
		log_err_dev_t( "Unable to open snapshot image: bd_disk is NULL. Device ", bdev->bd_dev );
		log_err_p( "Block device object ", bdev );
		return -ENODEV;
	}

	down_read(&snap_image_destroy_lock);
	do{
		snapimage_t* image = bdev->bd_disk->private_data;
		if (image == NULL){
			log_err_p( "Unable to open snapshot image: private data is not initialized. Block device object ", bdev );
			res = - ENODEV;
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

int _snapimage_getgeo( struct block_device* bdev, struct hd_geometry * geo )
{
	int res = SUCCESS;
	sector_t quotient;

	down_read(&snap_image_destroy_lock);
	do{
		snapimage_t* image = bdev->bd_disk->private_data;
		if (image == NULL){
			log_err_p( "Unable to open snapshot image: private data is not initialized. Block device object", bdev );
			res = - ENODEV;
			break;
		}

		log_tr_dev_t( "Getting geo for snapshot image device ", image->image_dev );

		geo->start = 0;
		if (image->capacity > 63){

			geo->sectors = 63;
			quotient = do_div_inline( image->capacity + (63 - 1), 63 );

			if (quotient > 255ULL){
				geo->heads = 255;
				geo->cylinders = (unsigned short)do_div_inline( quotient + (255 - 1), 255 );
			}
			else{
				geo->heads = (unsigned char)quotient;
				geo->cylinders = 1;
			}
		}
		else{
			geo->sectors = (unsigned char)image->capacity;
			geo->cylinders = 1;
			geo->heads = 1;
		}

		log_tr_format( "capacity=%lld, heads=%d, cylinders=%d, sectors=%d", image->capacity, geo->heads, geo->cylinders, geo->sectors );
	} while (false);
	up_read(&snap_image_destroy_lock);

	return res;
}

void _snapimage_close( struct gendisk *disk, fmode_t mode )
{
	if (disk->private_data != NULL){
		down_read(&snap_image_destroy_lock);
		do{
			snapimage_t* image = disk->private_data;

			//log_tr_format( "Snapshot image close device [%d:%d]. Block device object 0x%p", MAJOR( image->open_bdev->bd_dev ), MINOR( image->open_bdev->bd_dev ), image->open_bdev );

			mutex_lock( &image->open_locker );
			{
				if (image->open_cnt > 0)
					image->open_cnt--;

				if (image->open_cnt == 0)
					image->open_bdev = NULL;
			}
			mutex_unlock( &image->open_locker );
		} while (false);
		up_read(&snap_image_destroy_lock);
	}
	else
		log_err( "Unable to to close snapshot image: private data is not initialized" );
}

int _snapimage_ioctl( struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg )
{
	int res = -ENOTTY;
	down_read(&snap_image_destroy_lock);
	{
		snapimage_t* image = bdev->bd_disk->private_data;

		switch (cmd) {
			/*
			* The only command we need to interpret is HDIO_GETGEO, since
			* we can't partition the drive otherwise.  We have no real
			* geometry, of course, so make something up.
			*/
		case HDIO_GETGEO:
		{
			struct hd_geometry geo;

			res = _snapimage_getgeo( bdev, &geo );

			if (copy_to_user( (void *)arg, &geo, sizeof( geo ) ))
				res = -EFAULT;
			else
				res = SUCCESS;
		}
		break;
		case CDROM_GET_CAPABILITY: //0x5331  / * get capabilities * / 
		{
			struct gendisk *disk = bdev->bd_disk;

			if (bdev->bd_disk && (disk->flags & GENHD_FL_CD))
				res = SUCCESS;
			else
				res = -EINVAL;
		}
		break;

		default:
			log_tr_format( "Snapshot image ioctl receive unsupported command. Device [%d:%d], command 0x%x, arg 0x%lx",
				MAJOR( image->image_dev ), MINOR( image->image_dev ), cmd, arg );

			res = -ENOTTY; /* unknown command */
		}
	}
	up_read(&snap_image_destroy_lock);
	return res;
}

#ifdef CONFIG_COMPAT
int _snapimage_compat_ioctl( struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg )
{
	down_read(&snap_image_destroy_lock);
	{
		snapimage_t* image = bdev->bd_disk->private_data;

		log_tr_format( "Snapshot image compat ioctl receive unsupported command. Device [%d:%d], command 0x%x, arg 0xlx",
			MAJOR( image->image_dev ), MINOR( image->image_dev ), cmd, arg );
	}
	up_read(&snap_image_destroy_lock);
	return -ENOTTY;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
blk_qc_t _snapimage_submit_bio(struct bio *bio);
#endif

static struct block_device_operations g_snapimage_ops = {
	.owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
	.submit_bio = _snapimage_submit_bio,
#endif
	.open = _snapimage_open,
	.ioctl = _snapimage_ioctl,
	.release = _snapimage_close,
#ifdef CONFIG_COMPAT
	.compat_ioctl = _snapimage_compat_ioctl,
#endif
};

int _snapimage_request_read( defer_io_t* p_defer_io, blk_redirect_bio_endio_t* rq_endio )
{
	int res = -ENODATA;

	res = snapstore_device_read( p_defer_io->snapstore_device, rq_endio );

	return res;
}

int _snapimage_request_write( snapimage_t * image, blk_redirect_bio_endio_t* rq_endio )
{
	int res = SUCCESS;

	defer_io_t* p_defer_io = image->defer_io;
	cbt_map_t* cbt_map = image->cbt_map;

	BUG_ON( NULL == p_defer_io );
	BUG_ON( NULL == cbt_map );


	if (snapstore_device_is_corrupted( p_defer_io->snapstore_device ))
		return -ENODATA;

	if (!bio_has_data( rq_endio->bio )){
		log_warn_sz( "Snapshot image receive empty block IO. Flags=", rq_endio->bio->bi_flags );

		blk_redirect_complete( rq_endio, SUCCESS );
		return SUCCESS;
	}



	if (cbt_map != NULL){
		sector_t ofs = rq_endio->bio->bi_iter.bi_sector;
		sector_t cnt = bio_sectors(rq_endio->bio);

		res = cbt_map_set_both( cbt_map, ofs, cnt );
		if (res != SUCCESS){
			log_err_d( "Unable to write data to snapshot image: failed to set CBT map. errno=", res );
		}
	}

	res = snapstore_device_write( p_defer_io->snapstore_device, rq_endio );

	if (res != SUCCESS){
		log_err( "Failed to write data to snapshot image" );
		return res;
	}

	return res;
}

void _snapimage_processing( snapimage_t * image )
{
	int res = SUCCESS;
	blk_redirect_bio_endio_t* rq_endio;

	atomic64_inc( &image->state_inprocess );
	rq_endio = (blk_redirect_bio_endio_t*)queue_sl_get_first( &image->rq_proc_queue );

	if (bio_data_dir( rq_endio->bio ) == READ){
		image->last_read_sector = rq_endio->bio->bi_iter.bi_sector;
		image->last_read_size =  bio_sectors(rq_endio->bio);

		res = _snapimage_request_read( image->defer_io, rq_endio );
		if (res != SUCCESS){
			log_err_d( "Failed to read data from snapshot image. errno=", res );
		}
	}
	else{
		image->last_write_sector = rq_endio->bio->bi_iter.bi_sector;
		image->last_write_size = bio_sectors(rq_endio->bio);

		res = _snapimage_request_write( image, rq_endio );
		if (res != SUCCESS){
			log_err_d( "Failed to write data to snapshot image. errno=", res );
		}
	}

	if (res != SUCCESS)
		blk_redirect_complete( rq_endio, res );
}


int snapimage_processor_waiting( snapimage_t *image )
{
	int res = SUCCESS;

	if (queue_sl_empty( image->rq_proc_queue )){
		res = wait_event_interruptible_timeout( image->rq_proc_event, (!queue_sl_empty( image->rq_proc_queue ) || kthread_should_stop( )), 5 * HZ );
		if (res > 0){
			res = SUCCESS;
		}
		else if (res == 0){
			res = -ETIME;
		}
	}
	return res;
}


int snapimage_processor_thread( void *data )
{

	snapimage_t *image = data;
	
	log_tr_format( "Snapshot image thread for device [%d:%d] start", MAJOR( image->image_dev ), MINOR( image->image_dev ) );

	add_disk( image->disk );

	//priority
	set_user_nice( current, -20 ); //MIN_NICE

	while ( !kthread_should_stop( ) )
	{
		int res = snapimage_processor_waiting( image );
		if (res == SUCCESS){
			if (!queue_sl_empty( image->rq_proc_queue ))
				_snapimage_processing( image );
		} else if (res == -ETIME){
			//Nobody read me
		}
		else{
			log_err_d( "Failed to wait snapshot image thread queue. errno=", res );
			return res;
		}
		schedule( );
	}
	log_tr( "Snapshot image disk delete" );
	del_gendisk( image->disk );

	while (!queue_sl_empty( image->rq_proc_queue ))
		_snapimage_processing( image );

	log_tr_format( "Snapshot image thread for device [%d:%d] complete", MAJOR( image->image_dev ), MINOR( image->image_dev ) );
	return 0;
}


static inline void _snapimage_bio_complete( struct bio* bio, int err )
{
	if (err == SUCCESS)
		bio->bi_status = BLK_STS_OK;
	else
		bio->bi_status = BLK_STS_IOERR;

	bio_endio( bio );
}

void _snapimage_bio_complete_cb( void* complete_param, struct bio* bio, int err )
{
	snapimage_t* image = (snapimage_t*)complete_param;

	atomic64_inc( &image->state_processed );

	_snapimage_bio_complete( bio, err );

	if (queue_sl_unactive( image->rq_proc_queue )){
		wake_up_interruptible( &image->rq_complete_event );
	}

	atomic_dec( &image->own_cnt );
}


int _snapimage_throttling( defer_io_t* defer_io )
{
	return wait_event_interruptible( defer_io->queue_throttle_waiter, queue_sl_empty( defer_io->dio_queue ) );
}

#if LINUX_VERSION_CODE < KERNEL_VERSION( 5, 9, 0 )
blk_qc_t _snapimage_make_request(struct request_queue *q, struct bio *bio)
{
#else
blk_qc_t _snapimage_submit_bio(struct bio *bio)
{
	struct request_queue *q = bio->bi_disk->queue;
#endif


	blk_qc_t result = SUCCESS;

	snapimage_t* image = q->queuedata;


	if (unlikely(blk_mq_queue_stopped(q)))
	{
		log_tr_lx( "Failed to make snapshot image request. Queue already is not active. Queue flags=", q->queue_flags );
		_snapimage_bio_complete( bio, -ENODEV );

		return result;
	}

	atomic_inc( &image->own_cnt );
	do{
		blk_redirect_bio_endio_t* rq_endio;

		if (false == atomic_read( &(image->rq_proc_queue.active_state) )){
			_snapimage_bio_complete( bio, -ENODEV );
			break;
		}

		if (snapstore_device_is_corrupted( image->defer_io->snapstore_device )){
			_snapimage_bio_complete( bio, -ENODATA );
			break;
		}

		{
			int res = _snapimage_throttling( image->defer_io );
			if (SUCCESS != res){
				log_err_d( "Failed to throttle snapshot image device. errno=", res );
				_snapimage_bio_complete( bio, res );
				break;
			}
		}

		rq_endio = (blk_redirect_bio_endio_t*)queue_content_sl_new_opt( &image->rq_proc_queue, GFP_NOIO );
		if (NULL == rq_endio){
			log_err("Unable to make snapshot image request: failed to allocate redirect bio structure");
			_snapimage_bio_complete( bio, -ENOMEM );
			break;
		}
		rq_endio->bio = bio;
		rq_endio->complete_cb = _snapimage_bio_complete_cb;
		rq_endio->complete_param = (void*)image;
		atomic_inc( &image->own_cnt );

		atomic64_inc( &image->state_received );

		if (SUCCESS == queue_sl_push_back( &image->rq_proc_queue, &rq_endio->content )){
			wake_up( &image->rq_proc_event );
		}
		else{
			queue_content_sl_free( &rq_endio->content );
			_snapimage_bio_complete( bio, -EIO );

			if (queue_sl_unactive( image->rq_proc_queue )){
				wake_up_interruptible( &image->rq_complete_event );
			}
		}

	}while (false);
	atomic_dec( &image->own_cnt );

	return result;
}



static inline void _snapimage_free( snapimage_t* image )
{
	defer_io_put_resource( image->defer_io );
	cbt_map_put_resource( image->cbt_map );
	image->defer_io = NULL;
}


int snapimage_create( dev_t original_dev )
{
	int res = SUCCESS;
	defer_io_t*	defer_io = NULL;
	cbt_map_t* cbt_map = NULL;
	snapimage_t* image = NULL;
	struct gendisk *disk = NULL;
	int minor;
	blk_dev_info_t original_dev_info;

	log_tr_dev_t( "Create snapshot image for device ", original_dev );

	res = blk_dev_get_info( original_dev, &original_dev_info );
	if (res != SUCCESS){
		log_err( "Failed to obtain original device info" );
		return res;
	}

	{
		tracker_t* tracker = NULL;
		res = tracker_find_by_dev_id( original_dev, &tracker );
		if (res != SUCCESS){
			log_err_dev_t( "Unable to create snapshot image: cannot find tracker for device ", original_dev );
			return res;
		}
		defer_io = tracker->defer_io;
		cbt_map = tracker->cbt_map;
	}

	image = kmalloc( sizeof(snapimage_t), GFP_KERNEL);
	if (image == NULL){
		log_err("Failed to allocate snapshot image structure" );
		return -ENOMEM;
	}
	INIT_LIST_HEAD( &image->link );

	do{
		spin_lock( &g_snapimage_minors_lock );
		minor = bitmap_find_free_region( g_snapimage_minors, SNAPIMAGE_MAX_DEVICES, 0 );
		spin_unlock( &g_snapimage_minors_lock );

		if (minor < SUCCESS){
			log_err_d( "Failed to allocate minor for snapshot image device. errno=", 0-minor );
			break;
		}

		image->rq_processor = NULL;
		atomic64_set( &image->state_received, 0 );
		atomic64_set( &image->state_inprocess, 0 );
		atomic64_set( &image->state_processed, 0 );

		image->capacity = original_dev_info.count_sect;

		image->defer_io = defer_io_get_resource( defer_io );
		image->cbt_map = cbt_map_get_resource( cbt_map );
		image->original_dev = original_dev;

		image->image_dev = MKDEV( g_snapimage_major, minor );
		log_tr_dev_t( "Snapshot image device id ", image->image_dev );

		atomic_set( &image->own_cnt, 0 );

		// queue with per request processing
		spin_lock_init( &image->queue_lock );

		mutex_init( &image->open_locker );
		image->open_bdev = NULL;
		image->open_cnt = 0;
	   
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
		image->queue = blk_alloc_queue(NUMA_NO_NODE);
#else
		image->queue = blk_alloc_queue(_snapimage_make_request, NUMA_NO_NODE);
#endif

		if (NULL == image->queue){
			log_err( "Unable to create snapshot image: failed to allocate block device queue" );
			res = -ENOMEM;
			break;
		}
		image->queue->queuedata = image;

		blk_queue_max_segment_size( image->queue, 1024 * PAGE_SIZE );

		{
			unsigned int physical_block_size = original_dev_info.physical_block_size;
			unsigned short logical_block_size = original_dev_info.logical_block_size;//SECTOR512;

			log_tr_d( "Snapshot image physical block size ", physical_block_size );
			log_tr_d( "Snapshot image logical block size ", logical_block_size );

			blk_queue_physical_block_size( image->queue, physical_block_size );
			blk_queue_logical_block_size( image->queue, logical_block_size );
		}
		disk = alloc_disk( 1 );//only one partition on disk
		if (disk == NULL){
			log_err( "Failed to allocate disk for snapshot image device" );
			res = -ENOMEM;
			break;
		}
		image->disk = disk;

		if (snprintf( disk->disk_name, DISK_NAME_LEN, "%s%d", SNAP_IMAGE_NAME, minor ) < 0){
			log_err_d( "Unable to set disk name for snapshot image device: invalid minor ", minor );
			res = -EINVAL;
			break;
		}

		log_tr_format( "Snapshot image disk name [%s]", disk->disk_name );

		disk->flags |= GENHD_FL_NO_PART_SCAN;
		disk->flags |= GENHD_FL_REMOVABLE;

		disk->major = g_snapimage_major;
		disk->minors = 1;	// one disk have only one partition.
		disk->first_minor = minor;

		disk->private_data = image;

		disk->fops = &g_snapimage_ops;
		disk->queue = image->queue;

		set_capacity( disk, image->capacity );
		log_tr_format( "Snapshot image device capacity %lld bytes", sector_to_streamsize(image->capacity) );

		//res = -ENOMEM;
		res = queue_sl_init( &image->rq_proc_queue, sizeof( blk_redirect_bio_endio_t ) );
		if (res != SUCCESS){
			log_err_d( "Failed to initialize request processing queue for snapshot image device. errno=", res );
			break;
		}

		{
			struct task_struct* task = kthread_create( snapimage_processor_thread, image, disk->disk_name );
			if (IS_ERR( task )) {
				res = PTR_ERR( task );
				log_err_d( "Failed to create request processing thread for snapshot image device. errno=", res );
				break;
			}
			image->rq_processor = task;
		}
		init_waitqueue_head( &image->rq_complete_event );

		init_waitqueue_head( &image->rq_proc_event );
		wake_up_process( image->rq_processor );

		//log_tr_p( "disk=", disk );

	} while (false);

	if (res == SUCCESS) {
		down_write( &snap_images_lock );
		list_add_tail( &image->link, &snap_images );
		up_write( &snap_images_lock );
	}
	else{

		res = _snapimage_destroy( image );
		if (res == SUCCESS){
			_snapimage_free( image );
			kfree(image);
			image = NULL;
		}

	}
	return res;
}


void _snapimage_stop( snapimage_t* image )
{
	if (image->rq_processor != NULL){
		if (queue_sl_active( &image->rq_proc_queue, false )){
			struct request_queue* q = image->queue;

			log_tr( "Snapshot image request processing stop" );

			if (!blk_queue_stopped( q )){
				blk_sync_queue(q);
				blk_mq_stop_hw_queues(q);
			}
		}

		log_tr( "Snapshot image thread stop" );
		kthread_stop( image->rq_processor );
		image->rq_processor = NULL;

		while (!queue_sl_unactive( image->rq_proc_queue ))
			wait_event_interruptible( image->rq_complete_event, queue_sl_unactive( image->rq_proc_queue ) );
	}
}


int _snapimage_destroy( snapimage_t* image )
{
	if (image->rq_processor != NULL)
		_snapimage_stop( image );

	if (image->queue) {
		log_tr( "Snapshot image queue cleanup" );
		blk_cleanup_queue( image->queue );
		image->queue = NULL;
	}

	if (image->disk != NULL){
		struct gendisk*	disk = image->disk;
		image->disk = NULL;

		log_tr( "Snapshot image disk structure release" );

		disk->private_data = NULL;
		put_disk( disk );
	}
	queue_sl_done( &image->rq_proc_queue );

	spin_lock( &g_snapimage_minors_lock );
	bitmap_clear( g_snapimage_minors, MINOR(image->image_dev), (int)1 );
	spin_unlock( &g_snapimage_minors_lock );

	return SUCCESS;
}
	
snapimage_t* snapimage_find(dev_t original_dev)
{
	snapimage_t* image = NULL;

	down_read( &snap_images_lock );
	if (!list_empty( &snap_images )) { 
		struct list_head* _list_head;

		list_for_each( _list_head, &snap_images ) {
			snapimage_t* _image = list_entry( _list_head, snapimage_t, link );

			if (_image->original_dev == original_dev){
				image = _image;
				break;
			}
		}
	}
	up_read(&snap_images_lock);

	return image;
}

int snapimage_stop( dev_t original_dev )
{
	int res = SUCCESS;

	log_tr_dev_t( "Snapshot image processing stop for original device ", original_dev );

	down_read(&snap_image_destroy_lock);
	{
		snapimage_t* image = snapimage_find(original_dev);

		if (image != NULL) {
			_snapimage_stop( image );
			res = SUCCESS;
		}
		else{
			log_err_d( "Snapshot image is not removed. errno=", res );
			res = -ENODATA;
		}
	}
	up_read(&snap_image_destroy_lock);
	return res;
}

int snapimage_destroy( dev_t original_dev )
{
	int res = SUCCESS;
	snapimage_t* image = NULL;

	log_tr_dev_t( "Destroy snapshot image for device ", original_dev );

	down_write( &snap_images_lock );
	if (!list_empty( &snap_images )) { 
		struct list_head* _list_head;

		list_for_each( _list_head, &snap_images ) {
			snapimage_t* _image = list_entry( _list_head, snapimage_t, link );

			if ( _image->original_dev == original_dev){
				image = _image;
				list_del( &image->link);
				break;
			}
		}
	}
	up_write(&snap_images_lock);

	if (image != NULL){
		down_write(&snap_image_destroy_lock);
		res = _snapimage_destroy( image );
		if (SUCCESS == res){
			_snapimage_free( image );
			kfree( image);
			image = NULL;
			res = SUCCESS;
		}
		else
			log_err_d( "Failed to destroy snapshot image device. errno=", res );

		up_write(&snap_image_destroy_lock);
	}
	else{
		log_err_d( "Snapshot image is not removed. errno=", res );
		res = -ENODATA;
	}

	return res;
}

int snapimage_destroy_for( dev_t* p_dev, int count )
{
	int res = SUCCESS;
	int inx = 0;

	for (; inx < count; ++inx){
		int local_res = snapimage_destroy( p_dev[inx] );
		if (local_res != SUCCESS){
			log_err_format( "Failed to release snapshot image for original device [%d:%d]. errno=%d", 
				MAJOR( p_dev[inx] ), MINOR( p_dev[inx] ), 0 - local_res );
			res = local_res;
		}
	}
	return res;
}

int snapimage_create_for( dev_t* p_dev, int count )
{
	int res = SUCCESS;
	int inx = 0;

	for (; inx < count; ++inx){
		res = snapimage_create( p_dev[inx] );
		if (res != SUCCESS){
			log_err_dev_t( "Failed to create snapshot image for original device ", p_dev[inx] );
			break;
		}
	}
	if (res != SUCCESS)
		if (inx > 0)
			snapimage_destroy_for( p_dev, inx-1 );
	return res;
}


int snapimage_init( void )
{
	int res = SUCCESS;

	init_rwsem(&snap_image_destroy_lock);

	res = register_blkdev( g_snapimage_major, SNAP_IMAGE_NAME );
	if (res >= SUCCESS){
		g_snapimage_major = res;
		log_tr_format( "Snapshot image block device major %d was registered", g_snapimage_major );
		res = SUCCESS;

		spin_lock( &g_snapimage_minors_lock );
		g_snapimage_minors = bitmap_zalloc(SNAPIMAGE_MAX_DEVICES, GFP_KERNEL);
		spin_unlock( &g_snapimage_minors_lock );

		if (g_snapimage_minors == NULL)
			log_err( "Failed to initialize bitmap of minors" );
	}
	else
		log_err_d( "Failed to register snapshot image block device. errno=", res );

	return res;
}

int snapimage_done( void )
{
	int res = SUCCESS;

	down_write(&snap_image_destroy_lock);
	while(true) {
		snapimage_t* image = NULL;

		down_write(&snap_image_destroy_lock);
		if (!list_empty(&snap_images)) {
			image = list_entry( snap_images.next, snapimage_t, link );

			list_del( &image->link );
		}
		up_write(&snap_image_destroy_lock);

		if (NULL == image)
			break;

		log_err_dev_t( "Snapshot image for device was unexpectedly removed ", image->original_dev);

		res = _snapimage_destroy( image );
		if (res) {
			log_err( "Failed to perform snapshot images cleanup" );
			break;
		}

		_snapimage_free( image );
		kfree( image );
		image = NULL;
	}

	if (res == SUCCESS){
		spin_lock(&g_snapimage_minors_lock);
		bitmap_free(g_snapimage_minors);
		g_snapimage_minors = NULL;
		spin_unlock(&g_snapimage_minors_lock);

		//BUG_ON(!list_empty(&snap_images))
		if (!list_empty(&snap_images))
			log_err("Failed to release snapshot images container" );

		unregister_blkdev( g_snapimage_major, SNAP_IMAGE_NAME );
		log_tr_format( "Snapshot image block device [%d] was unregistered", g_snapimage_major );
	}
	up_write(&snap_image_destroy_lock);
	return res;
}

int snapimage_collect_images( int count, struct image_info_s* p_user_image_info, int* p_real_count )
{
	int res = SUCCESS;
	int real_count = 0;

	down_read( &snap_images_lock );
	if (!list_empty(&snap_images)) {
		struct list_head* _list_head;

		list_for_each( _list_head, &snap_images )
			real_count++;
	}
	up_read( &snap_images_lock );
	*p_real_count = real_count;

	if (count < real_count)
		res = -ENODATA;

	real_count = min( count, real_count );
	if (real_count > 0){
		struct image_info_s* p_kernel_image_info = NULL;
		size_t buff_size;

		buff_size = sizeof( struct image_info_s )*real_count;
		p_kernel_image_info = kzalloc( buff_size, GFP_KERNEL );
		if (p_kernel_image_info == NULL){
			log_err_sz( "Unable to collect snapshot images: not enough memory. Size=", buff_size );
			return res = -ENOMEM;
		}

		down_read(&snap_image_destroy_lock);
		down_read( &snap_images_lock );

		if (!list_empty(&snap_images)) {
			size_t inx = 0;
			struct list_head* _list_head;

			list_for_each( _list_head, &snap_images ) {
				snapimage_t* img = list_entry( _list_head, snapimage_t, link );

				real_count++;

				p_kernel_image_info[inx].original_dev_id.major = MAJOR( img->original_dev );
				p_kernel_image_info[inx].original_dev_id.minor = MINOR( img->original_dev );

				p_kernel_image_info[inx].snapshot_dev_id.major = MAJOR( img->image_dev );
				p_kernel_image_info[inx].snapshot_dev_id.minor = MINOR( img->image_dev );

				++inx;
				if (inx > real_count)
					break;
			}
		}

		up_read( &snap_images_lock );
		up_read(&snap_image_destroy_lock);

		if (0 != copy_to_user( p_user_image_info, p_kernel_image_info, buff_size )){
			log_err( "Unable to collect snapshot images: failed to copy data to user buffer" );
			res = - ENODATA;
		}

		kfree( p_kernel_image_info );
	}

	return res;
}

int snapimage_mark_dirty_blocks(dev_t image_dev_id, struct block_range_s* block_ranges, unsigned int count)
{
	size_t inx = 0;
	int res = SUCCESS;
	log_tr_format("Marking [%d] dirty blocks for image device [%d:%d]", count, MAJOR(image_dev_id), MINOR(image_dev_id));

	down_read(&snap_image_destroy_lock);
	do {
		snapimage_t* image = snapimage_find(image_dev_id);

		if (image == NULL){
			log_err_dev_t("Cannot find device ", image_dev_id);
			res = - ENODEV;
			break;
		}

		for (inx = 0; inx < count; ++inx){
			sector_t ofs = (sector_t)block_ranges[inx].ofs;
			sector_t cnt = (sector_t)block_ranges[inx].cnt;

			res = cbt_map_set_both(image->cbt_map, ofs, cnt);
			if (res != SUCCESS){
				log_err_d("Failed to set CBT table. Errno=", res);
				break;
			}
		}
	} while (false);
	up_read(&snap_image_destroy_lock);

	return res;
}
