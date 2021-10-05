// Copyright (c) Veeam Software Group GmbH

#include "stdafx.h"
#include "tracker.h"
#ifdef PERSISTENT_CBT
#include "cbt_persistent.h"
#endif
#include "blk_util.h"
#define SECTION "tracker   "
#include "log_format.h"


static container_sl_t trackers_container;

int tracker_init(void ){
    container_sl_init( &trackers_container, sizeof( tracker_t ) );
    return SUCCESS;
}

int tracker_done(void )
{
    int result = SUCCESS;

    result = tracker_remove_all();
    if (SUCCESS == result){
        if (SUCCESS != container_sl_done( &trackers_container ))
            log_err( "Failed to free up trackers container" );

        }
    else
        log_err("Failed to remove all tracking devices from tracking");

    return result;
}

#ifdef VEEAMSNAP_BDEV_BIO
int tracker_find_by_bdev(struct block_device *bdev, tracker_t** ptracker)
{
    int result = -ENODATA;
    content_sl_t *content = NULL;

    CONTAINER_SL_FOREACH_BEGIN(trackers_container, content) {
        tracker_t* tracker = (tracker_t*)content;

        if (bdev == tracker->target_dev) {
            *ptracker = tracker;
            result = SUCCESS;
            break;
        }
    }
    CONTAINER_SL_FOREACH_END(trackers_container);

    return result;
}
#else
int tracker_find_by_queue_and_sector(tracker_disk_t* queue, sector_t sector, tracker_t** ptracker )
{
    int result = -ENODATA;

    content_sl_t* content = NULL;
    tracker_t* tracker = NULL;
    CONTAINER_SL_FOREACH_BEGIN( trackers_container, content )
    {
        tracker = (tracker_t*)content;
        if ((queue == tracker->tr_disk) &&
            (sector >= blk_dev_get_start_sect( tracker->target_dev )) &&
            (sector < (blk_dev_get_start_sect( tracker->target_dev ) + blk_dev_get_capacity( tracker->target_dev )))
        ){
            *ptracker = tracker;
            result = SUCCESS;
            break;
        }
    }
    CONTAINER_SL_FOREACH_END( trackers_container );

    return result;
}
#endif

int tracker_find_intersection(tracker_disk_t* queue, sector_t b1, sector_t e1, tracker_t** ptracker)
{
    int result = -ENODATA;

    content_sl_t* content = NULL;
    tracker_t* tracker = NULL;
    CONTAINER_SL_FOREACH_BEGIN(trackers_container, content)
    {
        tracker = (tracker_t*)content;

        if (queue == tracker->tr_disk)
        {
            bool have_intersection = false;
            sector_t b2 = blk_dev_get_start_sect(tracker->target_dev);
            sector_t e2 = blk_dev_get_start_sect(tracker->target_dev) + blk_dev_get_capacity(tracker->target_dev);

            if ((b1 >= b2) && (e1 <= e2))
                have_intersection = true;
            if ((b1 <= b2) && (e1 >= e2))
                have_intersection = true;
            if ((b1 <= b2) && (e1 > b2))
                have_intersection = true;
            if ((b1 < e2) && (e1 >= e2))
                have_intersection = true;

            if (have_intersection){
                if (ptracker != NULL)
                    *ptracker = tracker;

                result = SUCCESS;
                break;
            }
        }
    }
    CONTAINER_SL_FOREACH_END(trackers_container);

    return result;
}

int tracker_find_by_dev_id( dev_t dev_id, tracker_t** ptracker )
{
    int result = -ENODATA;

    content_sl_t* content = NULL;
    tracker_t* tracker = NULL;
    CONTAINER_SL_FOREACH_BEGIN( trackers_container, content )
    {
        tracker = (tracker_t*)content;
        if (tracker->original_dev_id == dev_id){
            *ptracker = tracker;
            result =  SUCCESS;    //found!
            break;
        }
    }
    CONTAINER_SL_FOREACH_END( trackers_container );

    return result;
}

