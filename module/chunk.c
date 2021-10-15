// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME "-chunk: " fmt
#include <linux/slab.h>
#include <linux/dm-io.h>
#include "params.h"
#include "chunk.h"
#include "diff_area.h"
#include "diff_storage.h"

static
void diff_buffer_free(struct diff_buffer *diff_buffer)
{
	struct page_list *curr_page;

	if (unlikely(!diff_buffer))
		return;

	curr_page = diff_buffer->pages;
	while(curr_page) {
		if (curr_page->page)
			__free_page(curr_page->page);

		curr_page = curr_page->next;
	}
	kfree(diff_buffer);
}

static
struct diff_buffer *diff_buffer_new(size_t page_count, size_t buffer_size,
                                    gfp_t gfp_mask)
{
	struct diff_buffer *diff_buffer;
	size_t inx;
	struct page_list *prev_page;
	struct page_list *curr_page;
	struct page *page;

	if (unlikely(page_count <= 0))
		return NULL;

	diff_buffer = kzalloc(sizeof(struct diff_buffer) + page_count * sizeof(struct page_list),
	                      gfp_mask);
	if (!diff_buffer)
		return NULL;

	diff_buffer->size = buffer_size;

	/* Allocate first page */
	page = alloc_page(gfp_mask);
	if (!page)
		goto fail;

	diff_buffer->pages[0].page = page;
	prev_page = diff_buffer->pages;

	/* Allocate all other pages and make list link */
	for (inx = 1; inx < page_count; inx++) {
		page = alloc_page(gfp_mask);
		if (!page)
			goto fail;

		curr_page = prev_page + 1;
		curr_page->page = page;

		prev_page->next = curr_page;
		prev_page = curr_page;
	}

	return diff_buffer;
fail:
	diff_buffer_free(diff_buffer);
	return NULL;
}

static inline
void chunk_store_failed(struct chunk *chunk, int error)
{
	chunk_state_set(chunk, CHUNK_ST_FAILED);
	diff_area_set_corrupted(chunk->diff_area, error);
	up_read(&chunk->lock);
};

void chunk_schedule_storing(struct chunk *chunk)
{
	int ret;
	struct diff_area *diff_area = chunk->diff_area;

	might_sleep();
	WARN_ON(!rwsem_is_locked(&chunk->lock));

	if (diff_area->in_memory) {
		up_write(&chunk->lock);
		return;
	}

	downgrade_write(&chunk->lock);

	if (!chunk->diff_store) {
		struct diff_store *diff_store;

		diff_store = diff_storage_get_store(
				diff_area->diff_storage,
				diff_area_chunk_sectors(diff_area));
		if (unlikely(IS_ERR(diff_store))) {
			//pr_err("Cannot get new diff storage for chunk #%lu", chunk->number);
			chunk_store_failed(chunk, PTR_ERR(diff_store));
			return;
		}

		chunk->diff_store = diff_store;
	}

	ret = chunk_async_store_diff(chunk);
	if (ret)
		chunk_store_failed(chunk, ret);
}

void chunk_schedule_caching(struct chunk *chunk)
{
	bool need_to_cleanup = false;
	struct diff_area *diff_area = chunk->diff_area;

	might_sleep();
	WARN_ON(!rwsem_is_locked(&chunk->lock));

	spin_lock(&diff_area->cache_list_lock);
	if (!chunk_state_check(chunk, CHUNK_ST_IN_CACHE)) {
		chunk_state_set(chunk, CHUNK_ST_IN_CACHE);
		list_add_tail(&chunk->cache_link, &diff_area->caching_chunks);
		need_to_cleanup =
			atomic_inc_return(&diff_area->caching_chunks_count) >
			chunk_maximum_in_cache;
	}
	spin_unlock(&diff_area->cache_list_lock);

	up_read(&chunk->lock);

	// Initiate the cache clearing process.
	if (need_to_cleanup)
		queue_work(system_wq, &diff_area->caching_chunks_work);
}

static
void chunk_notify_work(struct work_struct *work)
{
	struct chunk *chunk = container_of(work, struct chunk, notify_work);

	might_sleep();

	if (unlikely(chunk->error)) {
		chunk_state_set(chunk, CHUNK_ST_FAILED);
		diff_area_set_corrupted(chunk->diff_area, chunk->error);

		WARN_ON(!rwsem_is_locked(&chunk->lock));
		if (chunk_state_check(chunk, CHUNK_ST_LOADING))
			up_write(&chunk->lock);

		if (chunk_state_check(chunk, CHUNK_ST_STORING))
			up_read(&chunk->lock);

		return;
	}

	if (chunk_state_check(chunk, CHUNK_ST_LOADING)) {
		chunk_state_unset(chunk, CHUNK_ST_LOADING);

		chunk_state_set(chunk, CHUNK_ST_BUFFER_READY);

		pr_debug("Chunk 0x%lu was read\n", chunk->number);

		chunk_schedule_storing(chunk);
		return;
	}

	if (chunk_state_check(chunk, CHUNK_ST_STORING)) {
		chunk_state_unset(chunk, CHUNK_ST_STORING);

		chunk_state_set(chunk, CHUNK_ST_STORE_READY);

		chunk_schedule_caching(chunk);
		return;
	}
}

struct chunk *chunk_alloc(struct diff_area *diff_area, unsigned long number)
{
	struct chunk *chunk;

	chunk = kzalloc(sizeof(struct chunk), GFP_KERNEL);
	if (!chunk)
		return NULL;

	INIT_LIST_HEAD(&chunk->cache_link);
	init_rwsem(&chunk->lock);
	chunk->diff_area = diff_area;
	chunk->number = number;

