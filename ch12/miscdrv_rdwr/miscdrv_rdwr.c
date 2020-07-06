/*
 * ch12/miscdrv_rdwr/miscdrv_rdwr.c
 ***************************************************************
 * This program is part of the source code released for the book
 *  "Linux Kernel Development Cookbook"
 *  (c) Author: Kaiwan N Billimoria
 *  Publisher:  Packt
 *  GitHub repository:
 *  https://github.com/PacktPublishing/Linux-Kernel-Development-Cookbook
 *
 * From: Ch 12 : Writing a Simple Misc Character Device Driver
 ****************************************************************
 * Brief Description:
 * This driver is built upon our previous 'skeleton' ../miscdrv/ miscellaneous
 * driver. The key difference: we use a few global data items throughout; to do
 * so, we introduce the notion of a 'driver context' data structure. On init,
 * we allocate memory to it on init and initialize it; one of the members
 * within is a so-called secret (the 'oursecret' member along with some fake
 * statistics and config words).
 * So, when a userpace process (or thread) opens the device file and issues a
 * read upon it, we pass back the 'secret' to it. When it writes data to us,
 * we consider that data to be the new 'secret' and update it here (in memory).
 *
 * For details, please refer the book, Ch 12.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>		// k[m|z]alloc(), k[z]free(), ...
#include <linux/mm.h>		// kvmalloc()
#include <linux/fs.h>		// the fops

// copy_[to|from]_user()
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 11, 0)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif

#include "../../convenient.h"

#define OURMODNAME   "miscdrv_rdwr"
MODULE_AUTHOR("Kaiwan N Billimoria");
MODULE_DESCRIPTION("LKDC book:ch12/miscdrv_rdwr: simple misc char driver with"
		   " a 'secret' to read/write");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

static int ga, gb = 1;		/* ignore for now ... */

static struct device *dev;	/* device pointer */
/* The driver 'context' data structure;
 * all relevant 'state info' reg the driver is here.
 */
struct drv_ctx {
	int tx, rx, err, myword;
	u32 config1, config2;
	u64 config3;
#define MAXBYTES    128		/* Must match the userspace app; we should actually
				 * use a common header file for things like this */
	char oursecret[MAXBYTES];
};
static struct drv_ctx *ctx;

/*--- The driver 'methods' follow ---*/
/*
 * open_miscdrv_rdwr()
 * The driver's open 'method'; this 'hook' will get invoked by the kernel VFS
 * when the device file is opened. Here, we simply print out some relevant info.
 * The POSIX standard requires open() to return the file descriptor in success;
 * note, though, that this is done within the kernel VFS (when we return). So,
 * all we do here is return 0 indicating success.
 */
static int open_miscdrv_rdwr(struct inode *inode, struct file *filp)
{
	PRINT_CTX();		// displays process (or intr) context info

	ga++;
	gb--;
	pr_info("%s:%s():\n"
		" filename: \"%s\"\n"
		" wrt open file: f_flags = 0x%x\n"
		" ga = %d, gb = %d\n",
		OURMODNAME, __func__, filp->f_path.dentry->d_iname,
		filp->f_flags, ga, gb);

	return 0;
}

/*
 * read_miscdrv_rdwr()
 * The driver's read 'method'; it has effectively 'taken over' the read syscall
 * functionality!
 * The POSIX standard requires that the read() and write() system calls return
 * the number of bytes read or written on success, 0 on EOF and -1 (-ve errno)
 * on failure; here, we copy the 'secret' from our driver context structure
 * to the userspace app.
 */
