#include <linux/kthread.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/freezer.h>
#include <linux/timer.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/rt.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/swapops.h>
#include <linux/pagewalk.h>
#include <linux/pgtable.h>
#include <linux/workqueue.h>
#include <linux/ksortd.h>
#include <linux/per_app.h>
#include <uapi/linux/sched/types.h>

/*
 * ksortd - Kernel thread for page access bit scanning
 *
 * PURPOSE:
 *   Scan a target process's pages to determine which are "hot" (accessed)
 *   and which are "cold" (not accessed) over a time interval.
 *
 * OPERATION:
 *   1. User writes PID to /proc/ksortd/pid
 *   2. Wait N seconds (configurable)
 *   3. CLEAR phase: Clear all accessed bits (in batches)
 *   4. Wait N seconds (configurable) - process runs, accesses pages
 *   5. CHECK phase: Check which pages have accessed bit set (in batches)
 *   6. Report statistics and return to idle
 *
 * LOCKING STRATEGY:
 *   This module uses a careful locking hierarchy to avoid deadlocks
 *   and to minimize lock contention:
 *
 *   1. ksortd_mutex: Protects scan_state (mm, address, phase)
 *      - Held briefly to read/update scan state
 *      - NEVER held while sleeping or doing actual page scanning
 *
 *   2. mmap_read_lock: Protects VMA list of target process
 *      - Acquired with trylock to avoid blocking
 *      - Held only during batch scanning (not during waits)
 *      - Released before cond_resched() between batches
 *
 *   3. page lock: Protects individual page metadata
 *      - Acquired with trylock - skip page if can't get lock
 *      - Held during rmap_walk for that page only
 *
 *   4. PTE spinlock (ptl): Protects page table entries
 *      - Held for microseconds while modifying one PTE
 *      - This is the only non-preemptible section
 *
 *   Reference counting:
 *   - mm_struct: mmget()/mmput() to keep mm alive during scan
 *   - struct page: follow_page(FOLL_GET)/put_page() for page reference
 *
 * PREEMPTION:
 *   - Thread CAN be preempted between pages (cond_resched() called)
 *   - Thread is non-preemptible only while holding PTE spinlock
 *   - PTE spinlock is held for ~microseconds (single PTE modification)
 *   - Compatible with SCHED_IDLE policy
 *
 * CONCURRENT MODIFICATIONS:
 *   - Target process may be running and modifying its address space
 *   - We handle this by:
 *     a) Using trylock variants (don't block if process is busy)
 *     b) Verifying page still exists after acquiring locks
 *     c) Checking PTE still points to expected page before modifying
 *     d) Small batch sizes to minimize time holding locks
 */

/* Scan modes */
enum ksortd_scan_mode {
	KSORTD_SCAN_CLEAR,   /* Phase 1: Clear accessed bits */
	KSORTD_SCAN_CHECK,   /* Phase 2: Check accessed bits */
};

/* Scan phases for the state machine */
enum ksortd_phase {
	KSORTD_PHASE_IDLE,           /* Waiting for target PID */
	KSORTD_PHASE_WAIT_CLEAR,     /* Waiting before CLEAR phase */
	KSORTD_PHASE_CLEARING,       /* Clearing accessed bits */
	KSORTD_PHASE_WAIT_CHECK,     /* Waiting before CHECK phase */
	KSORTD_PHASE_CHECKING,       /* Checking accessed bits */
	KSORTD_PHASE_COMPLETE,       /* Cycle complete, back to idle */
};

/* Configuration constants */
#define KSORTD_DEFAULT_SLEEP_MS      30000   /* seconds to sleep in between */
#define KSORTD_DEFAULT_PAGES_TO_SCAN 256
#define KSORTD_MIN_SLEEP_MS          100
#define KSORTD_MAX_SLEEP_MS          60000
#define KSORTD_MIN_PAGES             1
#define KSORTD_MAX_PAGES             10000
#define KSORTD_WORK_QUEUE_SIZE       64      /* Max pending work items */

/* Configuration variables */
static unsigned int ksortd_sleep_ms = KSORTD_DEFAULT_SLEEP_MS;
static unsigned int ksortd_thread_pages_to_scan = KSORTD_DEFAULT_PAGES_TO_SCAN;
static pid_t ksortd_target_pid;

/* Statistics - cumulative across all cycles */
static unsigned long ksortd_total_scanned;
static unsigned long ksortd_hot_pages;
static unsigned long ksortd_cold_pages;

static unsigned long ksortd_anon_scanned;
static unsigned long ksortd_hot_anon_pages;
static unsigned long ksortd_cold_anon_pages;

static unsigned long ksortd_file_scanned;
static unsigned long ksortd_hot_file_pages;
static unsigned long ksortd_cold_file_pages;

static unsigned long ksortd_shared_pages;    /* Skipped: shared (mapcount > 1) */
static unsigned long ksortd_skipped_pages;   /* Skipped: not present, locked, etc. */

/* Cycle statistics */
static unsigned long ksortd_clear_cycles;
static unsigned long ksortd_check_cycles;
static unsigned long ksortd_total_cycles;

/* Synchronization */
static DECLARE_WAIT_QUEUE_HEAD(ksortd_wait);
static struct task_struct *ksortd_thread;
static DEFINE_MUTEX(ksortd_mutex);

/* Work queue for processing PIDs */
struct ksortd_work_item {
	pid_t pid;
};

