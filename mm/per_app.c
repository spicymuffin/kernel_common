/*
 * Per-app memory management
 */

#include <linux/per_app.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/user_namespace.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cred.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/namei.h>
#include "internal.h"

static int insert_offset = 1;

void per_app_list_insert_from_tail(struct list_head *new, struct list_head *head)
{
  struct list_head *pos = head->prev;
  int i;

  for (i=0; i<insert_offset && pos != head; i++) {
    pos = pos->prev;
  }

  __list_add(new, pos, pos->next);
  insert_offset++;
}

const unsigned long reclaim_ratio[] = { 
  4,   /* R0->R1: 1/16 */
  3,   /* R1->R2: 1/8 */
  2,   /* R2->R3: 1/4 */
  1,   /* R3->R4: 1/2 */
  0    /* R4->OOM: all remaining */
};

const uid_t target_app_uids[] = {
  10150,
  10151,
  10152, // deskclock
  10153,
  10154, // google maps
  10192, // instagram
  10193, // thread
  10194,
  10195,
  10196,
};

const size_t num_target_app_uids = ARRAY_SIZE(target_app_uids);

// static package names
const char *packages[] = {
  "com.zeptolab.ctr.ads",
  "com.ebay.mobile",
  "com.instagram.barcelona",
  "com.facebook.katana",
  "com.facebook.orca",
  "com.madfingergames.legends",
  "com.pinterest",
  "com.wildspike.wormszone",
  "org.mozilla.firefox",
  "com.yelp.android",
  "com.trivago",
  "com.chess",
  "com.halfbrick.fruitninjafree",
  "com.block.juggle",
  "com.mrp",
  "com.nike.omega",
  "com.instagram.android",
  "com.android.providers.media.module",
  "com.android.providers.contacts",
  "com.android.companiondevicemanager",
  "com.android.providers.downloads",
  "com.android.credentialmanager",
  "com.android.devicelockcontroller",
  "com.android.documentsui",
  "com.android.adservices.api",
  "com.android.health.connect.backuprestore",
  "com.android.virtualmachine.res",
  "com.android.nearby.halfsheet",
  "com.android.intentresolver",
  "com.android.certinstaller",
  "com.android.apps.tag",
  "com.android.wifi.dialog",
  "com.android.captiveportallogin",
  "com.android.imsserviceentitlement",
  "com.android.providers.media",
  "com.android.statementservice",
  "com.android.simappdialog",
  "com.android.wallpaper.livepicker",
  "com.android.printservice.recommendation",
  "com.android.calendar",
  "com.android.managedprovisioning",
  "com.android.emergency",
  "com.android.healthconnect.controller",
  "com.android.traceur",
  "com.android.contacts",
  "com.android.mtp",
  "com.android.telephony.qns",
  "com.android.ondevicepersonalization.services",
  "com.android.ext.adservices.api",
  "com.android.gallery3d",
  "com.android.settings.intelligence",
  "com.android.storagemanager",
  "com.android.quicksearchbox",
  "com.android.packageinstaller",
  "com.android.printspooler",
  "com.android.deskclock",
  "com.android.egg",
  "com.android.soundpicker",
  "com.android.rkpdapp",
  "com.android.camera2",
  "com.android.hotspot2.osulogin",
  "com.android.messaging"
};
int num_packages = sizeof(packages) / sizeof(packages[0]);

/* global per-app manager */
static struct per_app_manager global_app_manager;

/* hash-table size: should be no more than 100 in theory */
#define PER_APP_HASH_BITS 8
#define PER_APP_HASH_SIZE (1 << PER_APP_HASH_BITS)

/* hash function for pid */
static inline unsigned int pid_hash(pid_t pid)
{
  return hash_32(pid, PER_APP_HASH_BITS);
}


/*
 * initialize the global per-app manager
 * should be called during kernel init
 */
int per_app_manager_init(void)
{
  int i;
  pr_info("[perapp]: initializing per-app memory manager\n");

  /* initialize global app list */
  INIT_LIST_HEAD(&global_app_manager.app_list);
  spin_lock_init(&global_app_manager.app_list_lock);
  atomic_set(&global_app_manager.nr_apps, 0);

  /* initialize global hash table */
  global_app_manager.pid_hash = kmalloc_array(PER_APP_HASH_SIZE, sizeof(struct hlist_head), GFP_KERNEL);
  if (!global_app_manager.pid_hash) {
    pr_err("[perapp]: failed to allocate global hash table\n");
    return -ENOMEM;
  }
  for (i=0; i<PER_APP_HASH_SIZE; i++)
    INIT_HLIST_HEAD(&global_app_manager.pid_hash[i]);
  global_app_manager.hash_bits = PER_APP_HASH_BITS;
  spin_lock_init(&global_app_manager.hash_lock);

  return 0;
}

/*
 * cleanup global manager
 */
void per_app_manager_exit(void)
{
  struct per_app *app, *tmp;
  pr_info("[perapp]: cleaning up per-app memory manager\n");

  /* free app list */
  spin_lock(&global_app_manager.app_list_lock);
  list_for_each_entry_safe(app, tmp, &global_app_manager.app_list, app_list) {
    list_del(&app->app_list);
#ifdef CONFIG_PAPP_USE_KREF
    per_app_put(app); // free struct ?
#endif
  }
  spin_unlock(&global_app_manager.app_list_lock);

  /* free hash table */
  kfree(global_app_manager.pid_hash);
  global_app_manager.pid_hash = NULL;
}

/*
 * Clear cached per_app pointer for a task
 * This should be called when credentials change or when the app is no longer valid
 */
void per_app_clear_cached(struct task_struct *task)
{
  __per_app_clear_cached(task);
}

// TODO: technically we don't need this, since per_app <-> task relationship is permanent.
/*
 * Update cached per_app pointer for a task
 * This should be called when a task's credentials change
 */
void per_app_update_cached(struct task_struct *task)
{
  kuid_t uid;
  pid_t pid;
  struct per_app *app;

	/* Clear old cached pointer */
	__per_app_clear_cached(task);
	
	/* Find and cache new per_app pointer */
	uid = task_uid(task);
  pid = task_tgid_nr(task);
	app = per_app_find(pid);
	
	/* Debug: Print cache update information */
	pr_info("[per-app-debug] per_app_update_cached: PID %u (%s) UID %d, found app=%p\n",
		pid, task->comm, from_kuid(&init_user_ns, uid), app);
	
	if (app) {
	  __per_app_set_cached(task, app);
#ifdef CONFIG_PAPP_USE_KREF
    per_app_put(app);
#endif
  } else {
	  pr_info("[per-app-debug] per_app_update_cached: No per_app struct found for UID %d\n",
		  from_kuid(&init_user_ns, uid));
  }
}

