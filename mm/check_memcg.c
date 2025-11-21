#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/highmem.h>
#include <linux/pid.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/hugetlb.h>
#include <linux/huge_mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>

#define PROC_NAME "check_memcg"
#define MAX_SAMPLES 1000

struct page_sample {
    unsigned long vma_start;
    unsigned long vma_end;
    unsigned long page_addr;
    unsigned long page_pfn;
    bool is_anon;
    bool is_file;
    int mapcount;
    unsigned long memcg_addr;
    unsigned long lruvec_addr;
    int nid;
    const char *lru_type;
    bool on_lru;
    bool is_private_vma;
    bool is_shared_vma;
};

struct analysis_result {
    int pid;
    int nr_pages_requested;
    int total_vmas;
    int samples_collected;
    bool scan_all_vmas;
    struct page_sample *samples;
    bool valid;
};

static struct analysis_result last_result = {0};
static DEFINE_MUTEX(result_mutex);

static const char* get_lru_type(struct page *page)
{
    if (!PageLRU(page))
        return "NOT_ON_LRU";
    
    if (PageUnevictable(page))
        return "unevictable";
    
    if (PageAnon(page)) {
        if (PageActive(page))
            return "active_anon";
        else
            return "inactive_anon";
    } else {
        if (PageActive(page))
            return "active_file";
        else
            return "inactive_file";
    }
}

static int sample_page_from_vma(struct vm_area_struct *vma, struct mm_struct *mm,
                                 struct page_sample *samples, int *samples_collected,
                                 int max_samples, bool scan_all)
{
    unsigned long addr;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    bool is_anon_vma;
    bool is_private_vma;
    bool is_shared_vma;
    
    if (!scan_all && *samples_collected >= max_samples)
        return 1;
    
    is_anon_vma = !vma->vm_file;
    is_private_vma = vma->vm_file && !(vma->vm_flags & VM_SHARED);
    is_shared_vma = vma->vm_file && (vma->vm_flags & VM_SHARED);
    
    for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
        struct page *page = NULL;
        struct mem_cgroup *memcg = NULL;
        struct lruvec *lruvec = NULL;
        
        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd))
            continue;
        
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d))
            continue;
        
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud))
            continue;
        
        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd))
            continue;
        
        if (pmd_trans_huge(*pmd))
            continue;
        
        pte = pte_offset_map(pmd, addr);
        if (!pte || !pte_present(*pte)) {
            if (pte)
                pte_unmap(pte);
            continue;
        }
        
        page = pte_page(*pte);
        pte_unmap(pte);
        
        if (!page)
            continue;
        
        if (page_mapcount(page) != 1)
            continue;
        
        if (PageReserved(page) || PageSlab(page))
            continue;
        
        rcu_read_lock();
        
        // Kernel 6.1: Use page_memcg() helper
        memcg = page_memcg(page);
        
        // For 4.19 use: memcg = page->mem_cgroup;
        
        if (memcg) {
            int nid = page_to_nid(page);
            
            // Kernel 6.1: mem_cgroup_lruvec(memcg, pgdat)
            lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
            
            // For 4.19: mem_cgroup_lruvec(pgdat, memcg)
            // lruvec = mem_cgroup_lruvec(NODE_DATA(nid), memcg);
            
            samples[*samples_collected].vma_start = vma->vm_start;
            samples[*samples_collected].vma_end = vma->vm_end;
            samples[*samples_collected].page_addr = (unsigned long)page;
            samples[*samples_collected].page_pfn = page_to_pfn(page);
            samples[*samples_collected].is_anon = PageAnon(page);
            samples[*samples_collected].is_file = !PageAnon(page);
            samples[*samples_collected].mapcount = page_mapcount(page);
            samples[*samples_collected].memcg_addr = (unsigned long)memcg;
            samples[*samples_collected].lruvec_addr = (unsigned long)lruvec;
            samples[*samples_collected].nid = nid;
            samples[*samples_collected].lru_type = get_lru_type(page);
            samples[*samples_collected].on_lru = PageLRU(page);
            samples[*samples_collected].is_private_vma = is_private_vma;
            samples[*samples_collected].is_shared_vma = is_shared_vma;
            
            (*samples_collected)++;
        }
        
        rcu_read_unlock();
        
        return 0;
    }
    
    return 0;
}