static struct ksortd_work_item ksortd_work_queue[KSORTD_WORK_QUEUE_SIZE];
static unsigned int ksortd_work_queue_head;  /* Next item to dequeue */
static unsigned int ksortd_work_queue_tail;  /* Next slot to enqueue */
static unsigned int ksortd_work_queue_count; /* Current number of items */
static DEFINE_SPINLOCK(ksortd_work_queue_lock);

/* Scan state - protected by ksortd_mutex */
struct ksortd_scan_state {
	struct per_app *app;             /* Target per_app struct */
	unsigned long scan_progress;     /* Pages scanned in current phase */
	enum ksortd_phase phase;         /* Current phase */
	bool phase_complete;             /* Current phase iteration complete */
};

static struct ksortd_scan_state scan_state;

/*
 * rmap walker argument - passed via rmap_walk_control.arg
 * This avoids per-CPU variable issues when thread migrates between CPUs
 */
struct rmap_check_arg {
	struct page *target_page;
	enum ksortd_scan_mode mode;
	bool accessed;
	unsigned int mapcount;
	unsigned int accessed_count;
};

/* ========== Forward Declarations ========== */
static void ksortd_reset_cycle_stats(void);
static enum ksortd_phase ksortd_get_phase(void);
static void ksortd_set_phase(enum ksortd_phase phase);
static bool ksortd_do_scan(enum ksortd_scan_mode mode);
static bool ksortd_wait_timeout(unsigned int ms, const char *reason);

/* ========== Work Queue Management ========== */

/*
 * Enqueue a PID for scanning
 * Returns 0 on success, -ENOSPC if queue is full
 */
static int ksortd_enqueue_work(pid_t pid)
{
	unsigned long flags;
	unsigned int i, idx;
	int ret = 0;

	if (pid == 0)
		return -EINVAL;

	spin_lock_irqsave(&ksortd_work_queue_lock, flags);

	/* Check if queue is full */
	if (ksortd_work_queue_count >= KSORTD_WORK_QUEUE_SIZE) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_warn("ksortd: Work queue is full, dropping PID %d\n", pid);
#endif
		ret = -ENOSPC;
		goto out;
	}

	/* Check if this PID is already in the queue */
	for (i = 0; i < ksortd_work_queue_count; i++) {
		idx = (ksortd_work_queue_head + i) % KSORTD_WORK_QUEUE_SIZE;
		if (ksortd_work_queue[idx].pid == pid) {
#ifdef CONFIG_DEBUG_KSORTD
			pr_debug("ksortd: PID %d already in queue, skipping\n", pid);
#endif
			ret = 0;  /* Not an error - already queued */
			goto out;
		}
	}

	/* Add to queue */
	ksortd_work_queue[ksortd_work_queue_tail].pid = pid;
	ksortd_work_queue_tail = (ksortd_work_queue_tail + 1) % KSORTD_WORK_QUEUE_SIZE;
	ksortd_work_queue_count++;

#ifdef CONFIG_DEBUG_KSORTD
	pr_info("ksortd: Enqueued PID %d (queue size: %u)\n", pid, ksortd_work_queue_count);
#endif
out:
	spin_unlock_irqrestore(&ksortd_work_queue_lock, flags);

	/* Wake up ksortd if we successfully enqueued work */
	if (ret == 0)
		wake_up_interruptible(&ksortd_wait);

	return ret;
}

/*
 * Dequeue a PID for scanning
 * Returns 0 if no work available
 */
static pid_t ksortd_dequeue_work(void)
{
	unsigned long flags;
	pid_t pid = 0;

	spin_lock_irqsave(&ksortd_work_queue_lock, flags);

	if (ksortd_work_queue_count > 0) {
		pid = ksortd_work_queue[ksortd_work_queue_head].pid;
		ksortd_work_queue_head = (ksortd_work_queue_head + 1) % KSORTD_WORK_QUEUE_SIZE;
		ksortd_work_queue_count--;
#ifdef CONFIG_DEBUG_KSORTD
		pr_info("ksortd: Dequeued PID %d (queue size: %u)\n", pid, ksortd_work_queue_count);
#endif
	}

	spin_unlock_irqrestore(&ksortd_work_queue_lock, flags);

	return pid;
}

/*
 * Check if there's work in the queue
 */
static bool ksortd_has_work(void)
{
	unsigned long flags;
	bool has_work;

	spin_lock_irqsave(&ksortd_work_queue_lock, flags);
	has_work = (ksortd_work_queue_count > 0);
	spin_unlock_irqrestore(&ksortd_work_queue_lock, flags);

	return has_work;
}

/*
 * Check if ksortd should be active
 * Returns true if either:
 *   1. Currently processing a PID (target_pid is set and per_app found)
 *   2. There's work in the queue
 */
static bool ksortd_should_run(void)
{
	bool result;

	mutex_lock(&ksortd_mutex);
	result = (ksortd_target_pid != 0 && scan_state.app != NULL);
	mutex_unlock(&ksortd_mutex);

	/* Also check if there's pending work */
	if (!result)
		result = ksortd_has_work();

	return result;
}

/*
 * Public API: Queue a PID for ksortd scanning
 * This is called from other subsystems (e.g., per_app)
 */
int ksortd_queue_work(pid_t pid)
{
	return ksortd_enqueue_work(pid);
}
EXPORT_SYMBOL(ksortd_queue_work);