/*
 * Fast access to cached per_app pointer
 */
struct per_app *per_app_get_cached(struct task_struct *task)
{
	return __per_app_get_cached(task);
}

/*
 * Check if task has cached per_app pointer
 */
bool per_app_is_cached(struct task_struct *task)
{
	return __per_app_is_cached(task);
}

/*
 * helper function to check whether this process is of interest
 */
bool per_app_is_target_uid(uid_t uid)
{
  return uid > 10000; 
  /*
  size_t i;
  for (i=0; i<num_target_app_uids; i++) {
    if (target_app_uids[i] == uid)
      return true;
  }
  return false;
  */
}

void per_app_init_home(struct home_entry *home)
{
  home->home_path_str[0] = '\0';
  home->home_path.mnt = NULL;
  home->home_path.dentry = NULL;
}

// match given dentry to app's home dentry.
// set inode i_flags if match
// return 0 if success
int per_app_match_home_dentry(struct per_app *app, struct dentry *dentry, struct inode *inode)
{
  struct dentry *home_dentry = app->home.home_path.dentry;

  if (unlikely(!app || !dentry))
    return -EINVAL;

  // this per_app's home dentry is NULL (no match was found.) simply skip
  if (!home_dentry) {
    return 1;
  }
  
  if (dentry->d_sb == home_dentry->d_sb && d_ancestor(home_dentry, dentry)) {
    // match found, tag inode
    inode_set_flags(inode, S_PERAPP, S_PERAPP);
    return 0;
  }
  return 1;
}


// tag inode if per-app
void per_app_instrument_do_dentry_open(struct file *file)
{
  struct inode *inode;
  struct dentry *dentry;
  struct per_app *app;

  if (unlikely(!file))
    return;
  
  inode = file_inode(file);
  if (unlikely(!inode))
    return;

  // skip if already tagged: using i_flags
  if (IS_PERAPP(inode))
    return;

  // skip non-regular files and O_PATH
  if (unlikely(!S_ISREG(inode->i_mode) || (file->f_flags & O_PATH)))
    return;

  dentry = file->f_path.dentry;
   
  app = per_app_get_current();

  if (app) {
    // try to match dentry with per_app->home.home_path
    per_app_match_home_dentry(app, dentry, inode); 
    /*
    if(per_app_match_home_dentry(app, dentry, inode) == 0) {
      pr_info("[perapp] MATCH FOUND! inode is per-app, and tagged\n");
    }
    */
  }

  return;
}

void per_app_instrument_destroy_inode(struct inode *inode)
{
  if (IS_PERAPP(inode)) {
    inode_set_flags(inode, 0, S_PERAPP);
  }
  // TODO: cleanup file pages from page list?
}

// 0 : success
// 1 : global LRU
// <0: error
int per_app_instrument_filemap_add_folio(struct address_space *mapping, struct folio *folio)
{
  struct inode *inode = mapping ? mapping->host : NULL;
  struct page *page = &folio->page;
  struct per_app *app;
  
  VM_BUG_ON_PAGE(PageUnevictable(page) || PageLRU(page), page);
  
  if (unlikely(!inode || !page)) {
    return -EFAULT;
  }
  
  app = per_app_get_current();

  if (app && IS_PERAPP(inode)) {
    // add this file page
    return per_app_add_file_page(page, app);
  }

  return 1;
}

/*
 * create and initialize per_app struct for given uid
 * for now, added to global app list regardless of state
 */
struct per_app *per_app_create(struct task_struct *task) 
{
  struct per_app *app;
  unsigned int hash;
  kuid_t uid = task_uid(task);
  pid_t pid = task_tgid_nr(task);
  const char *name = task->comm;

  app = kzalloc(sizeof(*app), GFP_KERNEL);
  if (!app) {
    pr_err("[perapp]: failed to allocate memory for per_app struct\n");
    return NULL;
  }

  app->uid = uid;
  app->pid = pid;
  strncpy(app->app_name, name, PACKAGE_NAME_LEN - 1);
  INIT_LIST_HEAD(&app->page_list);
  spin_lock_init(&app->page_list_lock);
  INIT_LIST_HEAD(&app->file_page_list);
  spin_lock_init(&app->file_page_list_lock);
  atomic_long_set(&app->nr_pages, 0);
  atomic_long_set(&app->nr_anon_pages, 0);
  atomic_long_set(&app->nr_file_pages, 0);
  atomic_long_set(&app->nr_reclaimed ,0);
  atomic_long_set(&app->nr_anon_reclaimed ,0);
  atomic_long_set(&app->nr_file_reclaimed ,0);
  //atomic_set(&app->nr_processes, 1);
  atomic_set(&app->nr_tasks, 1);
  app->reclaim_state = RECLAIM_STATE_R0;
  app->reclaimed = false;
#ifdef CONFIG_PAPP_USE_KREF      
  kref_init(&app->kref); // initial refcount is 1
#endif
  per_app_init_home(&app->home);

  // add this per_app to the head of global app list
  spin_lock(&global_app_manager.app_list_lock);
  
  if (strcmp(app->app_name, "anon_program") == 0) {
    per_app_list_insert_from_tail(&app->app_list, &global_app_manager.app_list);
  } else {
    list_add(&app->app_list, &global_app_manager.app_list);
  }

  atomic_inc(&global_app_manager.nr_apps);
  spin_unlock(&global_app_manager.app_list_lock);

  // add this per_app to global hash-table
  hash = pid_hash(pid);
  spin_lock(&global_app_manager.hash_lock);
  hlist_add_head(&app->hash_node, &global_app_manager.pid_hash[hash]);
  spin_unlock(&global_app_manager.hash_lock);

  // link the per_app struct to task
  __per_app_set_cached(task, app);
 
#ifdef CONFIG_DEBUG_PAPP
  pr_info("[per_app_create]: created per_app struct for uid %u pid %u (%s)\n",
      from_kuid(&init_user_ns, uid), app->pid, app->app_name);
#endif
  return app;
}