/*
int tracker_find_by_sb(struct super_block* sb, tracker_t** ptracker)
{
    int result = -ENODATA;

    content_sl_t* content = NULL;
    tracker_t* tracker = NULL;
    CONTAINER_SL_FOREACH_BEGIN(trackers_container, content)
    {
        tracker = (tracker_t*)content;
        if (tracker->sb == sb){
            *ptracker = tracker;
            result = SUCCESS;    //found!
            break;
        }
    }
    CONTAINER_SL_FOREACH_END(trackers_container);

    return result;
}
*/

int tracker_enum_cbt_info( int max_count, struct cbt_info_s* p_cbt_info, int* p_count )
{
    int result = -ENODATA;
    int count = 0;
    content_sl_t* content = NULL;
    tracker_t* tracker = NULL;
    CONTAINER_SL_FOREACH_BEGIN( trackers_container, content )
    {
        tracker = (tracker_t*)content;

        if (count >= max_count){
            result = -ENOBUFS;
            break;    //don`t continue
        }

        if (p_cbt_info != NULL){
            p_cbt_info[count].dev_id.major = MAJOR(tracker->original_dev_id);
            p_cbt_info[count].dev_id.minor = MINOR(tracker->original_dev_id);

            if (tracker->cbt_map){
                p_cbt_info[count].cbt_map_size = tracker->cbt_map->map_size;
                p_cbt_info[count].snap_number = (unsigned char)tracker->cbt_map->snap_number_previous;
                veeam_uuid_copy((veeam_uuid_t*)(p_cbt_info[count].generationId), &tracker->cbt_map->generationId_previous);
            }
            else{
                p_cbt_info[count].cbt_map_size = 0;
                p_cbt_info[count].snap_number = 0;
            }

            p_cbt_info[count].dev_capacity = sector_to_streamsize(blk_dev_get_capacity(tracker->target_dev));
        }
        //log_tr_dev_t("A device was found under tracking ", tracker->original_dev_id);
        ++count;
        result = SUCCESS;
    }
    CONTAINER_SL_FOREACH_END( trackers_container );
    *p_count = count;
    return result;
}

void tracker_cbt_start(tracker_t* tracker, unsigned long long snapshot_id, cbt_map_t* cbt_map)
{
    tracker_snapshot_id_set(tracker, snapshot_id);
    tracker->cbt_map = cbt_map_get_resource(cbt_map);
}


int tracker_create(unsigned long long snapshot_id, dev_t dev_id, unsigned int cbt_block_size_degree, cbt_map_t* cbt_map, tracker_t** ptracker)
{
    int result = SUCCESS;
    tracker_t* tracker = NULL;

    *ptracker = NULL;

    tracker = (tracker_t*)container_sl_new( &trackers_container );
    if (NULL==tracker)
        return -ENOMEM;

    atomic_set( &tracker->is_captured, false);
    tracker->is_unfreezable = false;
    init_rwsem(&tracker->unfreezable_lock);

    //tracker->put_super = NULL;
    //tracker->sb = NULL;

    tracker->original_dev_id = dev_id;

    result = blk_dev_open( tracker->original_dev_id, &tracker->target_dev );
    if (result != SUCCESS)
        return result;
    do{
#ifdef VEEAMSNAP_BLK_FREEZE
        struct super_block* superblock = NULL;
#endif

        log_tr_format( "Create tracker for device [%d:%d]. Start 0x%llx sector, capacity 0x%llx sectors",
            MAJOR( tracker->original_dev_id ), MINOR( tracker->original_dev_id ),
            (unsigned long long)blk_dev_get_start_sect( tracker->target_dev ),
            (unsigned long long)blk_dev_get_capacity(tracker->target_dev));

        if (cbt_map == NULL){
            cbt_map = cbt_map_create(cbt_block_size_degree-SECTOR_SHIFT, blk_dev_get_capacity(tracker->target_dev));
            if (cbt_map == NULL){
                result = -ENOMEM;
                break;
            }
            tracker_cbt_start(tracker, snapshot_id, cbt_map);
#ifdef PERSISTENT_CBT
            cbt_persistent_register(tracker->original_dev_id, tracker->cbt_map);
#endif
        }
        else
            tracker_cbt_start(tracker, snapshot_id, cbt_map);

#ifdef VEEAMSNAP_BLK_FREEZE
        result = blk_freeze_bdev( tracker->original_dev_id, tracker->target_dev, &superblock );
#else
        result = freeze_bdev(tracker->target_dev);
#endif
        if (result != SUCCESS){
            tracker->is_unfreezable = true;
            break;
        }
#if defined(VEEAMSNAP_DISK_SUBMIT_BIO)
        result = tracker_disk_ref(tracker->target_dev->bd_disk, &tracker->tr_disk);
#else
        result = tracker_disk_ref(tracker->target_dev->bd_disk->queue, &tracker->tr_disk);
#endif

#ifdef VEEAMSNAP_BLK_FREEZE
        superblock = blk_thaw_bdev( tracker->original_dev_id, tracker->target_dev, superblock );
#else
        result = thaw_bdev(tracker->target_dev);
        if (result != SUCCESS)
            log_err("Failed to thaw block device");
#endif

    }while(false);

    if (SUCCESS ==result){
        *ptracker = tracker;
    }else{
        int remove_status = SUCCESS;

        log_err_dev_t( "Failed to create tracker for device ", tracker->original_dev_id );

        remove_status = tracker_remove(tracker);
        if ((SUCCESS == remove_status) || (-ENODEV == remove_status))
            tracker = NULL;
        else
            log_err_d( "Failed to perfrom tracker cleanup. errno=", (0 - remove_status) );
        }

    return result;
}