/*
 * Process a single anonymous page using lazy rmap
 * Returns true if page was accessed (in CHECK mode)
 */
static bool ksortd_process_anon_page_lazy(struct page *page,
					   enum ksortd_scan_mode mode,
					   unsigned int *mapcount_ret)
{
	pte_t *ptep;
	pte_t pte;
	struct mm_struct *mm;
	spinlock_t *ptl;
	bool accessed = false;
	unsigned long address;

	/* Validate page */
	if (!page || !PagePerApp(page) || !PageAnon(page)) {
		if (mode == KSORTD_SCAN_CHECK)
			ksortd_skipped_pages++;
		return false;
	}

	/* Get PTE from lazy rmap (stored in page->mapping) */
	ptep = page_pte_lazy(page);
	if (!ptep) {
		if (mode == KSORTD_SCAN_CHECK)
			ksortd_skipped_pages++;
		return false;
	}

	/* Get mm_struct from page table page */
	mm = get_page_mm(ptep);
	if (!mm) {
		if (mode == KSORTD_SCAN_CHECK)
			ksortd_skipped_pages++;
		return false;
	}

	/* Get mapcount */
	if (mapcount_ret)
		*mapcount_ret = page_mapcount(page);

	/* Get PTE lock */
	ptl = &virt_to_page(ptep)->ptl;
	if (!ptl) {
		if (mode == KSORTD_SCAN_CHECK)
			ksortd_skipped_pages++;
		return false;
	}

	spin_lock(ptl);

	pte = *ptep;

	/* Verify PTE is still valid and points to our page */
	if (!pte_present(pte) || pte_page(pte) != page) {
		spin_unlock(ptl);
		if (mode == KSORTD_SCAN_CHECK)
			ksortd_skipped_pages++;
		return false;
	}

	/*
	 * For lazy rmap, we don't need the exact virtual address
	 * since we're directly accessing the PTE
	 */
	address = 0;  /* Not used for lazy rmap */

	if (mode == KSORTD_SCAN_CLEAR) {
		/* Phase 1: Clear accessed bit */
		if (pte_young(pte)) {
			pte_t new_pte = pte_mkold(pte);
			set_pte_at(mm, address, ptep, new_pte);
#ifdef CONFIG_DEBUG_KSORTD
			pr_debug("ksortd: CLEAR lazy anon page\n");
#endif
		}
	} else {
		/* Phase 2: Check if accessed since clearing */
		if (pte_young(pte)) {
			accessed = true;
#ifdef CONFIG_DEBUG_KSORTD
			pr_debug("ksortd: CHECK lazy anon page: ACCESSED\n");
#endif
		}
	}

	spin_unlock(ptl);

	return accessed;
}

/* ========== Target PID Management ========== */

/*
 * Set target PID for scanning
 * Called from procfs write handler or work queue
 */
static int ksortd_set_target_pid(pid_t pid)
{
	struct per_app *new_app, *old_app;

	/* Get per_app struct for the target PID */
	new_app = per_app_find(pid);
	if (!new_app && pid != 0) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_err("ksortd: Failed to find per_app for PID %d\n", pid);
#endif
		return -ESRCH;
	}

	mutex_lock(&ksortd_mutex);

	old_app = scan_state.app;

	scan_state.app = new_app;
	scan_state.scan_progress = 0;
	scan_state.phase = (pid != 0) ? KSORTD_PHASE_IDLE : KSORTD_PHASE_IDLE;
	ksortd_target_pid = pid;

	/* Reset all statistics */
	ksortd_reset_cycle_stats();
	ksortd_clear_cycles = 0;
	ksortd_check_cycles = 0;
	ksortd_total_cycles = 0;

	mutex_unlock(&ksortd_mutex);

	/* Note: per_app doesn't use reference counting like mm_struct */

	if (pid != 0) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_info("ksortd: Target set to PID %d (per_app=%p)\n", pid, new_app);
#endif
		wake_up_interruptible(&ksortd_wait);
	} else {
#ifdef CONFIG_DEBUG_KSORTD
		pr_info("ksortd: Target cleared\n");
#endif
	}

	return 0;
}

/*
 * rmap callback: Process one PTE mapping of a page
 * Called for each VMA that maps the folio
 */
static bool ksortd_rmap_one(struct folio *folio, struct vm_area_struct *vma,
			    unsigned long address, void *arg)
{
	struct rmap_check_arg *rca = arg;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;

	/* Validate all parameters - vma can be NULL in some rmap walks */
	if (!rca || !vma || !vma->vm_mm)
		return true;

	/* Walk page table manually to find PTE */
	pgd = pgd_offset(vma->vm_mm, address);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return true;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return true;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || pud_bad(*pud))
		return true;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		return true;

	/* Handle THP - skip for now, focus on base pages */
	if (pmd_trans_huge(*pmd))
		return true;

	/* Get PTE with proper locking */
	pte = pte_offset_map_lock(vma->vm_mm, pmd, address, &ptl);
	if (!pte)
		return true;

	/* Verify this PTE still points to our target page */
	if (pte_present(*pte) && pte_page(*pte) == rca->target_page) {
		rca->mapcount++;

		if (rca->mode == KSORTD_SCAN_CLEAR) {
			/* Phase 1: Clear accessed bit */
			if (pte_young(*pte)) {
				ptep_test_and_clear_young(vma, address, pte);
#ifdef CONFIG_DEBUG_KSORTD
				pr_debug("ksortd: CLEAR accessed bit at 0x%lx\n",
					 address);
#endif
			}
		} else {
			/* Phase 2: Check if accessed since clearing */
			if (pte_young(*pte)) {
				rca->accessed = true;
				rca->accessed_count++;
#ifdef CONFIG_DEBUG_KSORTD
				pr_debug("ksortd: CHECK at 0x%lx: ACCESSED\n",
					 address);
#endif
			}
		}
	}

	pte_unmap_unlock(pte, ptl);

	return true;  /* Continue to next mapping */
}