/*
 * find per_app using pid
 * returns struct with incremented ref count
 * this function increments per_app refcount if app was found
 * caller should correctly decrement refcount when done using it
 */
struct per_app *per_app_find(pid_t pid)
{
  struct per_app *app;
  unsigned int hash;

  /* check if system is initialized */
  if (!global_app_manager.pid_hash)
    return NULL;

  hash = pid_hash(pid);
  spin_lock(&global_app_manager.hash_lock);
  hlist_for_each_entry(app, &global_app_manager.pid_hash[hash], hash_node) {
    if (app->pid == pid) {
#ifdef CONFIG_PAPP_USE_KREF      
      per_app_get(app); // increment ref count
#endif
      spin_unlock(&global_app_manager.hash_lock);
      return app;
    }
  }
  spin_unlock(&global_app_manager.hash_lock);
  return NULL;
}

/*
 * detect proc/<pid>/oom_score_adj write
 * update the position of per_app struct in global app list
 * frequently used apps will naturally move towards the head
 */
void per_app_try_to_update_position(int old_oom, int new_oom)
{
  struct per_app *app;
  app = per_app_get_current();
  
  if (!app)
    return;
  
  /* detect app switch: bg -> fg */
  if (old_oom >= 100 && new_oom == 0) {
#ifdef CONFIG_DEBUG_PAPP
    pr_info("[per_app_update_position] per_app with pid %u moved to head\n", app->pid);
#endif
    spin_lock(&global_app_manager.app_list_lock); 
    list_move(&app->app_list, &global_app_manager.app_list);
    spin_unlock(&global_app_manager.app_list_lock); 
  }

  return;
}

/* REVERSE MAPPING */

