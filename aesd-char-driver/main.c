/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>      // file_operations
#include <linux/slab.h>    // kmalloc / kfree / krealloc
#include <linux/uaccess.h> // copy_from_user / copy_to_user
#include <linux/string.h>  // memcpy
#include <linux/device.h>  // class_create / device_create
#include <linux/version.h> // LINUX_VERSION_CODE
#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("HaVanDuc2002");
MODULE_LICENSE("Dual BSD/GPL");

static struct class  *aesd_class  = NULL;
static struct device *aesd_device_node = NULL;

struct aesd_dev *aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    aesd_device = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = aesd_device;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_byte_offset = 0;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->circular_buffer, (size_t)*f_pos, &entry_byte_offset);
    if (!entry)
        goto out; /* EOF – nothing to read */

    bytes_to_copy = entry->size - entry_byte_offset;
    if (bytes_to_copy > count)
        bytes_to_copy = count;

    if (copy_to_user(buf, entry->buffptr + entry_byte_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *tmp;
    ssize_t retval = -ENOMEM;
    size_t newline_pos;
    bool has_newline = false;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Grow the partial buffer and append incoming data */
    tmp = krealloc(dev->partial_buf, dev->partial_size + count, GFP_KERNEL);
    if (!tmp)
        goto out;
    dev->partial_buf = tmp;

    if (copy_from_user(dev->partial_buf + dev->partial_size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    dev->partial_size += count;

    /* Search for \n in the newly extended partial buffer */
    for (newline_pos = 0; newline_pos < dev->partial_size; newline_pos++) {
        if (dev->partial_buf[newline_pos] == '\n') {
            has_newline = true;
            break;
        }
    }

    if (has_newline) {
        struct aesd_buffer_entry new_entry;
        /* The entry length includes the \n */
        new_entry.size   = newline_pos + 1;
        new_entry.buffptr = dev->partial_buf;

        /* If buffer is full the oldest entry will be overwritten – free it first */
        if (dev->circular_buffer.full)
            kfree(dev->circular_buffer.entry[dev->circular_buffer.in_offs].buffptr);

        aesd_circular_buffer_add_entry(&dev->circular_buffer, &new_entry);

        /* Reset partial buffer (any bytes after \n are discarded per spec) */
        dev->partial_buf  = NULL;
        dev->partial_size = 0;
    }

    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}
loff_t aesd_seek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t total_size = 0;
    loff_t result;
    uint8_t i;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    for (i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
        total_size += dev->circular_buffer.entry[i].size;
    }

    result = fixed_size_llseek(filp, offset, whence, total_size);

    mutex_unlock(&dev->lock);
    return result;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    uint32_t i;
    loff_t newpos = 0;
    uint8_t num_entries;
    uint8_t index;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
            return -EFAULT;

        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;

        if (dev->circular_buffer.full)
            num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        else
            num_entries = (dev->circular_buffer.in_offs
                           + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                           - dev->circular_buffer.out_offs)
                          % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        if (seekto.write_cmd >= num_entries) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }

        index = (dev->circular_buffer.out_offs + seekto.write_cmd)
                % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        if (seekto.write_cmd_offset >= dev->circular_buffer.entry[index].size) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }

        for (i = 0; i < seekto.write_cmd; i++) {
            uint8_t idx = (dev->circular_buffer.out_offs + i)
                          % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
            newpos += dev->circular_buffer.entry[idx].size;
        }
        newpos += seekto.write_cmd_offset;

        filp->f_pos = newpos;
        mutex_unlock(&dev->lock);
        break;

    default:
        return -ENOTTY;
    }

    return 0;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_seek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    aesd_device = kzalloc(sizeof(struct aesd_dev), GFP_KERNEL);
    if (!aesd_device) {
        unregister_chrdev_region(dev, 1);
        return -ENOMEM;
    }

    mutex_init(&aesd_device->lock);
    aesd_circular_buffer_init(&aesd_device->circular_buffer);

    result = aesd_setup_cdev(aesd_device);

    if (result)
        goto err_cdev;

    /* Create a class and device so udev/mdev auto-creates /dev/aesdchar */
    /* class_create() API changed in kernel 6.4: THIS_MODULE arg was removed */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
    aesd_class = class_create(THIS_MODULE, "aesdchar");
#else
    aesd_class = class_create("aesdchar");
#endif
    if (IS_ERR(aesd_class)) {
        result = PTR_ERR(aesd_class);
        aesd_class = NULL;
        goto err_class;
    }

    aesd_device_node = device_create(aesd_class, NULL, MKDEV(aesd_major, aesd_minor),
                                     NULL, "aesdchar");
    if (IS_ERR(aesd_device_node)) {
        result = PTR_ERR(aesd_device_node);
        aesd_device_node = NULL;
        goto err_device;
    }

    return 0;

err_device:
    class_destroy(aesd_class);
    aesd_class = NULL;
err_class:
    cdev_del(&aesd_device->cdev);
err_cdev:
    unregister_chrdev_region(dev, 1);
    {
        uint8_t index;
        struct aesd_buffer_entry *entry;
        AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device->circular_buffer, index) {
            if (entry->buffptr)
                kfree(entry->buffptr);
        }
    }
    kfree(aesd_device->partial_buf);
    kfree(aesd_device);
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    /* Remove the udev/mdev device node and class */
    if (aesd_device_node)
        device_destroy(aesd_class, devno);
    if (aesd_class)
        class_destroy(aesd_class);

    cdev_del(&aesd_device->cdev);

    /* Free all entries stored in the circular buffer */
    {
        uint8_t index;
        struct aesd_buffer_entry *entry;
        AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device->circular_buffer, index) {
            if (entry->buffptr)
                kfree(entry->buffptr);
        }
    }
    /* Free any partial (unterminated) write that was in progress */
    kfree(aesd_device->partial_buf);
    /* Free the device structure itself */
    kfree(aesd_device);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