/*
 * Lock the anon_vma for reading during rmap walk
 */
static struct anon_vma *ksortd_anon_lock(struct folio *folio,
					 struct rmap_walk_control *rwc)
{
	return folio_lock_anon_vma_read(folio, rwc);
}

/*
 * Check/clear accessed bits for a page via reverse mapping
 * Returns true if page was accessed (only meaningful in CHECK mode)
 *
 * IMPORTANT: Only processes pages that are:
 *   - Mapped (has PTEs pointing to it)
 *   - Single-mapped (mapcount == 1) - skip shared pages
 *   - Lockable (can get page lock without blocking)
 */
static bool ksortd_check_page_accessed_rmap(struct page *page,
					    enum ksortd_scan_mode mode,
					    unsigned int *mapcount_ret,
					    unsigned int *accessed_count_ret)
{
	struct rmap_check_arg rca = {
		.target_page = page,
		.mode = mode,
		.accessed = false,
		.mapcount = 0,
		.accessed_count = 0,
	};
	struct rmap_walk_control rwc = {
		.rmap_one = ksortd_rmap_one,
		.arg = &rca,
		.anon_lock = ksortd_anon_lock,
	};
	struct folio *folio;
	int current_mapcount;

	/* Skip unmapped pages */
	if (!page_mapped(page))
		return false;

	/*
	 * Skip shared pages (mapcount > 1)
	 * We only want to track access patterns of single-mapped pages
	 * to avoid complications with shared memory regions
	 */
	current_mapcount = page_mapcount(page);
	if (current_mapcount != 1) {
		if (mapcount_ret)
			*mapcount_ret = current_mapcount;
		return false;
	}

	/*
	 * Try to lock the page - if we can't get it immediately,
	 * skip this page to avoid blocking
	 */
	if (!trylock_page(page))
		return false;

	folio = page_folio(page);

	/* Walk all reverse mappings for this folio */
	rmap_walk(folio, &rwc);

	unlock_page(page);

	if (mapcount_ret)
		*mapcount_ret = rca.mapcount;
	if (accessed_count_ret)
		*accessed_count_ret = rca.accessed_count;

	return rca.accessed;
}


/*
 * Scan ALL pages in per_app's cold page lists
 * Returns: number of pages actually scanned
 *
 * Scans entire cold_anon_list and cold_file_list in one pass.
 * Uses lock-drop pattern to minimize contention and calls cond_resched()
 * to yield CPU periodically.
 */
static unsigned int ksortd_scan_per_app_lists(struct per_app *app,
					       enum ksortd_scan_mode mode)
{
	struct page *page, *tmp;
	unsigned int scanned = 0;
	unsigned int skipped = 0;
	bool accessed;
	unsigned int mapcount;

	if (!app)
		return 0;

	/*
	 * Scan ALL cold anonymous pages
	 * Lock-drop pattern: hold lock only during list iteration,
	 * drop it while processing each page
	 */
	spin_lock(&app->cold_anon_lock);
	list_for_each_entry_safe(page, tmp, &app->cold_anon_list, lru) {
		/* Get page reference - page could be freed */
		if (!get_page_unless_zero(page)) {
			skipped++;
			continue;
		}

		spin_unlock(&app->cold_anon_lock);

		/* Validate page is still a per-app anonymous page */
		if (!PagePerApp(page) || !PageAnon(page)) {
			put_page(page);
			skipped++;
			spin_lock(&app->cold_anon_lock);
			continue;
		}

		/* Check mapcount */
		mapcount = page_mapcount(page);

		if (mapcount > 1) {
			/* Shared page - account but don't process */
			if (mode == KSORTD_SCAN_CHECK)
				ksortd_shared_pages++;
			put_page(page);
			scanned++;
			spin_lock(&app->cold_anon_lock);
			cond_resched();
			continue;
		}

		/* Process anonymous page using lazy rmap */
		accessed = ksortd_process_anon_page_lazy(page, mode, &mapcount);

		/* Update statistics and promote if accessed */
		if (mode == KSORTD_SCAN_CHECK) {
			ksortd_total_scanned++;
			ksortd_anon_scanned++;

			if (accessed) {
				ksortd_hot_pages++;
				ksortd_hot_anon_pages++;
				/* Promote page from cold to hot */
				per_app_promote_page_to_hot(page, app);
#ifdef CONFIG_DEBUG_KSORTD
				pr_debug("ksortd: Promoted anon page %p\n", page);
#endif
			} else {
				ksortd_cold_pages++;
				ksortd_cold_anon_pages++;
			}
		}

		put_page(page);
		scanned++;
		spin_lock(&app->cold_anon_lock);
		cond_resched();
	}
	spin_unlock(&app->cold_anon_lock);