// maybe inline?
pte_t *page_pte_lazy(struct page *page)
{
  unsigned long mapping = (unsigned long)page->mapping;
  
  if (!mapping) {
    WARN(1, "PAGE MAPPING IS NULL\n");
    return NULL;
    // temp: the bug should be enabled later
    //BUG();
  }
  if ((mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON)
    return NULL;
  return (void *)(mapping - PAGE_MAPPING_ANON);
}

// page->mapping = ptep
// page->index = address
// also save VMA information somewhere (in struct page)
void page_set_anon_rmap_lazy(struct page *page, struct vm_area_struct *vma,
    unsigned long address, pte_t *ptep, int exclusive)
{
  struct anon_vma *anon_vma = vma->anon_vma;
  
  BUG_ON(!anon_vma);

  if (PageAnon(page))
    goto out;

  // TODO: technically, there should be NO non-exclusive page here
  if (likely(exclusive)) {
    //pr_info_once("[LAZY] setting page->mapping as PTE\n");
    
    // single-mapped page: use lazy reverse mapping
    WRITE_ONCE(page->mapping, (struct address_space *)((unsigned long)ptep | PAGE_MAPPING_ANON));
    page->index = address; // not linear address
  } else {
    // shared page: use traditional anon_vma mapping
    anon_vma = anon_vma->root;
    anon_vma = (void *)anon_vma + PAGE_MAPPING_ANON;
    WRITE_ONCE(page->mapping, (struct address_space *)anon_vma);
    page->index = linear_page_index(vma, address);
  }

out:
  if (exclusive)
    SetPageAnonExclusive(page);
}

void page_add_new_anon_rmap_lazy(struct page *page, struct vm_area_struct *vma,
    unsigned long address, pte_t *ptep)
{
  int nr = 1;

  VM_BUG_ON_VMA(address < vma->vm_start || address >= vma->vm_end, vma);
  BUG_ON(PageCompound(page));
  
  //pr_info_once("[LAZY] detected lazy page\n");

  __SetPageSwapBacked(page);
  atomic_set(&page->_mapcount, 0);
  __mod_lruvec_page_state(page, NR_ANON_MAPPED, nr);
  page_set_anon_rmap_lazy(page, vma, address, ptep, 1);
}

// equivalent to page_add_anon_rmap()
// only used for exclusive, swapped-in page (in do_swap_page)
void page_add_anon_rmap_lazy(struct page *page, struct vm_area_struct *vma,
    unsigned long address, pte_t *ptep, rmap_t flags)
{
  bool compound = flags & RMAP_COMPOUND;
  bool first;
  int nr = compound ? thp_nr_pages(page) : 1;
  
  VM_BUG_ON_PAGE(!PageLocked(page), page);
  
  // Handle mapcount increment
  if (compound) {
    atomic_t *mapcount = compound_mapcount_ptr(page);
    first = atomic_inc_and_test(mapcount);
    // should not be compound page...
    BUG();
  } else {
    first = atomic_inc_and_test(&page->_mapcount);
  }
  
  // Validation: must be first mapping (guaranteed for exclusive swap-in)
  VM_BUG_ON_PAGE(!first, page);
  
  // Update statistics (always, since always first mapping)
  if (compound)
    __mod_lruvec_page_state(page, NR_ANON_THPS, nr);
  __mod_lruvec_page_state(page, NR_ANON_MAPPED, nr);
  
  WRITE_ONCE(page->mapping, 
      (struct address_space *)((unsigned long)ptep | PAGE_MAPPING_ANON));
  page->index = address;
  SetPageAnonExclusive(page);
  
  mlock_vma_page(page, vma, compound);
}

int restore_anon_vma_lazy(struct page *page)
{
  pte_t *ptep;
  struct mm_struct *mm;
  struct vm_area_struct *vma;
  struct anon_vma *anon_vma;
  unsigned long address;
  
  //VM_BUG_ON_PAGE(!PageLocked(page), page); 
  
  ptep = page_pte_lazy(page);
  mm = get_page_mm(ptep);
  address = page->index;
  vma = find_vma(mm, address);
  
  if (!mm) {
    pr_info("INVALID mm\n");
    //return 1;
  }
  if (!vma) {
    pr_info("INVALID vma\n");
  }
  
  anon_vma = vma->anon_vma->root;
  BUG_ON(!anon_vma);
  
  lock_page(page);
  anon_vma = (void *)anon_vma + PAGE_MAPPING_ANON;
  WRITE_ONCE(page->mapping, (struct address_space *)anon_vma);
  page->index = linear_page_index(vma, address);
  unlock_page(page);

  return 0;
}

// DEBUG
void lazy_rmap_debug_event(struct page *page, const char *event, pte_t *new_ptep)
{
  pte_t *cached;

  if (!PagePerApp(page) || !PageAnon(page))
    return;
  
  pr_warn("[LAZY RMAP DEBUG EVENT]: anonymous and per-app page for event %s\n", event);

  cached = page_pte_lazy(page);
  if (!cached) {
    pr_warn("[LAZY RMAP DEBUG EVENT]: page->mapping is NULL\n");
    return;
  }
  if (page != pte_page(*cached)) {
    pr_warn("[LAZY RMAP DEBUG EVENT]: at %s, page=%p ref=%d mapcount=%d old_ptep=%p new_ptep=%p\n",
      event, page, page_ref_count(page), page_mapcount(page), cached, new_ptep);
    dump_stack();
  }
}

// END DEBUG

/* update page->mapping to the new PTE pointer (e.g. in move_ptes() triggered by mremap) */
void page_update_rmap_lazy(struct page *page, pte_t *ptep, unsigned long addr)
{
  BUG_ON(page != pte_page(*ptep));

  WRITE_ONCE(page->mapping, (struct address_space *)((unsigned long)ptep | PAGE_MAPPING_ANON));
  page->index = addr;
}


// -- DEPRECATED: we are now using PTE pointer instead of VMA --
/*
 * page sanity check
 */
void per_app_page_check_anon_rmap(struct page *page, 
    struct vm_area_struct *vma, unsigned long address)
{
  struct folio *folio = page_folio(page);
  VM_BUG_ON_FOLIO(per_app_folio_vma(folio) != vma, folio);
  VM_BUG_ON_PAGE(page_to_pgoff(page) != linear_page_index(vma, address), page);
}

/*
 * set page->mapping to vma (instead of anon_vma)
 */
void per_app_set_anon_rmap_vma(struct page *page, struct vm_area_struct *vma, unsigned long address, int exclusive) {
  struct vm_area_struct *page_vma = vma;
  if (PageAnon(page))
    goto out;

  if (!vma)
    BUG();
  if (!vma->vm_mm) 
    BUG();
  
  page_vma = (void *) page_vma + PAGE_MAPPING_ANON;
  WRITE_ONCE(page->mapping, (struct address_space *) page_vma);
  page->index = linear_page_index(vma, address);
out:
  if (exclusive)
    SetPageAnonExclusive(page);
}

/*
 * per_app version of page_add_new_anon_rmap()
 * Use VMA instead of anon_vma
 */
void per_app_add_new_anon_rmap_vma(struct page *page, struct vm_area_struct *vma, 
    unsigned long address) {
  const bool compound = PageCompound(page);
  int nr = compound ? thp_nr_pages(page) : 1; 

  VM_BUG_ON_VMA(address < vma->vm_start || address >= vma->vm_end, vma);
  __SetPageSwapBacked(page);
  if (compound) {
    VM_BUG_ON_PAGE(!PageTransHuge(page), page);
    /* increment count (starts at -1) */
    atomic_set(compound_mapcount_ptr(page), 0);
    atomic_set(compound_pincount_ptr(page), 0);

    __mod_lruvec_page_state(page, NR_ANON_THPS, nr); 
  } else {
    /* increment count (starts at -1) */
    atomic_set(&page->_mapcount, 0);
  }
  //__mod_lruvec_page_state(page, NR_ANON_MAPPED, nr); 
  per_app_set_anon_rmap_vma(page, vma, address, 1);
}

/*
 * connect page->mapping to vma
 */
void per_app_move_anon_rmap_vma(struct page *page, struct vm_area_struct *vma) {
  struct vm_area_struct *page_vma = vma;
  struct folio *folio = page_folio(page);
  
  VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
  VM_BUG_ON_VMA(!page_vma, vma);
  
  page_vma = (void *) page_vma + PAGE_MAPPING_ANON;
  
  WRITE_ONCE(folio->mapping, (struct address_space *) page_vma);
  SetPageAnonExclusive(page);
}

/*
 * create reverse mapping for anon page (anon_vma)
 * called inside do_swap_page
 */
void per_app_add_anon_rmap_vma(struct page *page, struct vm_area_struct *vma,
    unsigned long address, rmap_t flags)
{
  bool compound = flags & RMAP_COMPOUND;
  bool first;

  if (unlikely(PageKsm(page)))
    lock_page_memcg(page);
  else
    VM_BUG_ON_PAGE(!PageLocked(page), page);
  
  if (compound) {
    atomic_t *mapcount;
    VM_BUG_ON_PAGE(!PageLocked(page), page);
    VM_BUG_ON_PAGE(!PageTransHuge(page), page);
    mapcount = compound_mapcount_ptr(page);
    first = atomic_inc_and_test(mapcount);
  } else {
    first = atomic_inc_and_test(&page->_mapcount);
  }
  VM_BUG_ON_PAGE(!first && (flags & RMAP_EXCLUSIVE), page);
  VM_BUG_ON_PAGE(!first && PageAnonExclusive(page), page);

  if (first) {
    int nr = compound ? thp_nr_pages(page) : 1; 
    if (compound)
      __mod_lruvec_page_state(page, NR_ANON_THPS, nr); 
    __mod_lruvec_page_state(page, NR_ANON_MAPPED, nr); 
  }

  if (unlikely(PageKsm(page)))
    unlock_page_memcg(page);

  /* address might be in next vma when migration races vma_adjust */
  else if (first)
    per_app_set_anon_rmap_vma(page, vma, address,
        !!(flags & RMAP_EXCLUSIVE));
  else 
    per_app_page_check_anon_rmap(page, vma, address);

  mlock_vma_page(page, vma, compound);
}

/*
 * restore the anon_vma page->mapping
 * caller should hold page lock
 */
void per_app_restore_anon_rmap(struct page *page, struct vm_area_struct *vma) 
{
  if (!vma)
    BUG();

  /* restore anonymous page mapping (anon_vma) for this page */
  if (PageAnon(page) && vma && vma->anon_vma) {
    struct folio *folio = page_folio(page);
    if (!folio_trylock(folio)) {
      pr_warn("[per_app_restore_anon_rmap] could not lock folio\n");
    } else {
      /* restore normal anon_vma page reverse mapping */
      void *anon_vma = vma->anon_vma;
      anon_vma = (void *)anon_vma + PAGE_MAPPING_ANON;
      WRITE_ONCE(folio->mapping, (struct address_space *)anon_vma);
      folio_unlock(folio);
    }
  }
}

/* END OF REVERSE MAPPING */

// return 0 on success
int per_app_add_file_page(struct page *page, struct per_app *app) {

  VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);
  VM_BUG_ON_PAGE(PageLRU(page), page);
  
  if (!app ||!page)
    return -EINVAL;

  // if page is already being managed by per_app, skip or error
  if (TestSetPagePerApp(page))
    return -EEXIST;

  // the page may have been managed by LRU before (shared -> per_app case)
  // in that case, remove the lru link and add to page list.
  if (WARN_ON_ONCE(TestClearPageLRU(page))) {
    pr_warn("[per_app_add_file_page]: this per-app page was managed by lru.\n");
    //list_del_init(&page->lru);
  }
  
  TestClearPageActive(page);
  get_page(page);
  spin_lock(&app->file_page_list_lock);
  list_add_tail(&page->lru, &app->file_page_list);
  atomic_long_inc(&app->nr_pages);
  atomic_long_inc(&app->nr_file_pages);
  spin_unlock(&app->file_page_list_lock);
  put_page(page);

  return 0;
}