static int analyze_process_memcg(int pid, int nr_pages, struct analysis_result *result)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    int max_samples;
    int samples_collected = 0;
    int total_vmas = 0;
    bool scan_all;
    struct page_sample *samples = NULL;

    result->valid = false;
    result->pid = pid;
    result->nr_pages_requested = nr_pages;
    
    task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        pr_err("check_memcg: No task found for PID %d\n", pid);
        return -ESRCH;
    }
    
    mm = get_task_mm(task);
    if (!mm) {
        pr_err("check_memcg: Failed to get mm_struct for PID %d\n", pid);
        put_task_struct(task);
        return -EFAULT;
    }
    
    VMA_ITERATOR(vmi, mm, 0);
    
    // Count total VMAs
    mmap_read_lock(mm);
    //for (vma = mm->mmap; vma; vma = vma->vm_next) {
    for_each_vma(vmi, vma) {
        total_vmas++;
    }
    vma_iter_init(&vmi, mm, 0);

    mmap_read_unlock(mm);
    
    result->total_vmas = total_vmas;
    
    // Determine sampling strategy
    if (nr_pages <= 0) {
        scan_all = true;
        max_samples = total_vmas;
    } else {
        scan_all = false;
        max_samples = (nr_pages < total_vmas) ? nr_pages : total_vmas;
    }
    
    if (max_samples > MAX_SAMPLES)
        max_samples = MAX_SAMPLES;
    
    result->scan_all_vmas = scan_all;
    
    samples = kmalloc(max_samples * sizeof(struct page_sample), GFP_KERNEL);
    if (!samples) {
        pr_err("check_memcg: Failed to allocate samples array\n");
        mmput(mm);
        put_task_struct(task);
        return -ENOMEM;
    }
    
    mmap_read_lock(mm);
    //for (vma = mm->mmap; vma; vma = vma->vm_next) {
    for_each_vma(vmi, vma) {
        if (!scan_all && samples_collected >= max_samples)
            break;
        sample_page_from_vma(vma, mm, samples, &samples_collected, max_samples, scan_all);
    }
    mmap_read_unlock(mm);
    
    result->samples_collected = samples_collected;
    result->samples = samples;
    result->valid = true;
    
    mmput(mm);
    put_task_struct(task);
    
    return 0;
}