int _tracker_remove( tracker_t* tracker )
{
    int result = SUCCESS;

    if (NULL != tracker->target_dev){
#ifdef VEEAMSNAP_BLK_FREEZE
        struct super_block* superblock = NULL;
#endif
        if (tracker->is_unfreezable)
            down_write(&tracker->unfreezable_lock);
        else {
#ifdef VEEAMSNAP_BLK_FREEZE
            result = blk_freeze_bdev(tracker->original_dev_id, tracker->target_dev, &superblock);
#else
            result = freeze_bdev(tracker->target_dev);
#endif
        }

        if (NULL != tracker->tr_disk){
            tracker_disk_unref( tracker->tr_disk );
            tracker->tr_disk = NULL;
        }
        if (tracker->is_unfreezable)
            up_write(&tracker->unfreezable_lock);
        else {
#ifdef VEEAMSNAP_BLK_FREEZE
            superblock = blk_thaw_bdev(tracker->original_dev_id, tracker->target_dev, superblock);
#else
            result = thaw_bdev(tracker->target_dev);
            if (result != SUCCESS)
                log_err("Failed to thaw block device");
#endif
        }

        blk_dev_close( tracker->target_dev );

        tracker->target_dev = NULL;
    }else
        result=-ENODEV;

    if (NULL != tracker->cbt_map){
        cbt_map_put_resource(tracker->cbt_map);
        tracker->cbt_map = NULL;
    }

    return result;
}


int tracker_remove(tracker_t* tracker)
{
    int result = _tracker_remove( tracker );

    container_sl_free( &tracker->content );

    return result;
}

int tracker_remove_all(void )
{
    int result = SUCCESS;
    int status;
    content_sl_t* content = NULL;

    log_tr("Removing all devices from tracking");

    while (NULL != (content = container_sl_get_first( &trackers_container ))){
        tracker_t* tracker = (tracker_t*)content;

        status = _tracker_remove( tracker );
        if (status != SUCCESS)
            log_err_format( "Failed to remove device [%d:%d] from tracking. errno=%d",
                MAJOR( tracker->original_dev_id ), MINOR( tracker->original_dev_id ), 0 - status );

        content_sl_free( content );
        content = NULL;
    }

    return result;
}

void tracker_cbt_bitmap_set( tracker_t* tracker, sector_t sector, sector_t sector_cnt )
{
    if (tracker->cbt_map == NULL)
        return;

    if (tracker->cbt_map->device_capacity != blk_dev_get_capacity(tracker->target_dev)){
        log_warn("Device resize detected");
        tracker->cbt_map->active = false;
        return;
    }

    if (SUCCESS != cbt_map_set(tracker->cbt_map, sector, sector_cnt)){ //cbt corrupt
        log_warn( "CBT fault detected" );
        tracker->cbt_map->active = false;
        return;
    }
}

bool tracker_cbt_bitmap_lock( tracker_t* tracker )
{
    bool result = false;
    if (tracker->cbt_map){
        cbt_map_read_lock( tracker->cbt_map );

        if (tracker->cbt_map->active){
            result = true;
        }
        else
            cbt_map_read_unlock( tracker->cbt_map );
    }
    return result;
}