	/*
	 * Scan ALL cold file pages
	 * Same lock-drop pattern
	 */
	spin_lock(&app->cold_file_lock);
	list_for_each_entry_safe(page, tmp, &app->cold_file_list, lru) {
		/* Get page reference - page could be freed */
		if (!get_page_unless_zero(page)) {
			skipped++;
			continue;
		}

		spin_unlock(&app->cold_file_lock);

		/* Validate page is still a per-app file page */
		if (!PagePerApp(page) || PageAnon(page)) {
			put_page(page);
			skipped++;
			spin_lock(&app->cold_file_lock);
			continue;
		}

		/* Check mapcount */
		mapcount = page_mapcount(page);

		if (mapcount > 1) {
			/* Shared page - account but don't process */
			if (mode == KSORTD_SCAN_CHECK)
				ksortd_shared_pages++;
			put_page(page);
			scanned++;
			spin_lock(&app->cold_file_lock);
			cond_resched();
			continue;
		}

		/* Process file page using standard rmap */
		accessed = ksortd_check_page_accessed_rmap(page, mode, &mapcount, NULL);

		/* Update statistics and promote if accessed */
		if (mode == KSORTD_SCAN_CHECK) {
			ksortd_total_scanned++;
			ksortd_file_scanned++;

			if (accessed) {
				ksortd_hot_pages++;
				ksortd_hot_file_pages++;
				/* Promote page from cold to hot */
				per_app_promote_page_to_hot(page, app);
#ifdef CONFIG_DEBUG_KSORTD
				pr_debug("ksortd: Promoted file page %p\n", page);
#endif
			} else {
				ksortd_cold_pages++;
				ksortd_cold_file_pages++;
			}
		}

		put_page(page);
		scanned++;
		spin_lock(&app->cold_file_lock);
		cond_resched();
	}
	spin_unlock(&app->cold_file_lock);

#ifdef CONFIG_DEBUG_KSORTD
	pr_info("ksortd: Scanned %u pages (%u skipped)\n", scanned, skipped);
#endif
	return scanned;
}

/*
 * Scan the entire per_app cold lists for one phase
 * Returns true since we scan everything in one pass
 */
static bool ksortd_do_scan(enum ksortd_scan_mode mode)
{
	struct per_app *app;
	unsigned int scanned;

	/*
	 * Get app under mutex protection
	 */
	mutex_lock(&ksortd_mutex);
	app = scan_state.app;
	mutex_unlock(&ksortd_mutex);

	if (!app) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_debug("ksortd: No target per_app\n");
#endif
		return true;
	}

	/*
	 * Verify per_app is still valid by checking if we can find it
	 * This protects against per_app being freed while we're scanning
	 */
	if (per_app_find(ksortd_target_pid) != app) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_warn("ksortd: per_app %p no longer valid for PID %d\n", app, ksortd_target_pid);
#endif
		return true;  /* Abort this scan */
	}

	/* Scan ALL pages in per_app's cold lists */
	scanned = ksortd_scan_per_app_lists(app, mode);

	/* Update scan progress under mutex */
	mutex_lock(&ksortd_mutex);
	if (scan_state.app == app) {  /* Verify app hasn't changed */
		scan_state.scan_progress = scanned;
	}
	mutex_unlock(&ksortd_mutex);

	if (mode == KSORTD_SCAN_CLEAR) {
#ifdef CONFIG_DEBUG_KSORTD
		pr_info("ksortd: [CLEAR] Scanned %u pages\n", scanned);
#endif
	} else {
#ifdef CONFIG_DEBUG_KSORTD
		pr_info("ksortd: [CHECK] Scanned %u pages (hot=%lu, cold=%lu)\n",
			scanned, ksortd_hot_pages, ksortd_cold_pages);
#endif
	}

	return true;  /* Always complete in one pass */
}

/*
 * Reset statistics for a new cycle
 */
static void ksortd_reset_cycle_stats(void)
{
	ksortd_total_scanned = 0;
	ksortd_hot_pages = 0;
	ksortd_cold_pages = 0;
	ksortd_anon_scanned = 0;
	ksortd_hot_anon_pages = 0;
	ksortd_cold_anon_pages = 0;
	ksortd_file_scanned = 0;
	ksortd_hot_file_pages = 0;
	ksortd_cold_file_pages = 0;
	ksortd_shared_pages = 0;
	ksortd_skipped_pages = 0;
}


/*
 * Get current phase (thread-safe)
 */
static enum ksortd_phase ksortd_get_phase(void)
{
	enum ksortd_phase phase;

	mutex_lock(&ksortd_mutex);
	phase = scan_state.phase;
	mutex_unlock(&ksortd_mutex);

	return phase;
}

/*
 * Set current phase (thread-safe)
 */
static void ksortd_set_phase(enum ksortd_phase phase)
{
	mutex_lock(&ksortd_mutex);
	scan_state.phase = phase;
	scan_state.scan_progress = 0;  /* Reset scan progress on phase change */
	mutex_unlock(&ksortd_mutex);
}

/*
 * Wait for specified time or until interrupted
 * Returns true if we should continue, false if we should stop
 */
static bool ksortd_wait_timeout(unsigned int ms, const char *reason)
{
	long timeout;

#ifdef CONFIG_DEBUG_KSORTD
	pr_info("ksortd: Waiting %u ms (%s)...\n", ms, reason);
#endif

	timeout = wait_event_interruptible_timeout(
		ksortd_wait,
		!ksortd_should_run() || kthread_should_stop(),
		msecs_to_jiffies(ms));

	if (kthread_should_stop())
		return false;

	if (!ksortd_should_run())
		return false;

	return true;
}

