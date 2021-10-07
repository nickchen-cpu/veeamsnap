// Copyright (c) Veeam Software Group GmbH

#include "stdafx.h"
#include "version.h"
#include "veeamsnap_ioctl.h"
#include "ctrl_fops.h"
#include "ctrl_pipe.h"

#include "blk_direct.h"
#include "blk_redirect.h"
#include "blk_deferred.h"
#include "snapimage.h"
#include "snapdata_collect.h"

#include "snapstore.h"
#include "snapstore_device.h"

#include "snapshot.h"
#include "tracker_queue.h"
#include "tracker.h"
#include "tracking.h"
#include "sparse_bitmap.h"
#include "ctrl_sysfs.h"
#include "kernel_entries.h"

#define SECTION "main      "
#include "log_format.h"

#ifdef PERSISTENT_CBT
#include <linux/notifier.h>
#include "cbt_persistent.h"
#endif

#include <linux/reboot.h>       //use old methon
//#include <linux/syscore_ops.h>    //more modern method


#ifndef PERSISTENT_CBT
#pragma message "Persistent CBT is not supported for this system"
#endif

static int g_param_zerosnapdata = 0;
static int g_param_debuglogging = 0;
static char* g_logdir = NULL;
static unsigned long g_param_logmaxsize = 15*1024*1024;
#ifdef PERSISTENT_CBT
static char* g_cbtdata = NULL;
#endif
static int g_param_snapstore_block_size_pow = 14;
static int g_param_change_tracking_block_size_pow = 18;
static unsigned int g_param_fixflags = 0;

int get_debuglogging( void )
{
    return g_param_debuglogging;
}
int get_zerosnapdata( void )
{
    return g_param_zerosnapdata;
}
int get_snapstore_block_size_pow(void)
{
    return g_param_snapstore_block_size_pow;
}
int inc_snapstore_block_size_pow(void)
{
    if (g_param_snapstore_block_size_pow > 30)
        return -EFAULT;

    ++g_param_snapstore_block_size_pow;
    return SUCCESS;
}
int get_change_tracking_block_size_pow(void)
{
    return g_param_change_tracking_block_size_pow;
}

unsigned int get_fixflags(void)
{
    return g_param_fixflags;
}

static int veeamsnap_major = 0;

int get_veeamsnap_major(void)
{
    return veeamsnap_major;
}

#ifdef VEEAMSNAP_SYSFS_PARAMS
int set_params(char* param_name, char* param_value)
{
    int res = -EINVAL;

    if (0 == strcmp(param_name, "logdir")){
        char* old_value = g_logdir;
        char* new_value = NULL;
        size_t len = strlen(param_value);

        new_value = dbg_kzalloc(len, GFP_KERNEL);
        if (new_value){
            strcpy(new_value, param_value);
            g_logdir = new_value;
            dbg_kfree(old_value);
        }else
            res = -ENOMEM;
    }
    else if (0 == strcmp(param_name, "logmaxsize")) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
        {
            char* endptr = NULL;
            g_param_logmaxsize = simple_strtoul(param_value, &endptr, 10);
        }
#else
        res = kstrtoul(param_value, 10, &g_param_logmaxsize);
        if (SUCCESS != res) {
            log_err_s("Failed to parse: ", param_value);
            return res;
        }
#endif
#ifdef PERSISTENT_CBT
    else if (0 == strcmp(param_name, "cbtdata")){
        char* old_value = g_cbtdata;
        char* new_value = NULL;
        size_t len = strlen(param_value);

        new_value = dbg_kzalloc(len, GFP_KERNEL);
        if (new_value){
            strcpy(new_value, param_value);
            g_cbtdata = new_value;
            dbg_kfree(old_value);
        }else
            res = -ENOMEM;
    }
#endif
    else if (0 == strcmp(param_name, "snapstore_block_size_pow")){
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
        {
            char* endptr = NULL;
            g_param_snapstore_block_size_pow = simple_strtol(param_value, &endptr, 10);
        }
#else
        res = kstrtoint(param_value, 10, &g_param_snapstore_block_size_pow);
        if (SUCCESS != res){
            log_err_s("Failed to parse: ", param_value);
            return res;
        }
#endif
    }
    else if (0 == strcmp(param_name, "change_tracking_block_size_pow")){
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
        {
            char* endptr = NULL;
            g_param_change_tracking_block_size_pow = simple_strtol(param_value, &endptr, 10);
        }
#else
        res = kstrtoint(param_value, 10, &g_param_change_tracking_block_size_pow);
        if (SUCCESS != res){
            log_err_s("Failed to parse: ", param_value);
            return res;
        }
#endif
    }
    else
        res = -EINVAL;

    return res;
}

