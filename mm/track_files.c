#include <linux/track_files.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/cred.h>

/* for sys fs */
#include <linux/sysfs.h>
#include <linux/kobject.h>

#ifdef CONFIG_TRACK_FILES

// Hash table to track seen files
#define PAPP_FILE_HASH_BITS 10
static DEFINE_HASHTABLE(tracked_files, PAPP_FILE_HASH_BITS);
static DEFINE_SPINLOCK(tracked_files_lock);

// Target UID to track (NULL means tracking disabled)
static kuid_t target_file_uid = INVALID_UID;
static bool tracking_enabled = false;
static DEFINE_SPINLOCK(target_uid_lock);

struct tracked_file_entry {
    unsigned long ino;
    dev_t dev;
    char pathname[256];  // Store the path for later reference
    char filename[64];
    char extension[16];
    struct hlist_node hash_node;
};

// Clear all tracked files
static void clear_tracked_files(void)
{
    struct tracked_file_entry *entry;
    struct hlist_node *tmp;
    int bkt;
    
    spin_lock(&tracked_files_lock);
    
    hash_for_each_safe(tracked_files, bkt, tmp, entry, hash_node) {
        hash_del(&entry->hash_node);
        kfree(entry);
    }
    
    spin_unlock(&tracked_files_lock);
    
    printk(KERN_INFO "Cleared all tracked files\n");
}

// Check if file is already tracked, if not add it
bool track_file_if_new(struct file *file)
{
    struct inode *inode = file->f_inode;
    unsigned long ino = inode->i_ino;
    dev_t dev = inode->i_sb->s_dev;
    struct tracked_file_entry *entry;
    bool is_new = true;
    char buf[256];
    char *pathname;
    const char *filename;
    const char *ext;
    
    spin_lock(&tracked_files_lock);
    
    // Check if already tracked
    hash_for_each_possible(tracked_files, entry, hash_node, ino) {
        if (entry->ino == ino && entry->dev == dev) {
            is_new = false;
            break;
        }
    }
    
    if (is_new) {
        // Allocate new entry
        entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
        if (!entry) {
            spin_unlock(&tracked_files_lock);
            return false;
        }
        
        // Get file information
        pathname = d_path(&file->f_path, buf, sizeof(buf));
        if (IS_ERR(pathname)) {
            pathname = "(unknown)";
        }
        
        filename = file->f_path.dentry->d_name.name;
        ext = strrchr(filename, '.');
        
        // Fill entry
        entry->ino = ino;
        entry->dev = dev;
        strscpy(entry->pathname, pathname, sizeof(entry->pathname));
        strscpy(entry->filename, filename, sizeof(entry->filename));
        strscpy(entry->extension, ext ? ext : "(none)", sizeof(entry->extension));
        
        // Add to hash table
        hash_add(tracked_files, &entry->hash_node, ino);
        
        // Log the new file
        printk(KERN_INFO "TRACKED_FILE: uid=%u path=%s name=%s ext=%s ino=%lu\n",
               from_kuid(&init_user_ns, current_uid()),
               entry->pathname, entry->filename, entry->extension, ino);
    }
    
    spin_unlock(&tracked_files_lock);
    
    return is_new;
}

// Set target UID (called from sysfs or proc interface)
static int set_target_uid(uid_t uid)
{
    spin_lock(&target_uid_lock);
    
    // Clear previous tracking data
    if (tracking_enabled) {
        clear_tracked_files();
    }
    
    if (uid == (uid_t)-1) {
        // Disable tracking
        tracking_enabled = false;
        target_file_uid = INVALID_UID;
        printk(KERN_INFO "File tracking disabled\n");
    } else {
        // Enable tracking for this UID
        target_file_uid = make_kuid(&init_user_ns, uid);
        tracking_enabled = true;
        printk(KERN_INFO "File tracking enabled for UID %u\n", uid);
    }
    
    spin_unlock(&target_uid_lock);
    
    return 0;
}

// Check if current task matches target UID
bool should_track_current_task(void)
{
    kuid_t current_uid;
    bool should_track = false;
    
    if (!tracking_enabled)
        return false;
    
    current_uid = current_uid();
    
    spin_lock(&target_uid_lock);
    should_track = uid_eq(current_uid, target_file_uid);
    spin_unlock(&target_uid_lock);
    
    return should_track;
}
static struct kobject *file_tracking_kobj;

// Sysfs: read current target UID
static ssize_t target_uid_show(struct kobject *kobj,
                                struct kobj_attribute *attr,
                                char *buf)
{
    ssize_t ret;

    spin_lock(&target_uid_lock);
    if (tracking_enabled) {
        ret = sprintf(buf, "%u\n", from_kuid(&init_user_ns, target_file_uid));
    } else {
        ret = sprintf(buf, "disabled\n");
    }
    spin_unlock(&target_uid_lock);

    return ret;
}

// Sysfs: write target UID
static ssize_t target_uid_store(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    uid_t uid;
    int ret;

    if (strncmp(buf, "disable", 7) == 0 || strncmp(buf, "-1", 2) == 0) {
        set_target_uid((uid_t)-1);
        return count;
    }

    ret = kstrtouint(buf, 10, &uid);
    if (ret)
        return ret;

    set_target_uid(uid);

    return count;
}

// Sysfs: dump all tracked files
static ssize_t tracked_files_show(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   char *buf)
{
    struct tracked_file_entry *entry;
    int bkt;
    ssize_t len = 0;

    spin_lock(&tracked_files_lock);

    hash_for_each(tracked_files, bkt, entry, hash_node) {
        len += scnprintf(buf + len, PAGE_SIZE - len,
                        "%s|%s|%s|%lu\n",
                        entry->pathname, entry->filename,
                        entry->extension, entry->ino);

        if (len >= PAGE_SIZE - 100)
            break;
    }

    spin_unlock(&tracked_files_lock);

    return len;
}

static struct kobj_attribute target_uid_attr =
    __ATTR(target_uid, 0644, target_uid_show, target_uid_store);

static struct kobj_attribute tracked_files_attr =
    __ATTR(tracked_files, 0444, tracked_files_show, NULL);

static struct attribute *file_tracking_attrs[] = {
    &target_uid_attr.attr,
    &tracked_files_attr.attr,
    NULL,
};

static struct attribute_group file_tracking_attr_group = {
    .attrs = file_tracking_attrs,
};

// Init function
static int __init file_tracking_init(void)
{
    int ret;

    // Create /sys/kernel/file_tracking/
    file_tracking_kobj = kobject_create_and_add("file_tracking", kernel_kobj);
    if (!file_tracking_kobj)
        return -ENOMEM;

    ret = sysfs_create_group(file_tracking_kobj, &file_tracking_attr_group);
    if (ret)
        kobject_put(file_tracking_kobj);

    return ret;
}

// Cleanup function
static void __exit file_tracking_exit(void)
{
    clear_tracked_files();
    kobject_put(file_tracking_kobj);
}

subsys_initcall(file_tracking_init);
//module_init(file_tracking_init);
//module_exit(file_tracking_exit);

#endif /* CONFIG_TRACK_FILES */