/*
 * Main kthread function - implements the state machine
 *
 * State machine (single cycle, then back to idle):
 *   IDLE -> WAIT_CLEAR -> CLEARING -> WAIT_CHECK -> CHECKING -> COMPLETE -> IDLE
 *
 * Scheduling: Uses SCHED_IDLE policy to only run during idle CPU time.
 * This is compatible with our locking strategy because:
 *   - We only hold spinlocks (PTE locks) for microseconds
 *   - We use trylock variants to avoid blocking
 *   - We call cond_resched() between pages to allow preemption
 */
static int ksortd(void *p)
{
	struct sched_param param = { .sched_priority = 0 };

	set_freezable();

	/*
	 * Set SCHED_IDLE policy - this thread will only run when no other
	 * runnable tasks exist on the CPU. This is ideal for background
	 * scanning work that shouldn't impact foreground performance.
	 *
	 * Note: SCHED_IDLE tasks can still hold spinlocks briefly.
	 * The scheduler won't preempt us mid-spinlock, and our spinlock
	 * holds are very short (microseconds for PTE modifications).
	 */
	sched_setscheduler(current, SCHED_IDLE, &param);

#ifdef CONFIG_DEBUG_KSORTD
	pr_info("ksortd: Thread started (SCHED_IDLE policy)\n");
#endif

	while (!kthread_should_stop()) {
		enum ksortd_phase phase;
		bool batch_complete;
		pid_t next_pid;

		/* Handle freezer for suspend/hibernate */
		try_to_freeze();

		/* Wait for a target to be set or work to be queued */
		if (!ksortd_should_run()) {
#ifdef CONFIG_DEBUG_KSORTD
			pr_info("ksortd: Idle - waiting for work...\n");
#endif
			wait_event_interruptible(ksortd_wait,
				ksortd_should_run() || kthread_should_stop());
			continue;
		}

		/*
		 * Check if we need to pick up new work from the queue
		 * Only dequeue when we're idle (no current target)
		 */
		mutex_lock(&ksortd_mutex);
		if (ksortd_target_pid == 0 && scan_state.phase == KSORTD_PHASE_IDLE) {
			mutex_unlock(&ksortd_mutex);

			/* Try to get work from queue */
			next_pid = ksortd_dequeue_work();
			if (next_pid != 0) {
				/* Start processing this PID */
				if (ksortd_set_target_pid(next_pid) == 0) {
#ifdef CONFIG_DEBUG_KSORTD
					pr_info("ksortd: Processing PID %d from work queue\n", next_pid);
#endif
				}
			}
		} else {
			mutex_unlock(&ksortd_mutex);
		}

		phase = ksortd_get_phase();

		switch (phase) {
		case KSORTD_PHASE_IDLE:
			/* New target set - transition to waiting for CLEAR phase */
#ifdef CONFIG_DEBUG_KSORTD
			pr_info("ksortd: New target acquired, starting scan cycle\n");
#endif
			ksortd_set_phase(KSORTD_PHASE_WAIT_CLEAR);
			break;

		case KSORTD_PHASE_WAIT_CLEAR:
			/* Wait before starting CLEAR phase */
			if (!ksortd_wait_timeout(ksortd_sleep_ms,
						 "before CLEAR phase"))
				continue;

#ifdef CONFIG_DEBUG_KSORTD
			pr_info("ksortd: ===== Starting CLEAR Phase =====\n");
#endif
			ksortd_set_phase(KSORTD_PHASE_CLEARING);
			break;

		case KSORTD_PHASE_CLEARING:
			/* Scan all pages in CLEAR mode */
			batch_complete = ksortd_do_scan(KSORTD_SCAN_CLEAR);

			if (batch_complete) {
				ksortd_clear_cycles++;
#ifdef CONFIG_DEBUG_KSORTD
				pr_info("ksortd: CLEAR phase complete - all accessed bits cleared\n");
#endif
				ksortd_set_phase(KSORTD_PHASE_WAIT_CHECK);
			}
			/*
			 * Yield between batches - important for SCHED_IDLE
			 * This ensures we don't monopolize CPU even if idle
			 */
			cond_resched();
			break;

		case KSORTD_PHASE_WAIT_CHECK:
			/* Wait before starting CHECK phase - let process run */
			if (!ksortd_wait_timeout(ksortd_sleep_ms,
						 "before CHECK phase"))
				continue;

#ifdef CONFIG_DEBUG_KSORTD
			pr_info("ksortd: ===== Starting CHECK Phase =====\n");
#endif
			ksortd_reset_cycle_stats();
			ksortd_set_phase(KSORTD_PHASE_CHECKING);
			break;

		case KSORTD_PHASE_CHECKING:
			/* Scan all pages in CHECK mode */
			batch_complete = ksortd_do_scan(KSORTD_SCAN_CHECK);

			if (batch_complete) {
				ksortd_check_cycles++;
				ksortd_total_cycles++;
#ifdef CONFIG_DEBUG_KSORTD
				pr_info("ksortd: ===== Scan Complete =====\n");
				pr_info("ksortd: Results - Hot: %lu, Cold: %lu, Total: %lu\n",
					ksortd_hot_pages, ksortd_cold_pages,
					ksortd_total_scanned);
				if (ksortd_total_scanned > 0) {
					pr_info("ksortd: Hot ratio: %lu%%\n",
						(ksortd_hot_pages * 100) / ksortd_total_scanned);
				}
#endif
				/* Scan complete - go to COMPLETE phase */
				ksortd_set_phase(KSORTD_PHASE_COMPLETE);
			}
			cond_resched();
			break;

		case KSORTD_PHASE_COMPLETE:
			/*
			 * Cycle complete - clear target and go back to idle.
			 * User must write a new PID (or same PID) to start again.
			 */
#ifdef CONFIG_DEBUG_KSORTD
			pr_info("ksortd: Cycle complete, returning to idle\n");
#endif
			mutex_lock(&ksortd_mutex);
			ksortd_target_pid = 0;  /* Clear target */
			/* Keep mm reference for stats viewing, but mark as done */
			scan_state.phase = KSORTD_PHASE_IDLE;
			mutex_unlock(&ksortd_mutex);
			break;

		default:
#ifdef CONFIG_DEBUG_KSORTD
			pr_warn("ksortd: Unknown phase %d, resetting to idle\n", phase);
#endif
			ksortd_set_phase(KSORTD_PHASE_IDLE);
			break;
		}
	}

#ifdef CONFIG_DEBUG_KSORTD
	pr_info("ksortd: Thread stopping\n");
#endif
	return 0;
}