static ssize_t read_miscdrv_rdwr(struct file *filp, char __user * ubuf,
				 size_t count, loff_t * off)
{
	int ret = count, secret_len = strnlen(ctx->oursecret, MAXBYTES);

	PRINT_CTX();
	pr_info("%s:%s():\n %s wants to read (upto) %zu bytes\n",
		OURMODNAME, __func__, current->comm, count);

	ret = -EINVAL;
	if (count < MAXBYTES) {
		pr_warn("%s:%s(): request # of bytes (%zu) is < required size"
			" (%d), aborting read\n",
			OURMODNAME, __func__, count, MAXBYTES);
		goto out_notok;
	}
	if (secret_len <= 0) {
		pr_warn("%s:%s(): whoops, something's wrong, the 'secret' isn't"
			" available..; aborting read\n", OURMODNAME, __func__);
		goto out_notok;
	}

	/* In a 'real' driver, we would now actually read the content of the
	 * device hardware (or whatever) into the user supplied buffer 'ubuf'
	 * for 'count' bytes, and then copy it to the userspace process (via
	 * the copy_to_user() routine).
	 * (FYI, the copy_to_user() routine is the *right* way to copy data from
	 * userspace to kernel-space; the parameters are:
	 *  'to-buffer', 'from-buffer', count
	 *  Returns 0 on success, i.e., non-zero return implies an I/O fault).
	 * Here, we simply copy the content of our context structure's 'secret'
	 * member to userspace.
	 */
	ret = -EFAULT;
	if (copy_to_user(ubuf, ctx->oursecret, secret_len)) {
		pr_warn("%s:%s(): copy_to_user() failed\n", OURMODNAME,
			__func__);
		goto out_notok;
	}
	ret = secret_len;

	// Update stats
	ctx->tx += secret_len;	// our 'transmit' is wrt this driver
	pr_info(" %d bytes read, returning... (stats: tx=%d, rx=%d)\n",
		secret_len, ctx->tx, ctx->rx);
 out_notok:
	return ret;
}

/*
 * write_miscdrv_rdwr()
 * The driver's write 'method'; it has effectively 'taken over' the write syscall
 * functionality!
 * The POSIX standard requires that the read() and write() system calls return
 * the number of bytes read or written on success, 0 on EOF and -1 (-ve errno)
 * on failure; Here, we accept the string passed to us and update our 'secret'
 * value to it.
 */
static ssize_t write_miscdrv_rdwr(struct file *filp, const char __user * ubuf,
				  size_t count, loff_t * off)
{
	int ret = count;
	void *kbuf = NULL;

	PRINT_CTX();
	if (unlikely(count > MAXBYTES)) {	/* paranoia */
		pr_warn("%s:%s(): count %zu exceeds max # of bytes allowed, "
			"aborting write\n", OURMODNAME, __func__, count);
		goto out_nomem;
	}
	pr_info("%s:%s():\n %s wants to write %zu bytes\n",
		OURMODNAME, __func__, current->comm, count);

	ret = -ENOMEM;
	kbuf = kvmalloc(count, GFP_KERNEL);
	if (unlikely(!kbuf)) {
		pr_warn("%s:%s(): kvmalloc() failed!\n", OURMODNAME, __func__);
		goto out_nomem;
	}
	memset(kbuf, 0, count);

	/* Copy in the user supplied buffer 'ubuf' - the data content to write -
	 * via the copy_from_user() macro.
	 * (FYI, the copy_from_user() macro is the *right* way to copy data from
	 * kernel-space to userspace; the parameters are:
	 *  'to-buffer', 'from-buffer', count
	 *  Returns 0 on success, i.e., non-zero return implies an I/O fault).
	 */
	ret = -EFAULT;
	if (copy_from_user(kbuf, ubuf, count)) {
		pr_warn("%s:%s(): copy_from_user() failed\n", OURMODNAME,
			__func__);
		goto out_cfu;
	}

	/* In a 'real' driver, we would now actually write (for 'count' bytes)
	 * the content of the 'ubuf' buffer to the device hardware (or whatever),
	 * and then return.
	 * Here, we do nothing, we just pretend we've done everything :-)
	 */
	strlcpy(ctx->oursecret, kbuf, (count > MAXBYTES ? MAXBYTES : count));
#if 0
	/* Might be useful to actually see a hex dump of the driver 'context' */
	print_hex_dump_bytes("ctx ", DUMP_PREFIX_OFFSET,
			     ctx, sizeof(struct drv_ctx));
#endif
	// Update stats
	ctx->rx += count;	// our 'receive' is wrt this driver

	ret = count;
	pr_info(" %zu bytes written, returning... (stats: tx=%d, rx=%d)\n",
		count, ctx->tx, ctx->rx);

 out_cfu:
	kvfree(kbuf);
 out_nomem:
	return ret;
}