static void print_analysis_to_log(struct analysis_result *result)
{
    int i;
    unsigned long first_memcg = 0, first_lruvec = 0;
    bool all_same_memcg = true;
    bool all_same_lruvec = true;
    int anon_count = 0, file_count = 0;
    int active_anon = 0, inactive_anon = 0;
    int active_file = 0, inactive_file = 0;
    
    pr_info("\n========================================\n");
    pr_info("MEMCG/LRU Analysis for PID %d\n", result->pid);
    pr_info("========================================\n");
    pr_info("Total VMAs: %d\n", result->total_vmas);
    pr_info("Samples requested: %s\n", 
            result->scan_all_vmas ? "ALL VMAs" : 
            (result->nr_pages_requested > 0 ? "specific count" : "ALL VMAs"));
    pr_info("Samples collected: %d\n", result->samples_collected);
    pr_info("========================================\n");
    
    if (result->samples_collected == 0) {
        pr_info("No suitable pages found!\n");
        pr_info("========================================\n");
        return;
    }
    
    // Analyze samples
    for (i = 0; i < result->samples_collected; i++) {
        if (result->samples[i].is_anon)
            anon_count++;
        else
            file_count++;
        
        if (strcmp(result->samples[i].lru_type, "active_anon") == 0)
            active_anon++;
        else if (strcmp(result->samples[i].lru_type, "inactive_anon") == 0)
            inactive_anon++;
        else if (strcmp(result->samples[i].lru_type, "active_file") == 0)
            active_file++;
        else if (strcmp(result->samples[i].lru_type, "inactive_file") == 0)
            inactive_file++;
        
        if (i == 0) {
            first_memcg = result->samples[i].memcg_addr;
            first_lruvec = result->samples[i].lruvec_addr;
        } else {
            if (result->samples[i].memcg_addr != first_memcg)
                all_same_memcg = false;
            if (result->samples[i].lruvec_addr != first_lruvec)
                all_same_lruvec = false;
        }
    }
    
    pr_info("\nPage Type Distribution:\n");
    pr_info("  Anonymous pages: %d\n", anon_count);
    pr_info("  File-backed pages: %d\n", file_count);
    pr_info("\nLRU List Distribution:\n");
    pr_info("  active_anon: %d\n", active_anon);
    pr_info("  inactive_anon: %d\n", inactive_anon);
    pr_info("  active_file: %d\n", active_file);
    pr_info("  inactive_file: %d\n", inactive_file);
    
    pr_info("\n========================================\n");
    pr_info("MEMORY CGROUP & LRU ANALYSIS:\n");
    pr_info("========================================\n");
    
    if (all_same_memcg) {
        pr_info("✓ All pages belong to SAME mem_cgroup (0x%lx)\n", first_memcg);
    } else {
        pr_info("✗ Pages belong to DIFFERENT mem_cgroups!\n");
        pr_info("  Unique memcg addresses:\n");
        for (i = 0; i < result->samples_collected; i++) {
            int j;
            bool duplicate = false;
            for (j = 0; j < i; j++) {
                if (result->samples[j].memcg_addr == result->samples[i].memcg_addr) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                pr_info("    - 0x%lx\n", result->samples[i].memcg_addr);
        }
    }
    
    if (all_same_lruvec) {
        pr_info("✓ All pages belong to SAME lruvec (0x%lx)\n", first_lruvec);
        pr_info("  => This process uses a SINGLE set of LRU lists\n");
    } else {
        pr_info("✗ Pages belong to DIFFERENT lruvecs!\n");
        pr_info("  => This process uses MULTIPLE sets of LRU lists\n");
        pr_info("  Unique lruvec addresses:\n");
        for (i = 0; i < result->samples_collected; i++) {
            int j;
            bool duplicate = false;
            for (j = 0; j < i; j++) {
                if (result->samples[j].lruvec_addr == result->samples[i].lruvec_addr) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                pr_info("    - 0x%lx\n", result->samples[i].lruvec_addr);
        }
    }
    
    pr_info("\n");
    if (all_same_memcg && all_same_lruvec) {
        pr_info("CONCLUSION: This device appears to use a GLOBAL LRU system\n");
        pr_info("            All processes share the same memory cgroup and LRU lists.\n");
        pr_info("            No per-process or per-app LRU isolation detected.\n");
    } else {
        pr_info("CONCLUSION: This device uses per-cgroup LRU isolation\n");
        pr_info("            Different memory cgroups maintain separate LRU lists.\n");
    }
    
    pr_info("========================================\n\n");
}

static ssize_t check_memcg_write(struct file *file, const char __user *buffer,
                                  size_t count, loff_t *ppos)
{
    char input[64];
    int pid = -1;
    int nr_pages = -1;
    char *comma;
    int ret;
    struct analysis_result new_result = {0};
    
    if (count >= sizeof(input))
        return -EINVAL;
    
    if (copy_from_user(input, buffer, count))
        return -EFAULT;
    
    input[count] = '\0';
    
    // Remove trailing newline
    if (count > 0 && input[count - 1] == '\n')
        input[count - 1] = '\0';
    
    // Parse input: "PID" or "PID,nr_pages"
    comma = strchr(input, ',');
    if (comma) {
        *comma = '\0';
        ret = kstrtoint(input, 10, &pid);
        if (ret) {
            pr_err("check_memcg: Invalid PID format\n");
            return -EINVAL;
        }
        ret = kstrtoint(comma + 1, 10, &nr_pages);
        if (ret) {
            pr_err("check_memcg: Invalid nr_pages format\n");
            return -EINVAL;
        }
    } else {
        ret = kstrtoint(input, 10, &pid);
        if (ret) {
            pr_err("check_memcg: Invalid PID format\n");
            return -EINVAL;
        }
        nr_pages = -1; // Scan all VMAs
    }
    
    if (pid <= 0) {
        pr_err("check_memcg: Invalid PID %d\n", pid);
        return -EINVAL;
    }
    
    if (nr_pages > MAX_SAMPLES) {
        pr_err("check_memcg: nr_pages too large (max: %d)\n", MAX_SAMPLES);
        return -EINVAL;
    }
    
    pr_info("check_memcg: Analyzing PID %d with nr_pages=%d\n", pid, nr_pages);
    
    ret = analyze_process_memcg(pid, nr_pages, &new_result);
    if (ret) {
        pr_err("check_memcg: Analysis failed with error %d\n", ret);
        return ret;
    }
    
    // Print results to kernel log
    print_analysis_to_log(&new_result);
    
    // Save results for potential read operation
    mutex_lock(&result_mutex);
    if (last_result.samples)
        kfree(last_result.samples);
    last_result = new_result;
    mutex_unlock(&result_mutex);
    
    return count;
}

static int check_memcg_show(struct seq_file *m, void *v)
{
    mutex_lock(&result_mutex);
    
    if (!last_result.valid) {
        seq_printf(m, "No analysis has been performed yet.\n");
        seq_printf(m, "Usage: echo PID[,nr_pages] > /proc/%s\n", PROC_NAME);
        seq_printf(m, "Example: echo 1234 > /proc/%s\n", PROC_NAME);
        seq_printf(m, "Example: echo 1234,10 > /proc/%s\n", PROC_NAME);
    } else {
        seq_printf(m, "Last analysis for PID %d\n", last_result.pid);
        seq_printf(m, "Total VMAs: %d\n", last_result.total_vmas);
        seq_printf(m, "Samples collected: %d\n", last_result.samples_collected);
        seq_printf(m, "\nCheck 'dmesg' or kernel log for detailed results.\n");
    }
    
    mutex_unlock(&result_mutex);
    return 0;
}

static int check_memcg_open(struct inode *inode, struct file *file)
{
    return single_open(file, check_memcg_show, NULL);
}

static const struct proc_ops check_memcg_fops = {
    .proc_open = check_memcg_open,
    .proc_read = seq_read,
    .proc_write = check_memcg_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init check_memcg_init(void)
{
    struct proc_dir_entry *entry;
    
    entry = proc_create(PROC_NAME, 0666, NULL, &check_memcg_fops);
    if (!entry) {
        pr_err("check_memcg: Failed to create /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }
    
    pr_info("check_memcg: Initialized. Use: echo PID[,nr_pages] > /proc/%s\n", PROC_NAME);
    
    return 0;
}

fs_initcall(check_memcg_init);