/*
 * add page to application's page list
 * @app: per_app struct for the application
 * @page: page to add
 *
 * exploits the page->lru field to link the page
 * page is either managed by lru or page list
 *
 * returns 0 on success, error code for failure
 */
int per_app_add_page(struct page *page, struct per_app *app) {
  
  VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);
  VM_BUG_ON_PAGE(PageLRU(page), page);
  
  if (!app ||!page)
    return -EINVAL;
  
  // if page is already being managed by per_app, skip or error
  if (TestSetPagePerApp(page))
    return -EEXIST;

  // the page may have been managed by LRU before (shared -> per_app case)
  // in that case, remove the lru link and add to page list.
  if (WARN_ON_ONCE(TestClearPageLRU(page))) {
    pr_warn("[per_app_add_page]: this per-app page was managed by lru.\n");
    //list_del_init(&page->lru);
  }
  
  TestClearPageActive(page);
  get_page(page);

  spin_lock(&app->page_list_lock);
  list_add_tail(&page->lru, &app->page_list);
  atomic_long_inc(&app->nr_pages);
  if (PageAnon(page)) {
    atomic_long_inc(&app->nr_anon_pages);
  } else {
    atomic_long_inc(&app->nr_file_pages);
  }
  spin_unlock(&app->page_list_lock);

  put_page(page);

  return 0;
}

/*
 * per-app version of add page to lru
 */
int per_app_add_page_vma(struct page *page, struct vm_area_struct *vma, struct per_app *app)
{
  int ret = -1; // mlocked or other cases (page add failure)

  VM_BUG_ON_PAGE(PageLRU(page), page);
  
  if (unlikely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) == VM_LOCKED))
    mlock_new_page(page);
  else 
    ret = per_app_add_page(page, app);
  return ret;
}

void per_app_remove_file_page(struct per_app *app, struct page *page)
{
  if (!app || !page)
    return;
  if (WARN_ON_ONCE(PageLRU(page))) {
    pr_warn("[per_app_remove_file_page]: removing LRU page, not per-app\n");
  }
  
  spin_lock(&app->file_page_list_lock);
  list_del(&page->lru);
  atomic_long_dec(&app->nr_pages);
  atomic_long_dec(&app->nr_file_pages);
  spin_unlock(&app->file_page_list_lock);

  ClearPagePerApp(page);
}

/*
 * remove page from page list
 */
void per_app_remove_page(struct per_app *app, struct page *page)
{
  if (!app || !page)
    return;
  if (WARN_ON_ONCE(PageLRU(page))) {
    pr_warn("[per_app_remove_page]: removing LRU page, not per-app\n");
  }
  
  spin_lock(&app->page_list_lock);
  list_del(&page->lru);
  atomic_long_dec(&app->nr_pages);
  atomic_long_dec(&app->nr_anon_pages);
  spin_unlock(&app->page_list_lock);

  ClearPagePerApp(page);
}


/*
 * move this page from app's page list to lru list
 * caller should hold page lock
 */
void per_app_move_page_to_lru(struct per_app *app, struct page *page, struct vm_area_struct *vma)
{
  struct folio *folio = page_folio(page);
  
  if (folio_test_anon(folio)) {
    per_app_remove_page(app, page);
  } else {
    per_app_remove_file_page(app, page);
  }
  
  if (vma) {
    folio_add_lru_vma(folio, vma);
  } else {
    folio_add_lru(folio);
  }

  // update stats?
  /*
  if (PageAnon(page)) {
    __mod_lruvec_page_state(page, NR_ANON_MAPPED, 1);
  } else {
    __mod_lruvec_page_state(page, NR_FILE_MAPPED, 1);
  }
  */
}

#ifdef CONFIG_PAPP_USE_KREF      
/*
 * increment ref count of per_app
 */
struct per_app *per_app_get(struct per_app *app)
{
  if (app)
    kref_get(&app->kref);
  return app;
}

/* 
 * decrement ref count of per_app
 */
void per_app_put(struct per_app *app)
{
  if (app)
    kref_put(&app->kref, per_app_release);
}
#endif

/*
 * release function for kref
 */
#ifdef CONFIG_PAPP_USE_KREF      
static void per_app_release(struct kref *kref)
{
  struct per_app *app = container_of(kref, struct per_app, kref);

  /* remove from hash-table */
  spin_lock(&global_app_manager.hash_lock);
  hlist_del(&app->hash_node);
  spin_unlock(&global_app_manager.hash_lock);

  /* remove from app list */
  spin_lock(&global_app_manager.app_list_lock);
  list_del(&app->app_list);
  atomic_dec(&global_app_manager.nr_apps);
  spin_unlock(&global_app_manager.app_list_lock);

  /* free per_app struct */
  kfree(app);
}
#else
static void per_app_release(struct per_app *app)
{
  /* remove from hash-table */
  spin_lock(&global_app_manager.hash_lock);
  hlist_del(&app->hash_node);
  spin_unlock(&global_app_manager.hash_lock);

  /* remove from app list */
  spin_lock(&global_app_manager.app_list_lock);
  list_del(&app->app_list);
  atomic_dec(&global_app_manager.nr_apps);
  spin_unlock(&global_app_manager.app_list_lock);
  
  // free per_app struct
  //kfree(app);
}
#endif