int get_params(char* buf)
{
    int res = SUCCESS;



    return res;
}
#endif //VEEAMSNAP_SYSFS_PARAMS

static struct device* veeamsnap_device = NULL;

static struct file_operations ctrl_fops = {
    .owner  = THIS_MODULE,
    .read   = ctrl_read,
    .write  = ctrl_write,
    .open   = ctrl_open,
    .release= ctrl_release,
    .poll   = ctrl_poll,
    //.ioctl  = ctrl_ioctl,
    .unlocked_ioctl = ctrl_unlocked_ioctl
};

static inline void show_distrib(const char* distrib_name)
{
    log_tr_format("Compile for distributive: %s", distrib_name);
}

static inline void show_distrib_version(const char* distrib_name)
{
#if defined(DISTRIB_VERSION_1) && defined(DISTRIB_VERSION_2)
    log_tr_format("Compile for distributive: %s %d.%d", distrib_name, DISTRIB_VERSION_1, DISTRIB_VERSION_2);
#else
#if defined(DISTRIB_VERSION_1)
    log_tr_format("Compile for distributive: %s %d", distrib_name, DISTRIB_VERSION_1);
#else
    show_distrib(distrib_name);
#endif
#endif
}


static void _cbt_syscore_shutdown(void)
{
    //stop logging thread. In this time it is not needed
    logging_done();

    {//stop tracking
        int result = tracker_remove_all();
        if (result != SUCCESS)
            log_err("Failed to remove all tracking devices from tracking");
    }
#ifdef PERSISTENT_CBT
    //store cbt maps - it`s shared resource.
    cbt_persistent_store();
#endif
}

#ifdef _LINUX_SYSCORE_OPS_H
/*
static int _cbt_syscore_suspend(void)
{
    //cbt_persistent_suspend();
    return 0;
}

static void _cbt_syscore_resume(void)
{
    // ??? really necessary ?
    return;
}*/


struct syscore_ops _cbt_syscore_ops = {
    .suspend = NULL/*_cbt_syscore_suspend*/,
    .resume = NULL/*_cbt_syscore_resume*/,
    .shutdown = _cbt_syscore_shutdown,
};
#endif

#ifdef _LINUX_REBOOT_H
static int veeamsnap_shutdown_notify(struct notifier_block *nb, unsigned long type, void *p)
{
    logging_mode_sys(); //switch logger to system log

    switch (type) {
    case SYS_HALT:
        log_tr("system halt");
        break;
    case SYS_POWER_OFF:
        log_tr("system power off");
        break;
    case SYS_DOWN:
    default:
        log_tr("system down");
        break;
    }
    logging_flush();

    _cbt_syscore_shutdown();

    return NOTIFY_DONE;
}

static struct notifier_block _veeamsnap_shutdown_nb = {
    .notifier_call = veeamsnap_shutdown_notify,
};
#endif


