#pragma once
#include <linux/ioctl.h>
#include <linux/types.h>

#define PAPPFP_IOC_MAGIC 'R'
#define PAPPFP_PATH_MAX 512

enum pappfp_home_type { PAPPFP_HOME_CE, PAPPFP_HOME_DE, PAPPFP_HOME_NR };

struct pappfp_ioc_set_home {
	__u32 type;
	char path[PAPPFP_PATH_MAX];
};

struct pappfp_ioc_clear_home {
	__u32 type;
};

#define PAPPFP_IOC_SET_HOME \
	_IOW(PAPPFP_IOC_MAGIC, 0x11, struct pappfp_ioc_set_home)
#define PAPPFP_IOC_CLEAR_HOME \
	_IOW(PAPPFP_IOC_MAGIC, 0x12, struct pappfp_ioc_clear_home)