/*
 * close_miscdrv_rdwr()
 * The driver's close 'method'; this 'hook' will get invoked by the kernel VFS
 * when the device file is closed (technically, when the file ref count drops
 * to 0). Here, we simply print out some info, and return 0 indicating success.
 */
static int close_miscdrv_rdwr(struct inode *inode, struct file *filp)
{
	PRINT_CTX();		// displays process (or intr) context info

	ga--;
	gb++;
	pr_info("%s:%s(): filename: \"%s\"\n"
		" ga = %d, gb = %d\n",
		OURMODNAME, __func__, filp->f_path.dentry->d_iname, ga, gb);
	return 0;
}

/* The driver 'functionality' is encoded via the fops */
static const struct file_operations llkd_misc_fops = {
	.open = open_miscdrv_rdwr,
	.read = read_miscdrv_rdwr,
	.write = write_miscdrv_rdwr,
	.llseek = no_llseek,	// dummy, we don't support lseek(2)
	.release = close_miscdrv_rdwr,
	/* As you learn more reg device drivers, you'll realize that the
	 * ioctl() would be a very useful method here. As an exercise,
	 * implement an ioctl method; when issued with the 'GETSTATS' 'command',
	 * it should return the statistics (tx, rx, errors) to the calling app
	 */
};

static struct miscdevice llkd_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,	/* kernel dynamically assigns a free minor# */
	.name = "llkd_miscdrv_rdwr",	/* when misc_register() is invoked, the kernel
					   will auto-create device file as /dev/llkd_miscdrv_rdwr;
					   also populated within /sys/class/misc/ and /sys/devices/virtual/misc/ */
	.mode = 0666,		/* ... dev node perms set as specified here */
	.fops = &llkd_misc_fops,	/* connect to this driver's 'functionality' */
};

static int __init miscdrv_init(void)
{
	int ret = 0;

	ret = misc_register(&llkd_miscdev);
	if (ret) {
		pr_notice("%s: misc device registration failed, aborting\n",
			  OURMODNAME);
		return ret;
	}
	/* Retrieve the device pointer for this device */
	dev = llkd_miscdev.this_device;
	pr_info("%s: LLKD misc driver (major # 10) registered, minor# = %d,"
		" dev node is /dev/llkd_miscdrv_rdwr\n",
		OURMODNAME, llkd_miscdev.minor);

	/* A 'managed' kzalloc(): use the 'devres' API devm_kzalloc() for mem
	 * alloc; why? as the underlying kernel devres framework will take care of
	 * freeing the memory automatically upon driver 'detach' or when the driver
	 * is unloaded from memory
	 */
	ctx = devm_kzalloc(dev, sizeof(struct drv_ctx), GFP_KERNEL);
	if (unlikely(!ctx)) {
		pr_notice("%s: kzalloc failed! aborting\n", OURMODNAME);
		return -ENOMEM;
	}
	strlcpy(ctx->oursecret, "initmsg", 8);
	dev_dbg(dev,
		"A sample print via the dev_dbg(): driver %s initialized\n",
		OURMODNAME);

	return 0;		/* success */
}

static void __exit miscdrv_exit(void)
{
	//kzfree(ctx);
	misc_deregister(&llkd_miscdev);
	pr_info("%s: LKDC misc driver deregistered, bye\n", OURMODNAME);
	dev_dbg(dev,
		"A sample print via the dev_dbg(): driver %s deregistered, bye\n",
		OURMODNAME);
}

module_init(miscdrv_init);
module_exit(miscdrv_exit);