int __init veeamsnap_init(void)
{
    //int conteiner_cnt = 0;
    int result = SUCCESS;

#ifdef VEEAMSNAP_MEMORY_LEAK_CONTROL
    dbg_mem_init( );
#endif
    logging_init( g_logdir, g_param_logmaxsize );
    log_tr( "================================================================================" );
    log_tr( "Loading" );
    log_tr_s( "Version: ", FILEVER_STR );
    log_tr_s( "Author: ", AUTHOR_STR );
    log_tr_s( "licence: ", LICENCE_STR );
    log_tr_s( "description: ", DESCRIPTION_STR );

    log_tr_d("zerosnapdata: ", g_param_zerosnapdata);
    log_tr_d("debuglogging: ", g_param_debuglogging);
    log_tr_d("snapstore_block_size_pow: ", g_param_snapstore_block_size_pow);
    log_tr_d("change_tracking_block_size_pow: ", g_param_change_tracking_block_size_pow);
    log_tr_s("logdir: ", g_logdir);
    log_tr_ld("logmaxsize: ", g_param_logmaxsize);
#ifdef PERSISTENT_CBT
    log_tr_s("cbtdata: ", g_cbtdata);
#endif
    log_tr_x("fixflags: ", g_param_fixflags);
#if defined(VEEAMSNAP_DISK_SUBMIT_BIO)
    log_tr("The substitution of the disk's fops is used.");
#endif
    if (g_param_snapstore_block_size_pow > 23){
        g_param_snapstore_block_size_pow = 23;
        log_tr_d("Limited snapstore_block_size_pow: ", g_param_snapstore_block_size_pow);
    }
    else if (g_param_snapstore_block_size_pow < 12){
        g_param_snapstore_block_size_pow = 12;
        log_tr_d("Limited snapstore_block_size_pow: ", g_param_snapstore_block_size_pow);
    }

    if (g_param_change_tracking_block_size_pow > 23){
        g_param_change_tracking_block_size_pow = 23;
        log_tr_d("Limited change_tracking_block_size_pow: ", g_param_change_tracking_block_size_pow);
    }
    else if (g_param_change_tracking_block_size_pow < 12){
        g_param_change_tracking_block_size_pow = 12;
        log_tr_d("Limited change_tracking_block_size_pow: ", g_param_change_tracking_block_size_pow);
    }

#if defined(DISTRIB_NAME_RHEL)
    show_distrib_version("RHEL");
#endif
#if defined(DISTRIB_NAME_CENTOS)
    show_distrib_version("CentOS");
#endif
#if defined(DISTRIB_NAME_OL)
    show_distrib_version("Oracle");
#endif
#if defined(DISTRIB_NAME_FEDORA)
    show_distrib_version("Fedora");
#endif

#if defined(DISTRIB_NAME_SLES) || defined(DISTRIB_NAME_SLES_SAP)
    show_distrib_version("SLES");
#elif defined(DISTRIB_NAME_OPENSUSE) || defined(DISTRIB_NAME_OPENSUSE_LEAP)
    show_distrib_version("openSUSE");
#elif defined(DISTRIB_NAME_OPENSUSE_TUMBLEWEED)
    show_distrib_version("openSUSE Tumbleweed");
#elif defined(OS_RELEASE_SUSE)
    show_distrib_version("SLES");
#endif

#if defined(DISTRIB_NAME_DEBIAN)
    show_distrib_version("Debian");
#endif
#if defined(DISTRIB_NAME_UBUNTU)
    show_distrib_version("Ubuntu");
#endif

#ifdef SNAPIMAGE_TRACER
    log_tr("Snapshot image tracing is available");
#endif

    page_arrays_init( );

    ctrl_init();

    do{
        log_tr("Registering reboot notification");

#ifdef _LINUX_REBOOT_H
        result = register_reboot_notifier(&_veeamsnap_shutdown_nb); //always return SUCCESS
        if (result != SUCCESS){
            log_err_d("Failed to register reboot notification. Error code ", result);
            break;
        }
#endif
#ifdef _LINUX_SYSCORE_OPS_H
        register_syscore_ops(&_cbt_syscore_ops);
#endif


        veeamsnap_major = register_chrdev(0, MODULE_NAME, &ctrl_fops);
        if (veeamsnap_major < 0) {
            log_err_d("Failed to register a character device. errno=", veeamsnap_major);
            result = veeamsnap_major;
            break;
        }
        log_tr_format("Module major [%d]", veeamsnap_major);

        if ((result = blk_direct_bioset_create( )) != SUCCESS)
            break;
        if ((result = blk_redirect_bioset_create( )) != SUCCESS)
            break;

        blk_deferred_init( );

        if ((result = blk_deferred_bioset_create( )) != SUCCESS)
            break;

        if ((result = sparsebitmap_init( )) != SUCCESS)
            break;

        if ((result = tracker_init( )) != SUCCESS)
            break;

        if ((result = tracker_disk_init( )) != SUCCESS)
            break;

        if ((result = snapshot_Init( )) != SUCCESS)
            break;

        if ((result = snapstore_device_init( )) != SUCCESS)
            break;
        if ((result = snapstore_init( )) != SUCCESS)
            break;

        if ((result = snapdata_collect_Init( )) != SUCCESS)
            break;

        if ((result = snapimage_init( )) != SUCCESS)
            break;


#ifdef PERSISTENT_CBT
        {
            int cbt_persistent_result = cbt_persistent_init(g_cbtdata);
            if (cbt_persistent_result == SUCCESS) {
                cbt_persistent_load();
            }
            else if (cbt_persistent_result == ENODATA) {
                //do nothing
            }
            else {
                log_err("Failed to initialize persistent CBT");
            }
            //cbt_persistent_start_trackers();
        }
#endif

        if ((result = ctrl_sysfs_init(&veeamsnap_device)) != SUCCESS){
#if 0
			log_warn("Cannot initialize sysfs attributes");
			result = SUCCESS;
#else
			log_err("Failed to initialize sysfs attributes");
            break;
#endif
        }

    }while(false);
/*

    conteiner_cnt = container_alloc_counter( );
    log_tr_d( "container_alloc_counter=", conteiner_cnt );

    conteiner_cnt = container_sl_alloc_counter( );
    log_tr_d( "container_sl_alloc_counter=", conteiner_cnt );
*/

    return result;
}