/* ========== Procfs Interface ========== */

static ssize_t ksortd_pid_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char buf[32];
	pid_t pid;
	int ret;

	if (count > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	buf[count] = '\0';

	ret = kstrtoint(buf, 10, &pid);
	if (ret)
		return ret;

	ret = ksortd_set_target_pid(pid);
	if (ret)
		return ret;

	return count;
}

static int ksortd_pid_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", ksortd_target_pid);
	return 0;
}

static int ksortd_pid_open(struct inode *inode, struct file *file)
{
	return single_open(file, ksortd_pid_show, NULL);
}

static const struct proc_ops ksortd_pid_ops = {
	.proc_open = ksortd_pid_open,
	.proc_read = seq_read,
	.proc_write = ksortd_pid_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const char *ksortd_phase_str(enum ksortd_phase phase)
{
	switch (phase) {
	case KSORTD_PHASE_IDLE:       return "IDLE";
	case KSORTD_PHASE_WAIT_CLEAR: return "WAIT_CLEAR";
	case KSORTD_PHASE_CLEARING:   return "CLEARING";
	case KSORTD_PHASE_WAIT_CHECK: return "WAIT_CHECK";
	case KSORTD_PHASE_CHECKING:   return "CHECKING";
	case KSORTD_PHASE_COMPLETE:   return "COMPLETE";
	default:                      return "UNKNOWN";
	}
}

static int ksortd_stats_show(struct seq_file *m, void *v)
{
	mutex_lock(&ksortd_mutex);

	seq_printf(m, "=== Configuration ===\n");
	seq_printf(m, "Target PID: %d\n", ksortd_target_pid);
	seq_printf(m, "Sleep interval: %u ms\n", ksortd_sleep_ms);
	seq_printf(m, "Pages per batch: %u\n", ksortd_thread_pages_to_scan);
	seq_printf(m, "\n");

	seq_printf(m, "=== State ===\n");
	seq_printf(m, "Current phase: %s\n", ksortd_phase_str(scan_state.phase));
	seq_printf(m, "Scan progress: %lu pages\n", scan_state.scan_progress);
	seq_printf(m, "Per-app: %p\n", scan_state.app);
	seq_printf(m, "\n");

	seq_printf(m, "=== Cycle Counts ===\n");
	seq_printf(m, "Total cycles completed: %lu\n", ksortd_total_cycles);
	seq_printf(m, "Clear phases completed: %lu\n", ksortd_clear_cycles);
	seq_printf(m, "Check phases completed: %lu\n", ksortd_check_cycles);
	seq_printf(m, "\n");

	seq_printf(m, "=== Current/Last Cycle Statistics ===\n");
	seq_printf(m, "Total pages scanned: %lu\n", ksortd_total_scanned);
	seq_printf(m, "  Anonymous pages: %lu\n", ksortd_anon_scanned);
	seq_printf(m, "  File-backed pages: %lu\n", ksortd_file_scanned);
	seq_printf(m, "\n");

	seq_printf(m, "Hot pages (accessed): %lu\n", ksortd_hot_pages);
	seq_printf(m, "  Hot anonymous: %lu\n", ksortd_hot_anon_pages);
	seq_printf(m, "  Hot file-backed: %lu\n", ksortd_hot_file_pages);
	seq_printf(m, "\n");

	seq_printf(m, "Cold pages (not accessed): %lu\n", ksortd_cold_pages);
	seq_printf(m, "  Cold anonymous: %lu\n", ksortd_cold_anon_pages);
	seq_printf(m, "  Cold file-backed: %lu\n", ksortd_cold_file_pages);
	seq_printf(m, "\n");

	seq_printf(m, "=== Skipped Pages ===\n");
	seq_printf(m, "Shared pages (mapcount > 1): %lu\n", ksortd_shared_pages);
	seq_printf(m, "Other skipped (locked, unmapped, etc.): %lu\n", ksortd_skipped_pages);

	if (ksortd_total_scanned > 0) {
		seq_printf(m, "\n");
		seq_printf(m, "=== Ratios (of processed pages) ===\n");
		seq_printf(m, "Hot ratio: %lu%%\n",
			   (ksortd_hot_pages * 100) / ksortd_total_scanned);
		seq_printf(m, "Cold ratio: %lu%%\n",
			   (ksortd_cold_pages * 100) / ksortd_total_scanned);
	}

	mutex_unlock(&ksortd_mutex);
	return 0;
}

static int ksortd_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ksortd_stats_show, NULL);
}