void tracker_cbt_bitmap_unlock( tracker_t* tracker )
{
    if (tracker->cbt_map)
        cbt_map_read_unlock( tracker->cbt_map );
}

int _tracker_capture_snapshot( tracker_t* tracker )
{
    int result;
    defer_io_t* defer_io = NULL;

    result = defer_io_create( tracker->original_dev_id, tracker->target_dev, &defer_io );
    if (result != SUCCESS){
        log_err( "Failed to create defer IO processor" );
        return result;
    }

#ifdef HAVE_BLK_INTERPOSER
    blk_disk_freeze(tracker->target_dev->bd_disk);
#endif

    tracker->defer_io = defer_io_get_resource( defer_io );

    atomic_set( &tracker->is_captured, true );

    if (tracker->cbt_map != NULL){
        cbt_map_write_lock( tracker->cbt_map );
        cbt_map_switch( tracker->cbt_map );
        cbt_map_write_unlock( tracker->cbt_map );

        log_tr_format( "Snapshot captured for device [%d:%d]. New snap number %ld",
            MAJOR( tracker->original_dev_id ), MINOR( tracker->original_dev_id ), tracker->cbt_map->snap_number_active );
    }

#ifdef HAVE_BLK_INTERPOSER
    blk_disk_unfreeze(tracker->target_dev->bd_disk);
#endif

    return 0;

}

int tracker_capture_snapshot( snapshot_t* snapshot )
{
    int result = SUCCESS;
    int inx = 0;

    for (inx = 0; inx < snapshot->dev_id_set_size; ++inx){
#ifdef VEEAMSNAP_BLK_FREEZE
        struct super_block* superblock = NULL;
#endif
        tracker_t* tracker = NULL;
        dev_t dev_id = snapshot->dev_id_set[inx];

        result = tracker_find_by_dev_id( dev_id, &tracker );
        if (result != SUCCESS){
            log_err_dev_t( "Unable to capture snapshot: cannot find device ", dev_id );
            break;
        }


        if (tracker->is_unfreezable)
            down_write(&tracker->unfreezable_lock);
        else {
#ifdef VEEAMSNAP_BLK_FREEZE
            blk_freeze_bdev(tracker->original_dev_id, tracker->target_dev, &superblock);
#else
            result = freeze_bdev(tracker->target_dev);
            if (result != SUCCESS){
                log_err_dev_t("Unable to capture snapshot: cannot freeze device ", dev_id );
                break;
            }
#endif
        }

        result = _tracker_capture_snapshot( tracker );
        if (result != SUCCESS)
            log_err_dev_t( "Failed to capture snapshot for device ", dev_id );

        if (tracker->is_unfreezable)
            up_write(&tracker->unfreezable_lock);
        else {
#ifdef VEEAMSNAP_BLK_FREEZE
            superblock = blk_thaw_bdev(tracker->original_dev_id, tracker->target_dev, superblock);
#else
            result = thaw_bdev(tracker->target_dev);
            if (result != SUCCESS) {
                log_err("Unable to capture snapshot: failed to thaw block device");
                break;
            }
#endif
        }
    }
    if (result != SUCCESS)
        return result;

    for (inx = 0; inx < snapshot->dev_id_set_size; ++inx){
        tracker_t* p_tracker = NULL;
        dev_t dev_id = snapshot->dev_id_set[inx];

        result = tracker_find_by_dev_id( dev_id, &p_tracker );
        if (result != SUCCESS){
            log_err_dev_t("Unable to capture snapshot: cannot find device ", dev_id);
            continue;
        }

        if (snapstore_device_is_corrupted( p_tracker->defer_io->snapstore_device )){
            log_err_format( "Unable to freeze devices [%d:%d]: snapshot data is corrupted", MAJOR(dev_id), MINOR(dev_id) );
            result = -EDEADLK;
            break;
        }
    }

    if (result != SUCCESS){
        int status;
        log_err_d( "Failed to capture snapshot. errno=", result );

        status = tracker_release_snapshot( snapshot );
        if (status != SUCCESS)
            log_err_d( "Failed to perfrom snapshot cleanup. errno= ", status );
    }
    return result;
}