void __exit veeamsnap_exit(void)
{
    int conteiner_cnt = 0;
    int result;
    log_tr("Unloading module");

/*
    conteiner_cnt = container_alloc_counter( );
    log_tr_d( "start. container_alloc_counter=", conteiner_cnt );
    conteiner_cnt = container_sl_alloc_counter( );
    log_tr_d( "start. container_sl_alloc_counter=", conteiner_cnt );*/


#ifdef VEEAMSNAP_MEMORY_LEAK_CONTROL
    //log_tr_d( "mem_cnt=", atomic_read( &g_mem_cnt ) );
    //log_tr_d( "vmem_cnt=", atomic_read( &g_vmem_cnt ) );
#endif


    log_tr("Unregistering reboot notification");
#ifdef _LINUX_SYSCORE_OPS_H
    unregister_syscore_ops(&_cbt_syscore_ops);
#endif

#ifdef _LINUX_REBOOT_H
    unregister_reboot_notifier(&_veeamsnap_shutdown_nb);
#endif

    ctrl_sysfs_done(&veeamsnap_device);

    result = snapshot_Done( );
    if (SUCCESS == result){

        snapdata_collect_Done( );

        snapstore_device_done( );
        snapstore_done( );

        result = tracker_done( );
        if (SUCCESS == result){
            result = tracker_disk_done( );

#ifdef PERSISTENT_CBT
            cbt_persistent_done();
#endif
        }
        snapimage_done( );

        sparsebitmap_done( );

        blk_deferred_bioset_free( );
        blk_deferred_done( );

        blk_redirect_bioset_free( );
        blk_direct_bioset_free( );
    }

    if (SUCCESS != result){
        log_tr_d( "Failed to unload. errno=", result );
        return;
    }

    unregister_chrdev(veeamsnap_major, MODULE_NAME);

    ctrl_done( );

    logging_done( );

    conteiner_cnt = container_alloc_counter( );
    if (conteiner_cnt != 0)
        log_err_d( "container_alloc_counter=", conteiner_cnt );

    conteiner_cnt = container_sl_alloc_counter( );
    if (conteiner_cnt != 0)
        log_err_d( "container_sl_alloc_counter=", conteiner_cnt );

#ifdef VEEAMSNAP_MEMORY_LEAK_CONTROL
    if (atomic_read( &g_mem_cnt ) != 0)
        log_err_d( "mem_cnt=", atomic_read( &g_mem_cnt ) );
#endif

}

module_init(veeamsnap_init);
module_exit(veeamsnap_exit);


module_param_named( zerosnapdata, g_param_zerosnapdata, int, 0644 );
MODULE_PARM_DESC( zerosnapdata, "Zeroing snapshot data algorithm determine." );

module_param_named( debuglogging, g_param_debuglogging, int, 0644 );
MODULE_PARM_DESC( debuglogging, "Logging level switch." );

module_param_named(logdir, g_logdir, charp, 0644);
MODULE_PARM_DESC( logdir, "Directory for module logs." );

module_param_named( logmaxsize, g_param_logmaxsize, ulong, 0644 );
MODULE_PARM_DESC( logmaxsize, "Maximum log file size." );

#ifdef PERSISTENT_CBT
module_param_named(cbtdata, g_cbtdata, charp, 0644);
MODULE_PARM_DESC(cbtdata, "Parameters for persistent CBT.");
#endif

module_param_named(snapstore_block_size_pow, g_param_snapstore_block_size_pow, int, 0644);
MODULE_PARM_DESC(snapstore_block_size_pow, "Snapstore block size binary pow. 20 for 1MiB block size");

module_param_named(change_tracking_block_size_pow, g_param_change_tracking_block_size_pow, int, 0644);
MODULE_PARM_DESC(change_tracking_block_size_pow, "Change-tracking block size binary pow. 18 for 256 KiB block size");

module_param_named(fixflags, g_param_fixflags, uint, 0644);
MODULE_PARM_DESC(fixflags, "Flags for known issues");

MODULE_LICENSE( LICENCE_STR );
MODULE_AUTHOR( AUTHOR_STR );

MODULE_DESCRIPTION( DESCRIPTION_STR );
MODULE_VERSION(FILEVER_STR);

MODULE_INFO( supported, "external" );
