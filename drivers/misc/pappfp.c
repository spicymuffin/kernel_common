
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/slab.h>

#include <uapi/linux/pappfp.h>

#include <linux/per_app.h>

static inline enum home_type uapi_to_kernel_home_type(u32 t)
{
	switch (t) {
	case PAPPFP_HOME_CE:
		return HOME_CE;
	case PAPPFP_HOME_DE:
		return HOME_DE;
	default:
		return HOME_NR;
	}
}

// maybe later we need to make sure that the caller is allowed to do this (like inside zygote)
static bool pappfp_allowed(void)
{
	return true;
}

static long pappfp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case PAPPFP_IOC_SET_HOME: {
		struct pappfp_ioc_set_home req;
		struct per_app *app;
		enum home_type type;
		int err;

		if (!pappfp_allowed())
			return -EPERM;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		req.path[PAPPFP_PATH_MAX - 1] = '\0';

		// convert to kernel enum
		type = uapi_to_kernel_home_type(req.type);

		// out of range
		if (type >= HOME_NR)
			return -EINVAL;

		// get current task's per_app struct
		app = per_app_get_current();
		if (!app)
			return -ESRCH;

		err = per_app_home_entry_set(app, type, req.path);
#ifdef CONFIG_PAPP_USE_KREF
		per_app_put(app);
#endif
		return err;
	}

	case PAPPFP_IOC_CLEAR_HOME: {
		struct pappfp_ioc_clear_home req;
		struct per_app *app;
		enum home_type type;

		if (!pappfp_allowed())
			return -EPERM;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		type = uapi_to_kernel_home_type(req.type);
		if (type >= HOME_NR)
			return -EINVAL;

		app = per_app_get_current();
		if (!app)
			return -ESRCH;

		per_app_home_entry_clear(app, type);
#ifdef CONFIG_PAPP_USE_KREF
		per_app_put(app);
#endif
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations pappfp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = pappfp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = pappfp_ioctl,
#endif
};

static struct miscdevice pappfp_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pappfp",
	.fops = &pappfp_fops,
	.mode = 0600,
};

static int __init pappfp_init(void)
{
	int ret = misc_register(&pappfp_miscdev);
	if (!ret)
		pr_info("registered /dev/%s\n", pappfp_miscdev.name);
	return ret;
}
module_init(pappfp_init);

static void __exit pappfp_exit(void)
{
	misc_deregister(&pappfp_miscdev);
}
module_exit(pappfp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("per app filepage control");
MODULE_AUTHOR("babaika");
