#ifndef _LINUX_KSORTD_H
#define _LINUX_KSORTD_H

#include <linux/types.h>

/*
 * ksortd work queue interface
 *
 * This interface allows other subsystems to request ksortd to scan
 * a process's pages to identify hot/cold pages.
 */

/**
 * ksortd_queue_work - Queue a PID for ksortd scanning
 * @pid: Process ID to scan
 *
 * This function queues a work request for ksortd to scan the pages
 * of the specified process. The work is queued asynchronously and
 * ksortd will process it when idle.
 *
 * Returns: 0 on success, negative error code on failure
 */
int ksortd_queue_work(pid_t pid);

#endif /* _LINUX_KSORTD_H */
