// Copyright (c) Veeam Software Group GmbH

#pragma once

#include "shared_resource.h"
#include "snapstore_device.h"
#include "tracker_queue.h"

typedef struct defer_io_s
{
    shared_resource_t sharing_header;

    wait_queue_head_t queue_add_event;

    atomic_t queue_filling_count;
    wait_queue_head_t queue_throttle_waiter;

    dev_t original_dev_id;
    struct block_device*  original_blk_dev;

    snapstore_device_t* snapstore_device;

    struct task_struct* dio_thread;

    void*  rangecopy_buff;
    size_t rangecopy_buff_size;

    queue_sl_t dio_queue;

    atomic64_t state_bios_received;
    atomic64_t state_bios_processed;
    atomic64_t state_sectors_received;
    atomic64_t state_sectors_processed;
    atomic64_t state_sectors_copy_read;
}defer_io_t;


int defer_io_create( dev_t dev_id, struct block_device* blk_dev, defer_io_t** pp_defer_io );
int defer_io_stop( defer_io_t* defer_io );

static inline defer_io_t* defer_io_get_resource( defer_io_t* defer_io )
{
    return (defer_io_t*)shared_resource_get( &defer_io->sharing_header );
}
static inline void defer_io_put_resource( defer_io_t* defer_io )
{
    shared_resource_put( &defer_io->sharing_header );
}
#if defined(VEEAMSNAP_DISK_SUBMIT_BIO)
int defer_io_redirect_bio( defer_io_t* defer_io, struct bio *bio, sector_t sectStart, sector_t sectCount,
        make_request_fn* target_make_request_fn, void* tracker );
#else
int defer_io_redirect_bio( defer_io_t* defer_io, struct bio *bio, sector_t sectStart, sector_t sectCount,
        struct request_queue *q, make_request_fn* target_make_request_fn, void* tracker );
#endif
void defer_io_print_state( defer_io_t* defer_io );
