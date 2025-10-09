#ifndef _LINUX_PER_APP_H
#define _LINUX_PER_APP_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/uidgid.h>
#include <linux/page-flags.h>
#include <linux/rmap.h>
#include <linux/android_vendor.h>
#include <linux/memcontrol.h>

#ifdef CONFIG_PAPP_USE_KREF
#include <linux/kref.h>
#endif

/* Forward declarations to avoid circular dependencies */
struct task_struct;

/* target UIDs of applications */
extern const uid_t target_app_uids[];
extern const size_t num_target_app_uids;

/* Define TASK_COMM_LEN since we can't include sched.h */
#define TASK_COMM_LEN 16

// per-app reclaim flags
#define PER_APP_CONGESTED 0 // writeback, immediate reclaim is required
#define PER_APP_WRITEBACK 1 // writeback pages exist
#define PER_APP_STALL 2 // direct reclaims should be stalled
#define PER_APP_DIRTY 3 // dirty pages exist

// maximum number of pages to scan per app
#define APP_CLUSTER_MAX 1024

#define PATHSTR_LEN 256
#define PACKAGE_NAME_LEN 256

#define TEMP_HOME_PATHSTR "/var/tmp" // tmp pathstr for apps without home dir

#define OVERRIDE_HOME_PATHSTR 1

// which type of page to reclaim?
enum per_app_reclaim_type {
	RECLAIM_ANON,
	RECLAIM_FILE,
	RECLAIM_ALL,
};

// reclaim state machine
typedef enum {
	RECLAIM_STATE_R0 = 0, /* initial state - not reclaimed yet */
	RECLAIM_STATE_R1 = 1,
	RECLAIM_STATE_R2 = 2,
	RECLAIM_STATE_R3 = 3,
	RECLAIM_STATE_R4 = 4,
	RECLAIM_STATE_OOM = 5, /* candidate for termination, OOM */
} reclaim_state_t;

#define HOME_BIT(t) (1U << (t))

enum home_type { HOME_CE, HOME_DE, HOME_NR };

static_assert(HOME_NR <= 32, "bitmap too small for HOME_NR");

struct home_entry {
	struct dentry *root;
	enum home_type type;
	char path[PATHSTR_LEN];
};

struct home_dentry_cache {
	struct rw_semaphore rwsem;

	struct home_entry slots[HOME_NR];
	u32 bitmap;
};

// RCU: https://lwn.net/Articles/262464/

struct inode_tag {
	struct rhash_head hnode; // in-hashtable linkage
	struct inode *inode; // address of tagged inode
	struct per_app *owner; // the app/UID that owns this inode
	struct rcu_head rcu; // for RCU freeing
	enum home_type type; // which home this inode belongs to
};

// app->nr_pages >> reclaim_ratio[i]
extern const unsigned long reclaim_ratio[];

/*
 * per-app memory management struct
 * one per each unique app process (uid: pid)
 */
struct per_app {
	kuid_t uid;
	pid_t pid; // thread group id

	char app_name[TASK_COMM_LEN];

	struct list_head page_list; // head of page list
	struct list_head filepage_list; // head of file page list

	spinlock_t page_list_lock; // spinlock for page list
	spinlock_t filepage_list_lock; // spinlock for file page list

	atomic_long_t nr_pages; // present in RAM? or including swap?
	atomic_long_t nr_anon_pages;
	atomic_long_t nr_file_pages;

	atomic_long_t nr_reclaimed;
	atomic_long_t nr_anon_reclaimed; // swapped out
	atomic_long_t nr_file_reclaimed;

#ifdef CONFIG_PAPP_USE_KREF
	// ref count and lifetime management
	struct kref kref;
#endif

	// global linkage
	struct list_head app_list;
	struct hlist_node hash_node;

	struct home_dentry_cache home;

	// atomic_t nr_processes; // should be just 1. if 0, main thread exited
	atomic_t nr_tasks; // including main thread