static const struct proc_ops ksortd_stats_ops = {
	.proc_open = ksortd_stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t ksortd_sleep_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int ms;
	int ret;

	if (count > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	buf[count] = '\0';

	ret = kstrtouint(buf, 10, &ms);
	if (ret)
		return ret;

	if (ms < KSORTD_MIN_SLEEP_MS || ms > KSORTD_MAX_SLEEP_MS)
		return -EINVAL;

	WRITE_ONCE(ksortd_sleep_ms, ms);
	wake_up_interruptible(&ksortd_wait);

	pr_info("ksortd: Sleep interval set to %u ms\n", ms);
	return count;
}

static int ksortd_sleep_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", ksortd_sleep_ms);
	return 0;
}

static int ksortd_sleep_open(struct inode *inode, struct file *file)
{
	return single_open(file, ksortd_sleep_show, NULL);
}

static const struct proc_ops ksortd_sleep_ops = {
	.proc_open = ksortd_sleep_open,
	.proc_read = seq_read,
	.proc_write = ksortd_sleep_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t ksortd_pages_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int pages;
	int ret;

	if (count > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	buf[count] = '\0';

	ret = kstrtouint(buf, 10, &pages);
	if (ret)
		return ret;

	if (pages < KSORTD_MIN_PAGES || pages > KSORTD_MAX_PAGES)
		return -EINVAL;

	WRITE_ONCE(ksortd_thread_pages_to_scan, pages);
	pr_info("ksortd: Pages per batch set to %u\n", pages);
	return count;
}

static int ksortd_pages_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", ksortd_thread_pages_to_scan);
	return 0;
}

static int ksortd_pages_open(struct inode *inode, struct file *file)
{
	return single_open(file, ksortd_pages_show, NULL);
}

static const struct proc_ops ksortd_pages_ops = {
	.proc_open = ksortd_pages_open,
	.proc_read = seq_read,
	.proc_write = ksortd_pages_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ========== Module Init/Exit ========== */

static struct proc_dir_entry *ksortd_proc_dir;

static int __init ksortd_init(void)
{
	int err;

	pr_info("ksortd: Initializing...\n");

	memset(&scan_state, 0, sizeof(scan_state));
	scan_state.phase = KSORTD_PHASE_IDLE;

	/* Initialize work queue */
	memset(ksortd_work_queue, 0, sizeof(ksortd_work_queue));
	ksortd_work_queue_head = 0;
	ksortd_work_queue_tail = 0;
	ksortd_work_queue_count = 0;

	/* Create procfs directory and files */
	ksortd_proc_dir = proc_mkdir("ksortd", NULL);
	if (!ksortd_proc_dir) {
		pr_err("ksortd: Failed to create /proc/ksortd\n");
		return -ENOMEM;
	}

	proc_create("pid", 0666, ksortd_proc_dir, &ksortd_pid_ops);
	proc_create("stats", 0444, ksortd_proc_dir, &ksortd_stats_ops);
	proc_create("sleep_ms", 0666, ksortd_proc_dir, &ksortd_sleep_ops);
	proc_create("pages_to_scan", 0666, ksortd_proc_dir, &ksortd_pages_ops);

	/* Start the kernel thread */
	ksortd_thread = kthread_run(ksortd, NULL, "ksortd");
	if (IS_ERR(ksortd_thread)) {
		err = PTR_ERR(ksortd_thread);
		pr_err("ksortd: Failed to create thread (%d)\n", err);
		remove_proc_subtree("ksortd", NULL);
		return err;
	}

	pr_info("ksortd: Initialization complete\n");
	pr_info("ksortd: Operation: WAIT -> CLEAR (batched) -> WAIT -> CHECK (batched) -> repeat\n");
	pr_info("ksortd: Usage:\n");
	pr_info("  echo <PID> > /proc/ksortd/pid        # Set target process\n");
	pr_info("  cat /proc/ksortd/stats               # View statistics\n");
	pr_info("  echo <ms> > /proc/ksortd/sleep_ms    # Set wait interval\n");
	pr_info("  echo <n> > /proc/ksortd/pages_to_scan # Set batch size\n");

	return 0;
}

static void __exit ksortd_exit(void)
{
	pr_info("ksortd: Shutting down...\n");

	/* Stop the kernel thread */
	if (ksortd_thread) {
		kthread_stop(ksortd_thread);
		ksortd_thread = NULL;
	}

	/* Remove procfs entries */
	remove_proc_subtree("ksortd", NULL);

	/* Clear per_app reference */
	mutex_lock(&ksortd_mutex);
	scan_state.app = NULL;
	mutex_unlock(&ksortd_mutex);

	pr_info("ksortd: Shutdown complete\n");
}

subsys_initcall(ksortd_init);
module_exit(ksortd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel thread for page access bit scanning");
MODULE_AUTHOR("ksortd");