/*
 * handle process exit for per_app
 */
void per_app_process_exit(struct task_struct *task)
{
  struct per_app *app;
  //int nr_processes;
  int nr_tasks;

  app = __per_app_get_cached(task);
  if (!app)
    return;
  
  nr_tasks = atomic_dec_return(&app->nr_tasks);

  per_app_clear_cached(task);

  if (nr_tasks == 0) {
    per_app_release(app);
  }
}



/*
 * get per_app for current process
 * return NULL if this process/app is not managed by per_app system
 */
struct per_app *per_app_get_current(void)
{
	struct per_app *app = __per_app_get_cached(current);
#ifdef CONFIG_PAPP_USE_KREF      
  if (app) {
    per_app_get(app);
  }
#endif
  return app;
}

/*
 * select target app for memory reclaim
 * return NULL if no app available
 */
struct per_app *per_app_select_app(void) 
{
  struct per_app *app, *target = NULL;
  reclaim_state_t app_state, min_state = RECLAIM_STATE_OOM;
  
  spin_lock(&global_app_manager.app_list_lock);
  /* no app exists */
  if (list_empty(&global_app_manager.app_list)) {
    spin_unlock(&global_app_manager.app_list_lock);
    return NULL;
  }
  /* select target app for reclaim */
  list_for_each_entry_reverse(app, &global_app_manager.app_list, app_list) {
    app_state = app->reclaim_state;
    /* skip app if alreay OOM? */
    /* TODO: kill instead of skipping? */
    if (app_state >= RECLAIM_STATE_OOM)
      continue;
    /* 
     * select an app that has
     * 1) lowest reclaim state
     * 2) if tied, app that is more towards the tail
     * TODO: is there a way to do this more efficiently, without scanning the entire app list?
     */
    if (app_state < min_state) {
      target = app;
      min_state = app_state;
    }
  }
  spin_unlock(&global_app_manager.app_list_lock);
  
  /* TODO: we may not need this. If so, remove per_app_put from per_app_shrink_node.. */
#ifdef CONFIG_PAPP_USE_KREF      
  if (target)
    per_app_get(target);
#endif

  return target;
}

struct per_app *per_app_select_app_baseline(void)
{
  struct per_app *app, *target = NULL;
  
  spin_lock(&global_app_manager.app_list_lock);
  /* no app exists */
  if (list_empty(&global_app_manager.app_list)) {
    spin_unlock(&global_app_manager.app_list_lock);
    return NULL;
  }
  /* select target app for reclaim */
  list_for_each_entry_reverse(app, &global_app_manager.app_list, app_list) {
    /* 
     * select app from tail
     * if not enough pages exist, select the next tail app
     */
    unsigned long nr_pages = per_app_get_page_count(app);
    if (nr_pages >= PER_APP_MIN_RECLAIM_AMOUNT && app->reclaimed == false) {
      app->reclaimed = true;
      target = app;
      break;
    }
  }
  spin_unlock(&global_app_manager.app_list_lock);
  
  return target;
}

void per_app_reset_reclaimed(void)
{
  struct per_app *app;
  spin_lock(&global_app_manager.app_list_lock);
  list_for_each_entry_reverse(app, &global_app_manager.app_list, app_list) {
    if (app->reclaimed == true) {
      app->reclaimed = false;
    }
  }
  spin_unlock(&global_app_manager.app_list_lock);
}



// calculate how many pages can be reclaimed (from one session)
unsigned long per_app_calculate_nr_to_scan(struct per_app *app)
{
  unsigned long pages_to_reclaim;
  unsigned long nr_reclaimed = atomic_long_read(&app->nr_reclaimed);
  unsigned long nr_pages = atomic_long_read(&app->nr_pages);
  reclaim_state_t app_state = app->reclaim_state;

  /* TODO: kill this app? */
  if (app_state >= RECLAIM_STATE_OOM)
    return 0UL;
  pages_to_reclaim = (nr_pages >> reclaim_ratio[app_state]) - nr_reclaimed;
  return max(pages_to_reclaim, 0UL);
}

void per_app_try_to_advance_reclaim_state(struct per_app *app, unsigned long nr_reclaimed)
{
  /* account for number of pages reclaimed in one session */
  unsigned long app_reclaimed_pages = atomic_long_add_return(nr_reclaimed, &app->nr_reclaimed);
  while (app->reclaim_state < RECLAIM_STATE_OOM && 
         app_reclaimed_pages >= (per_app_get_page_count(app) >> reclaim_ratio[app->reclaim_state])) {
    app->reclaim_state++;
  }
}

/*
 * advance reclaim state of an app regardless of app->nr_reclaimed
 * prevents infinite looping of selecting an app that cannot be reclaimed
 */
void per_app_advance_reclaim_state(struct per_app *app)
{
  if (app->reclaim_state < RECLAIM_STATE_OOM)
    app->reclaim_state++;
}

/* Debug helper function to print per_app reclaim status */
void per_app_print_reclaim_status(struct per_app *app)
{
  unsigned long nr_pages = atomic_long_read(&app->nr_pages);
  unsigned long nr_reclaimed = atomic_long_read(&app->nr_reclaimed);
  unsigned long nr_anon_pages = atomic_long_read(&app->nr_anon_pages);
  unsigned long nr_file_pages = atomic_long_read(&app->nr_file_pages);
  
  pr_info("  [per_app_debug] UID: %u, State: %d, Pages: %lu (anon: %lu, file: %lu), Reclaimed: %lu\n",
          from_kuid(&init_user_ns, app->uid), 
          app->reclaim_state,
          nr_pages, nr_anon_pages, nr_file_pages, nr_reclaimed);
}