	char package_name[PACKAGE_NAME_LEN]; // com.app.android

	// these fields are not currently being used.. but maybe in the future
	// int oom_score_adj;
	// unsigned long flags; // reclaim flags for this per-app

	reclaim_state_t reclaim_state;
};

/*
 * global app (lru) list
 */
struct per_app_manager {
	struct list_head app_list;
	spinlock_t app_list_lock;
	atomic_t nr_apps;

	// hash-table for faster lookup (pid -> struct per_app)
	struct hlist_head *pid_hash;
	unsigned int hash_bits;
	spinlock_t hash_lock;
};

// inline functions
static inline struct vm_area_struct *per_app_folio_vma(struct folio *folio)
{
	unsigned long mapping = (unsigned long)folio->mapping;
	// struct vm_area_struct *vma;

	if (!mapping)
		BUG();

	// if (!(mapping & PAGE_MAPPING_ANON)) {
	// 	pr_err("[per_app_folio_vma] per-app has no VMA: mapping=0x%lx, PAGE_MAPPING_ANON=0x%x\n",
	// 	       mapping, PAGE_MAPPING_ANON);
	// 	WARN(1, "[per_app_folio_vma] per-app has no VMA\n");
	// 	return NULL;
	// }

	if ((mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON) {
		return NULL;
	}

	return (void *)(mapping - PAGE_MAPPING_ANON);
}

/*
 * ===============================================
 * VENDOR DATA ACCESS FUNCTIONS FOR per_app CACHE
 * ===============================================
 */

/**
 * per_app_set_cached - Store per_app pointer in task's vendor data
 * @task: The task_struct to modify
 * @app: Pointer to per_app struct (can be NULL)
 */
static inline void __per_app_set_cached(struct task_struct *task,
					struct per_app *app)
{
	task->android_vendor_data1[0] = (u64)(uintptr_t)app;
}

/**
 * per_app_get_cached - Retrieve per_app pointer from task's vendor data
 * @task: The task_struct to read from
 *
 * Returns: Pointer to per_app struct, or NULL if not set
 */
static inline struct per_app *__per_app_get_cached(struct task_struct *task)
{
	return (struct per_app *)(uintptr_t)task->android_vendor_data1[0];
}

/**
 * per_app_clear_cached - Clear the per_app cache (set to NULL)
 * @task: The task_struct to modify
 */
static inline void __per_app_clear_cached(struct task_struct *task)
{
	task->android_vendor_data1[0] = 0;
}

/**
 * per_app_is_cached - Check if task has cached per_app pointer
 * @task: The task_struct to check
 *
 * Returns: true if cached pointer exists, false otherwise
 */
static inline bool __per_app_is_cached(struct task_struct *task)
{
	return task->android_vendor_data1[0] != 0;
}

/**
 * per_app_init_vendor_data - Initialize vendor data for a task
 * @task: The task_struct to initialize
 */
static inline void __per_app_init_vendor_data(struct task_struct *task)
{
	android_init_vendor_data(task, 1);
}

// page flags for per-app pages
#define APP_PAGE_ANON (1 << 0)
#define APP_PAGE_FILE (1 << 1)
#define APP_PAGE_DIRTY (1 << 2)

struct per_app *per_app_create(struct task_struct *task);
struct per_app *per_app_find(pid_t pid);

bool per_app_is_target_uid(uid_t uid);
void per_app_try_to_update_position(struct task_struct *task, int oom);

#ifdef CONFIG_PAPP_USE_KREF
struct per_app *per_app_get(struct per_app *app);
void per_app_put(struct per_app *app);
#endif

int per_app_add_page(struct page *page, struct per_app *app);
int per_app_add_page_vma(struct page *page, struct vm_area_struct *vma,
			 struct per_app *app);
void per_app_remove_page(struct per_app *app, struct page *page);
void per_app_move_page_to_lru(struct per_app *app, struct page *page,
			      struct vm_area_struct *vma);
struct per_app *per_app_get_current(void);
unsigned long per_app_get_page_count(struct per_app *app);
struct per_app *per_app_select_app(void);
unsigned long per_app_calculate_nr_to_scan(struct per_app *app);
void per_app_try_to_advance_reclaim_state(struct per_app *app,
					  unsigned long nr_reclaimed);
void per_app_advance_reclaim_state(struct per_app *app);
void per_app_print_reclaim_status(struct per_app *app);

int per_app_manager_init(void);
void per_app_manager_exit(void);
int __init per_app_init_subsystem(void);
void __exit per_app_exit_subsystem(void);

// process creation/destruction hooks
void per_app_process_fork(struct task_struct *parent,
			  struct task_struct *child);
void per_app_process_exit(struct task_struct *task);

// cache management functions
void per_app_clear_cached(struct task_struct *task);
void per_app_update_cached(struct task_struct *task);
struct per_app *per_app_get_cached(struct task_struct *task);
bool per_app_is_cached(struct task_struct *task);

// reverse mapping
void per_app_add_new_anon_rmap_vma(struct page *page,
				   struct vm_area_struct *vma,
				   unsigned long address);
void per_app_add_anon_rmap_vma(struct page *page, struct vm_area_struct *vma,
			       unsigned long address, rmap_t flags);
void per_app_set_anon_rmap_vma(struct page *page, struct vm_area_struct *vma,
			       unsigned long address, int exclusive);
void per_app_move_anon_rmap_vma(struct page *page, struct vm_area_struct *vma);
void per_app_restore_anon_rmap(struct page *page, struct vm_area_struct *vma);

// force reclaim function declarations for vmscan.c
extern void per_app_force_reclaim(kuid_t uid, enum per_app_reclaim_type type,
				  unsigned long amount_mb, bool uid_specified);
extern void do_targeted_reclaim(int nid, enum per_app_reclaim_type type,
				unsigned long nr_to_reclaim,
				struct mem_cgroup *memcg);
extern void setup_scan_control_advanced(enum per_app_reclaim_type type,
					unsigned long nr_to_reclaim,
					bool aggressive,
					struct mem_cgroup *memcg);

// home_dentry_cache management
void per_app_home_dentry_cache_init(struct per_app *app);
void per_app_home_dentry_cache_destroy(struct per_app *app);

// resolution to dentry
int resolve_dir(const char *path, struct dentry **out);

// setting home_entry(s) inside the home_dentry_cache
int per_app_home_entry_set(struct per_app *app, enum home_type type,
			   const char *pathstr);
int per_app_home_entry_clear(struct per_app *app, enum home_type type);

// inode tagging
// TODO: i dont think all these functions need to be exposed in the header
int inode_tag_ht_init(void);
void inode_tag_ht_destroy(void);

void inode_tag_free_rcu(struct rcu_head *rcu);
void inode_tag_free_elem(void *ptr, void *arg);

// path matching
int per_app_home_match_dentry(struct per_app *app, struct dentry *leaf,
			      enum home_type *out_type);
int per_app_home_match_inode(struct per_app *app, struct inode *inode,
			     enum home_type *out_type);

// instrumentation hooks
void per_app_do_dentry_open_instrument(struct file *f);
void per_app_destroy_inode_instrument(struct inode *inode);

int per_app_filemap_add_folio_instrument(struct address_space *mapping,
					 struct folio *folio);
int per_app_filemap_remove_folio_instrument(struct address_space *mapping,
					    struct folio *folio);

// macros
#define for_each_app_page(pos, app) \
	list_for_each_entry(pos, &(app)->page_list, lru)
#define for_each_app_page_safe(pos, tmp, app) \
	list_for_each_entry_safe(pos, tmp, &(app)->page_list, lru)

#endif /* _LINUX_PER_APP_H */