	chunk->error = 0;
	INIT_WORK(&chunk->notify_work, chunk_notify_work);

	return chunk;
}

void chunk_free(struct chunk *chunk)
{
	if (unlikely(!chunk))
		return;

	diff_buffer_free(chunk->diff_buffer);
	kfree(chunk->diff_store);
	kfree(chunk);
}

/**
 * chunk_allocate_buffer() - Allocate diff buffer.
 * @chunk:
 * 	?
 * @gfp_mask:
 * 	?
 *
 * Don't forget to lock the chunk for writing before allocating the buffer.
 */
int chunk_allocate_buffer(struct chunk *chunk, gfp_t gfp_mask)
{
	struct diff_buffer *buf;
	size_t page_count;
	size_t buffer_size;

	page_count = round_up(chunk->sector_count, SECTOR_IN_PAGE) / SECTOR_IN_PAGE;
	buffer_size = chunk->sector_count << SECTOR_SHIFT;

	buf = diff_buffer_new(page_count, buffer_size, gfp_mask);
	if (!buf) {
		pr_err("Failed allocate memory buffer for chunk");
		return -ENOMEM;
	}
	chunk->diff_buffer = buf;

	return 0;
}

/**
 * chunk_free_buffer() - Free diff buffer.
 * @chunk:
 * 	?
 *
 * Don't forget to lock the chunk for writing before freeing the buffer.
 */
void chunk_free_buffer(struct chunk *chunk)
{
	diff_buffer_free(chunk->diff_buffer);
	chunk->diff_buffer = NULL;
	chunk_state_unset(chunk, CHUNK_ST_BUFFER_READY);
}

static inline
struct page_list *chunk_first_page(struct chunk *chunk)
{
	return chunk->diff_buffer->pages;
};


static
void notify_fn(unsigned long error, void *context)
{
	struct chunk *chunk = context;

	cant_sleep();
	chunk->error = error;
	queue_work(system_wq, &chunk->notify_work);
}

/**
 * chunk_async_store_diff() - Starts asynchronous storing of a chunk to the
 *	difference storage.
 *
 */
int chunk_async_store_diff(struct chunk *chunk)
{
	unsigned long sync_error_bits;
	struct dm_io_region region = {
		.bdev = chunk->diff_store->bdev,
		.sector = chunk->diff_store->sector,
		.count = chunk->sector_count,
	};
	struct dm_io_request reguest = {
		.bi_op = REQ_OP_WRITE,
		.bi_op_flags = 0,
		.mem.type = DM_IO_PAGE_LIST,
		.mem.offset = 0,
		.mem.ptr.pl = chunk_first_page(chunk),
		.notify.fn = notify_fn,
		.notify.context = chunk,
		.client = chunk->diff_area->io_client,
	};

	chunk_state_set(chunk, CHUNK_ST_STORING);
	return dm_io(&reguest, 1, &region, &sync_error_bits);
}

/**
 * chunk_asunc_load_orig() - Starts asynchronous loading of a chunk from
 * 	the origian block device.
 */
int chunk_asunc_load_orig(struct chunk *chunk)
{
	unsigned long sync_error_bits;
	struct dm_io_region region = {
		.bdev = chunk->diff_area->orig_bdev,
		.sector = (sector_t)(chunk->number) * diff_area_chunk_sectors(chunk->diff_area),
		.count = chunk->sector_count,
	};
	struct dm_io_request reguest = {
		.bi_op = REQ_OP_READ,
		.bi_op_flags = 0,
		.mem.type = DM_IO_PAGE_LIST,
		.mem.offset = 0,
		.mem.ptr.pl = chunk_first_page(chunk),
		.notify.fn = notify_fn,
		.notify.context = chunk,
		.client = chunk->diff_area->io_client,
	};

	chunk_state_set(chunk, CHUNK_ST_LOADING);
	return dm_io(&reguest, 1, &region, &sync_error_bits);
}

/**
 * chunk_load_orig() - Performs synchronous loading of a chunk from the
 * 	original block device.
 */
int chunk_load_orig(struct chunk *chunk)
{
	unsigned long sync_error_bits = 0;
	struct dm_io_region region = {
		.bdev = chunk->diff_area->orig_bdev,
		.sector = (sector_t)chunk->number * diff_area_chunk_sectors(chunk->diff_area),
		.count = chunk->sector_count,
	};
	struct dm_io_request reguest = {
		.bi_op = REQ_OP_READ,
		.bi_op_flags = 0,
		.mem.type = DM_IO_PAGE_LIST,
		.mem.offset = 0,
		.mem.ptr.pl = chunk_first_page(chunk),
		.notify.fn = NULL,
		.notify.context = NULL,
		.client = chunk->diff_area->io_client,
	};

	return dm_io(&reguest, 1, &region, &sync_error_bits);
}

/**
 * chunk_load_diff() - Performs synchronous loading of a chunk from the
 * 	difference storage.
 */
int chunk_load_diff(struct chunk *chunk)
{
	unsigned long sync_error_bits = 0;
	struct dm_io_region region = {
		.bdev = chunk->diff_store->bdev,
		.sector = chunk->diff_store->sector,
		.count = chunk->diff_store->count,
	};
	struct dm_io_request reguest = {
		.bi_op = REQ_OP_READ,
		.bi_op_flags = 0,
		.mem.type = DM_IO_PAGE_LIST,
		.mem.offset = 0,
		.mem.ptr.pl = chunk_first_page(chunk),
		.notify.fn = NULL,
		.notify.context = NULL,
		.client = chunk->diff_area->io_client,
	};

	return dm_io(&reguest, 1, &region, &sync_error_bits);
}
