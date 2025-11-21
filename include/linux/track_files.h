/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TRACK_FILES_H
#define _LINUX_TRACK_FILES_H

#include <linux/fs.h>

#ifdef CONFIG_TRACK_FILES
bool should_track_current_task(void);
bool track_file_if_new(struct file *file);
#else
static inline bool should_track_current_task(void) { return false; }
static inline bool track_file_if_new(struct file *file) { return false; }
#endif

#endif /* _LINUX_TRACK_FILES_H */