#ifdef CONFIG_PROC_FS
static int per_app_proc_show(struct seq_file *m, void *v)
{
  struct per_app *app;
  unsigned long nr_total_pages = 0, nr_total_reclaimed = 0;

  seq_printf(m, "Per-App Memory Management Statistics\n");
  seq_printf(m, "====================================\n");
  
  spin_lock(&global_app_manager.app_list_lock);
  list_for_each_entry(app, &global_app_manager.app_list, app_list) {
    seq_printf(m, "UID: %u\n", from_kuid(&init_user_ns, app->uid));
    seq_printf(m, "PID: %u\n", app->pid);
    seq_printf(m, "App Name: %s\n", app->app_name);
    seq_printf(m, "Active Tasks: %d\n", atomic_read(&app->nr_tasks));
    seq_printf(m, "Total Pages: %ld\n", atomic_long_read(&app->nr_pages));
    seq_printf(m, "  - Anonymous: %ld\n", atomic_long_read(&app->nr_anon_pages));
    seq_printf(m, "  - File-backed: %ld\n", atomic_long_read(&app->nr_file_pages));
    seq_printf(m, "Total Reclaimed Pages: %ld\n", atomic_long_read(&app->nr_reclaimed));
    seq_printf(m, "  - Reclaimed Anonymous: %ld\n", atomic_long_read(&app->nr_anon_reclaimed));
    seq_printf(m, "  - ReclaimedFile-backed: %ld\n", atomic_long_read(&app->nr_file_reclaimed));
    seq_printf(m, "App reclaim state: R%d\n", app->reclaim_state);
#ifdef CONFIG_PAPP_USE_KREF
    seq_printf(m, "App Reference Count: %d\n", kref_read(&app->kref));
#endif
    seq_printf(m, "---\n");
    nr_total_pages += atomic_long_read(&app->nr_pages);
    nr_total_reclaimed += atomic_long_read(&app->nr_reclaimed);
  }
  seq_printf(m, "Total apps tracked: %d\n\n", 
    atomic_read(&global_app_manager.nr_apps));
  seq_printf(m, "Total per-app pages: %lu\n", nr_total_pages);
  seq_printf(m, "Total per-app reclaimed pages: %lu\n", nr_total_reclaimed);
  seq_printf(m, "====================================\n");
  spin_unlock(&global_app_manager.app_list_lock);
  
  return 0;
}

static int per_app_proc_open(struct inode *inode, struct file *file)
{
  return single_open(file, per_app_proc_show, NULL);
}

static const struct proc_ops per_app_proc_ops = {
  .proc_open = per_app_proc_open,
  .proc_read = seq_read,
  .proc_lseek = seq_lseek,
  .proc_release = single_release,
};

static int __init per_app_create_proc_entry(void)
{
  struct proc_dir_entry *entry;
  
  entry = proc_create("per_app_stats", 0444, NULL, &per_app_proc_ops);
  if (!entry) {
    pr_err("[perapp]: failed to create proc entry\n");
    return -ENOMEM;
  }
  
  pr_info("[perapp]: created /proc/per_app_stats\n");
  return 0;
}

#endif /* CONFIG_PROC_FS */

// 0: no match, home dentry should be still NULL
// 1: match found, home dentry now stores something (e.g. /data/user/0/<package_name>)
// either update to full package or task->comm
int per_app_update_package_name(struct per_app *app, char *name)
{
  int i;
  for (i=0; i<num_packages; i++) {
    if (strstr(packages[i], name)) {
      // found match
      strncpy(app->app_name, packages[i], PACKAGE_NAME_LEN - 1);
      app->app_name[PACKAGE_NAME_LEN - 1] = '\0';
      return 1;
    }
  }
  // no match, set to name (task->comm)
  strncpy(app->app_name, name, PACKAGE_NAME_LEN - 1);
  app->app_name[PACKAGE_NAME_LEN - 1] = '\0';
  return 0;
}

/* 
 * create per_app struct upon fork()
 * return per_app instead of void??
 */
void per_app_process_fork(struct task_struct *parent, struct task_struct *child)
{
  struct per_app *app;
  kuid_t child_uid;
  pid_t child_pid;
  pid_t child_tid;
  
  /* skip kernel threads */
  if (!child->mm)
    return;
  
  child_uid = task_uid(child);
  child_pid = task_tgid_nr(child);
  child_tid = task_pid_nr(child);

  /* filter applications of interest */
  if (!per_app_is_target_uid(from_kuid(&init_user_ns, child_uid)))
    return;
  
  if (unlikely(child_pid == child_tid)) {
    app = per_app_find(child_pid);
    if (unlikely(app)) {
      WARN(1, "[per_app_process_fork] per_app struct already exists for main thread with PID %u UID %u\n",
              child_pid, from_kuid(&init_user_ns, child_uid));
      //BUG();
    } else {
      per_app_create(child);
    }
    
  } else {
    // find and link the per_app struct to this new task
    app = per_app_find(child_pid);
    if (likely(app)) {
      __per_app_set_cached(child, app);
      atomic_inc(&app->nr_tasks);
      if (strcmp(app->app_name, "main") == 0) {
        if (per_app_update_package_name(app, parent->comm)) {
          int ret;
          snprintf(app->home.home_path_str, HOME_PATH_MAX, "/data/user/0/%s", app->app_name);
          ret = kern_path(app->home.home_path_str, LOOKUP_FOLLOW, &app->home.home_path);
          if (ret) {
            WARN(1, "[per_app_process_fork] failed to resolve kernel path for %s\n",
                    app->app_name);
          } else {
            //pr_info("[per_app_process_fork] per_app home dentry cache is created\n");
          }
        }
      }
#ifdef CONFIG_PAPP_USE_KREF
      per_app_put(app);
#endif
    } else {
      WARN(1, "[per_app_process_fork]: per_app struct doesn't exist for child thread with PID: %d UID: %u\n",
        child_pid, from_kuid(&init_user_ns, child_uid));
      //BUG();
    }
  }

  return;
}

/*
 * get total page count of an app
 */
unsigned long per_app_get_page_count(struct per_app *app)
{
  return atomic_long_read(&app->nr_pages);
}

unsigned int per_app_get_app_count(void)
{
  return atomic_read(&global_app_manager.nr_apps);
}

/*
 * =======================================
 * procfs write handler for force reclaim
 * =======================================
 */
