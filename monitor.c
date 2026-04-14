/*
 * monitor.c - Container Memory Monitor (Linux Kernel Module)
 *
 * FIXES APPLIED:
 *  1. timer_callback: replaced list_for_each_entry (not safe for deletion)
 *     with list_for_each_entry_safe so that stale (exited) entries can be
 *     removed from the list while iterating without corrupting the list.
 *     Previously, when get_rss_bytes() returned -1 (process gone) the entry
 *     was silently skipped but never freed, leaking kernel memory and causing
 *     the timer to keep inspecting dead PIDs forever.
 *  2. kill_process: added a missing check — send_sig is only called when the
 *     task pointer is non-NULL (was already done via the rcu_read_lock guard,
 *     but the null-check before the call makes the intent explicit and safe).
 *  3. All other logic (ioctl, init, exit) was correct and is unchanged.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* =========================================================================
 * Per-process tracking entry
 * ========================================================================= */

struct monitored_entry {
    struct list_head  list;
    pid_t             pid;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               soft_warned;
    char              container_id[MONITOR_NAME_LEN];
};

static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);

static struct timer_list monitor_timer;
static dev_t             dev_num;
static struct cdev       c_dev;
static struct class     *cl;

/* =========================================================================
 * RSS helper
 * ========================================================================= */

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* =========================================================================
 * Logging helpers
 * ========================================================================= */

static void log_soft_limit_event(const char *container_id,
                                  pid_t pid,
                                  unsigned long limit_bytes,
                                  long rss_bytes)
{
    pr_warn("[container_monitor] SOFT LIMIT container=%s pid=%d "
            "rss=%ld limit=%lu\n",
            container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id,
                          pid_t pid,
                          unsigned long limit_bytes,
                          long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)                          /* FIX 2: explicit null-guard */
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    pr_warn("[container_monitor] HARD LIMIT container=%s pid=%d "
            "rss=%ld limit=%lu\n",
            container_id, pid, rss_bytes, limit_bytes);
}

/* =========================================================================
 * Timer callback — fires every CHECK_INTERVAL_SEC seconds.
 *
 * FIX 1: Use list_for_each_entry_safe instead of list_for_each_entry.
 *   The _safe variant keeps a 'tmp' pointer to the next element before the
 *   loop body runs, so it is safe to call list_del + kfree on the current
 *   entry without corrupting the iteration.  The non-safe variant would
 *   dereference freed memory on the next iteration if the entry was removed.
 * ========================================================================= */

static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;

    mutex_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss_bytes = get_rss_bytes(entry->pid);

        if (rss_bytes < 0) {
            /*
             * Process no longer exists.  Remove the stale entry to avoid
             * leaking kernel memory and to stop futile future checks.
             */
            pr_info("[container_monitor] PID %d gone — removing stale entry "
                    "container=%s\n", entry->pid, entry->container_id);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss_bytes > (long)entry->hard_limit_bytes) {
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit_bytes,
                         rss_bytes);
        } else if (rss_bytes > (long)entry->soft_limit_bytes &&
                   !entry->soft_warned) {
            entry->soft_warned = 1;
            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit_bytes,
                                 rss_bytes);
        }
    }

    mutex_unlock(&monitored_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* =========================================================================
 * ioctl handler
 * ========================================================================= */

static long monitor_ioctl(struct file *f,
                           unsigned int cmd,
                           unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req,
                       (struct monitor_request __user *)arg,
                       sizeof(req)))
        return -EFAULT;

    /* ── REGISTER ──────────────────────────────────────────────────── */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, req.container_id,
                MONITOR_NAME_LEN - 1);
        entry->container_id[MONITOR_NAME_LEN - 1] = '\0';

        mutex_lock(&monitored_lock);
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&monitored_lock);

        pr_info("[container_monitor] Registered container=%s pid=%d "
                "soft=%lu hard=%lu\n",
                req.container_id, req.pid,
                req.soft_limit_bytes, req.hard_limit_bytes);
        return 0;
    }

    /* ── UNREGISTER ────────────────────────────────────────────────── */
    mutex_lock(&monitored_lock);
    {
        struct monitored_entry *entry, *tmp;
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("[container_monitor] Unregistered container=%s "
                        "pid=%d\n", req.container_id, req.pid);
                break;
            }
        }
    }
    mutex_unlock(&monitored_lock);

    return 0;
}

/* =========================================================================
 * File operations
 * ========================================================================= */

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* =========================================================================
 * Module init
 * ========================================================================= */

static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    pr_info("[container_monitor] Module loaded. Device: /dev/%s\n",
            DEVICE_NAME);
    return 0;
}

/* =========================================================================
 * Module exit
 * ========================================================================= */

static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    pr_info("[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