int _tracker_release_snapshot( tracker_t* tracker )
{
    int result = SUCCESS;
#ifdef VEEAMSNAP_BLK_FREEZE
    struct super_block* superblock = NULL;
#endif
    defer_io_t* defer_io = tracker->defer_io;

    if (!tracker->defer_io) {
        log_err_dev_t( "Tracker is already released for device ", tracker->original_dev_id);
        return SUCCESS;
    }

    if (tracker->is_unfreezable)
        down_write(&tracker->unfreezable_lock);
    else {
#ifdef VEEAMSNAP_BLK_FREEZE
        result = blk_freeze_bdev(tracker->original_dev_id, tracker->target_dev, &superblock);
#else
        result = freeze_bdev(tracker->target_dev);
#endif
    }

#ifdef HAVE_BLK_INTERPOSER
    blk_disk_freeze(tracker->target_dev->bd_disk);
#endif

    //clear freeze flag
    atomic_set(&tracker->is_captured, false);

    tracker->defer_io = NULL;

#ifdef HAVE_BLK_INTERPOSER
    blk_disk_unfreeze(tracker->target_dev->bd_disk);
#endif

    if (tracker->is_unfreezable)
        up_write(&tracker->unfreezable_lock);
    else {
#ifdef VEEAMSNAP_BLK_FREEZE
        superblock = blk_thaw_bdev(tracker->original_dev_id, tracker->target_dev, superblock);
#else
        if (!result)
            thaw_bdev(tracker->target_dev);
#endif
    }

    defer_io_stop(defer_io);
    defer_io_put_resource(defer_io);

    return result;
}


int tracker_release_snapshot( snapshot_t* snapshot )
{
    int result = SUCCESS;
    int inx = 0;
    log_tr_format( "Release snapshot [0x%llx]", snapshot->id );

    for (; inx < snapshot->dev_id_set_size; ++inx){
        int status;
        tracker_t* p_tracker = NULL;
        dev_t dev = snapshot->dev_id_set[inx];

        status = tracker_find_by_dev_id( dev, &p_tracker );
        if (status == SUCCESS){
            status = _tracker_release_snapshot( p_tracker );
            if (status != SUCCESS){
                log_err_dev_t( "Failed to release snapshot for device ", dev );
                result = status;
                break;
            }
        }
        else
            log_err_dev_t( "Unable to release snapshot: cannot find tracker for device ", dev );
    }

    return result;
}


void tracker_print_state( void )
{
    size_t sz;
    tracker_t** trackers;
    int tracksers_cnt = 0;

    log_tr("");
    log_tr("Trackers state:");

    tracksers_cnt = container_sl_length( &trackers_container );
    if (tracksers_cnt == 0){
        log_tr("Trackers not found.");
        return;
    }

    sz = tracksers_cnt * sizeof( tracker_t* );
    trackers = dbg_kzalloc( sz, GFP_KERNEL );
    if (trackers == NULL){
        log_err_sz( "Failed to allocate buffer for trackers. Size=", sz );
        return;
    }

    do{
        size_t inx = 0;
        content_sl_t* pContent = NULL;
        CONTAINER_SL_FOREACH_BEGIN( trackers_container, pContent )
        {
            tracker_t* tracker = (tracker_t*)pContent;

            trackers[inx] = tracker;
            inx++;
            if (inx >= tracksers_cnt)
                break;
        }
        CONTAINER_SL_FOREACH_END( trackers_container );

        for (inx = 0; inx < tracksers_cnt; ++inx){

            if (NULL != trackers[inx]){
                log_tr_dev_t( "original device ", trackers[inx]->original_dev_id );
                if (trackers[inx]->defer_io)
                    defer_io_print_state( trackers[inx]->defer_io );
                if (trackers[inx]->cbt_map)
                    cbt_print_state(trackers[inx]->cbt_map);
            }
        }
    } while (false);

    dbg_kfree( trackers );
}

void tracker_snapshot_id_set(tracker_t* tracker, unsigned long long snapshot_id)
{
    tracker->snapshot_id = snapshot_id;
}

unsigned long long tracker_snapshot_id_get(tracker_t* tracker)
{
    return tracker->snapshot_id;
}