static ssize_t per_app_reclaim_write(struct file *file, const char __user *buffer, 
    size_t count, loff_t *ppos)
{
    char *kbuf, *token;
    char *args[3] = {NULL, NULL, NULL};  /* uid, type, amount */
    kuid_t uid = INVALID_UID;
    unsigned long uid_val, amount_mb = 0;
    enum per_app_reclaim_type type = RECLAIM_ALL;  /* default to all */
    int ret, argc = 0;
    bool uid_specified = false;
    
    /* Safely copy the user's input into a kernel buffer */
    kbuf = memdup_user_nul(buffer, count);
    if (IS_ERR(kbuf))
        return PTR_ERR(kbuf);
    
    /* Parse space-separated arguments */
    token = kbuf;
    while (token && argc < 3) {
        args[argc] = strsep(&token, " ");
        if (args[argc] && strlen(args[argc]) > 0)
            argc++;
    }
    
    /* We need at least 1 argument (amount) */
    if (argc == 0) {
        pr_warn("[perapp]: invalid format. Use: '[uid] [type] <amount>' or '[type] <amount>' or '<amount>'\n");
        kfree(kbuf);
        return -EINVAL;
    }
    
    /* Parse arguments based on count */
    if (argc == 1) {
        /* Format: "<amount>" */
        ret = kstrtoul(args[0], 10, &amount_mb);
        if (ret) {
            pr_warn("[perapp]: invalid amount specified '%s'\n", args[0]);
            kfree(kbuf);
            return ret;
        }
        /* uid remains INVALID_UID, type remains RECLAIM_ALL */
        
    } else if (argc == 2) {
        /* Format: "<type> <amount>" or "<uid> <amount>" */
        /* Try to parse first argument as UID */
        ret = kstrtoul(args[0], 10, &uid_val);
        if (ret == 0) {
            /* First arg is numeric, assume it's UID */
            uid = make_kuid(&init_user_ns, uid_val);
            uid_specified = true;
            
            /* Second arg must be amount */
            ret = kstrtoul(args[1], 10, &amount_mb);
            if (ret) {
                pr_warn("[perapp]: invalid amount specified '%s'\n", args[1]);
                kfree(kbuf);
                return ret;
            }
        } else {
            /* First arg is not numeric, assume it's type */
            if (strcmp(args[0], "anon") == 0) {
                type = RECLAIM_ANON;
            } else if (strcmp(args[0], "file") == 0) {
                type = RECLAIM_FILE;
            } else if (strcmp(args[0], "all") == 0) {
                type = RECLAIM_ALL;
            } else {
                pr_warn("[perapp]: invalid type specified '%s'. Use 'anon', 'file', or 'all'.\n",
                    args[0]);
                kfree(kbuf);
                return -EINVAL;
            }
            
            /* Second arg must be amount */
            ret = kstrtoul(args[1], 10, &amount_mb);
            if (ret) {
                pr_warn("[perapp]: invalid amount specified '%s'\n", args[1]);
                kfree(kbuf);
                return ret;
            }
        }
        
    } else if (argc == 3) {
        /* Format: "<uid> <type> <amount>" */
        /* Parse UID */
        ret = kstrtoul(args[0], 10, &uid_val);
        if (ret) {
            pr_warn("[perapp]: invalid UID specified '%s'\n", args[0]);
            kfree(kbuf);
            return ret;
        }
        uid = make_kuid(&init_user_ns, uid_val);
        uid_specified = true;
        
        /* Parse reclaim type */
        if (strcmp(args[1], "anon") == 0) {
            type = RECLAIM_ANON;
        } else if (strcmp(args[1], "file") == 0) {
            type = RECLAIM_FILE;
        } else if (strcmp(args[1], "all") == 0) {
            type = RECLAIM_ALL;
        } else {
            pr_warn("[perapp]: invalid type specified '%s'. Use 'anon', 'file', or 'all'.\n",
                args[1]);
            kfree(kbuf);
            return -EINVAL;
        }
        
        /* Parse amount in MB */
        ret = kstrtoul(args[2], 10, &amount_mb);
        if (ret) {
            pr_warn("[perapp]: invalid amount specified '%s'\n", args[2]);
            kfree(kbuf);
            return ret;
        }
    }
    
    kfree(kbuf);
    
    /* Call the main reclaim function in vmscan.c */
    //per_app_force_reclaim(uid, type, amount_mb, uid_specified);
    
    return count;
}

static const struct proc_ops per_app_reclaim_proc_ops = {
  .proc_write = per_app_reclaim_write,
  //.proc_open = default_proc_open,
  //.proc_lseek = default_llseek,
};

/**
 * per_app_create_proc_reclaim_file - Creates the /proc/per_app_reclaim file.
 *
 * This should be called from your main module init function.
 */
static int __init per_app_create_proc_reclaim_file(void)
{
  struct proc_dir_entry *entry;
	
  entry = proc_create("per_app_reclaim", 0222, NULL, &per_app_reclaim_proc_ops);
  if (!entry) {
    pr_err("[perapp]: failed to create proc entry\n");
    return -ENOMEM;
  }
  
  pr_info("[perapp]: created /proc/per_app_reclaim\n");
  return 0;
}

/**
 * per_app_remove_proc_reclaim_file - Removes the /proc/per_app_reclaim file.
 *
 * This should be called from your main module exit function.
 */
void per_app_remove_proc_reclaim_file(void)
{
	remove_proc_entry("per_app_reclaim", NULL);
}

/*
 * initialize per-app subsystem
 * should be called during kernel init
 */
int __init per_app_init_subsystem(void)
{
  int ret;
  pr_info("[perapp]: Initializing per-app memory management subsystem\n");
  // Initialize the manager
  ret = per_app_manager_init();
  if (ret) {
    pr_err("[perapp]: Failed to initialize manager: %d\n", ret);
    return ret;
  }
  
#ifdef CONFIG_PROC_FS
  ret = per_app_create_proc_entry();
  if (ret)
    pr_err("[perapp]: failed to create proc entry: %d\n", ret);
  ret = per_app_create_proc_reclaim_file();
  if (ret)
    pr_err("[perapp]: failed to create proc entry: %d\n", ret);
#endif

  pr_info("[perapp]: Subsystem initialization complete. Tracking %d apps\n",
    atomic_read(&global_app_manager.nr_apps));
  return 0;
}

/*
 * cleanup per-app subsystem
 */
void __exit per_app_exit_subsystem(void)
{
  pr_info("[perapp] system exiting\n");
  per_app_manager_exit();
}


/* initialize per-app system after core memory management */
subsys_initcall(per_app_init_subsystem);
//late_initcall(per_app_init_subsystem);


