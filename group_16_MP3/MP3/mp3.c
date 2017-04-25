#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include "mp3_given.h"
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_16");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1

struct mp3_struct{
    struct list_head task_node;
    struct task_struct* linux_task;
    unsigned long process_util;
    unsigned long major_fault;
    unsigned long minor_fault;
    int pid;
    unsigned long curr_jiffies;
} typedef mp3_struct;

void* our_buffer;
DEFINE_MUTEX(mp3_lock);
static LIST_HEAD(task_list_head);
unsigned long offset;

static struct workqueue_struct *my_work;
static struct delayed_work *work;
struct cdev *my_cdev;
dev_t dev;

/*
*Registers the pid into our linked list, starts the queue_work if it's the first one added
*/
void registration(unsigned int pid){
    printk(KERN_ALERT "registering pid: %u\n",pid);
    mp3_struct* new_process = kmalloc(sizeof(mp3_struct),GFP_KERNEL);
    new_process->pid = pid;
    new_process->linux_task = find_task_by_pid(pid);
    new_process->process_util = 0;
    new_process->major_fault = 0;
    new_process->minor_fault = 0;
    // new_process->curr_jiffies = jiffies;
    mutex_lock(&mp3_lock);
    if(list_empty(&task_list_head)){
        list_add_tail(&(new_process->task_node),&task_list_head);

        //create workqueue job
        queue_delayed_work(my_work,work,msecs_to_jiffies(50));
        mutex_unlock(&mp3_lock);
        return;
    }
    else{
        struct list_head* ptr;
        mp3_struct* pos;
        
        
        list_for_each(ptr,&task_list_head){
            pos = list_entry(ptr, mp3_struct, task_node);
            if(new_process->pid < pos->pid ){
                list_add_tail(&(new_process->task_node), ptr);
                mutex_unlock(&mp3_lock);
                return;

            }
        }

        list_add_tail(&(new_process->task_node),&task_list_head);
  
        mutex_unlock(&mp3_lock);        
    
    }

}
/*
* Removes the pid from our linked list and if it's the last one,
*This means we can cancel a work that was delayed and flush and destroy the workqueue
*/
void unregistration(unsigned int pid){
    mp3_struct* pos;
    mp3_struct* i;
    mutex_lock(&mp3_lock);
    list_for_each_entry_safe(pos,i,&task_list_head,task_node){
        if(pos->pid == pid){
            list_del_init(&(pos->task_node));
            kfree(pos);
        }
    }

    if(list_empty(&task_list_head)){

        if(delayed_work_pending(work)){
            cancel_delayed_work(work);
        }
        flush_workqueue(my_work);
        destroy_workqueue(my_work);
       
    }
    mutex_unlock(&mp3_lock);
}


/*
* Extracts whether the user in userspace wants to register or unregister their process
*/
static ssize_t mp3_write (struct file *file, const char __user *buffer, size_t count, loff_t *data){
    printk(KERN_ALERT "called our write\n");

    int copied = sizeof(unsigned int);
    char* buf;
    buf = (char*)kmalloc(count, GFP_KERNEL);
    copy_from_user((void*)buf, buffer, count);
    char command;
    unsigned int pid;
    sscanf(buf, "%c", &command);
    switch(command){
        case 'R':
            printk(KERN_ALERT "called Registration \n");
            sscanf(buf,"%c %u", &command,&pid);
            registration(pid);
            printk(KERN_ALERT "done Registration \n");
            break;
        case 'U':
            printk(KERN_ALERT "called Unregistration \n");
            sscanf(buf,"%c %u", &command,&pid);
            unregistration(pid);
            printk(KERN_ALERT "done unRegistration \n");
            break;
    }

    kfree(buf);
    printk(KERN_ALERT "Finished Writing\n");
    return count;
}
/*
* When user calls mmap, this function gets called which maps our buffer we made into the virtual address
* the user wants. It loops until it finishes mapping all the pages from our buffer onto their virtual address
*/
static ssize_t mp3_mmap (struct file *filp, struct vm_area_struct *vma)
{
    // unsigned long pfn = vmalloc_to_pfn(our_buffer);
    // printk(KERN_ALERT "pfn: %lu, start: %lu, offset: %lu", pfn, vma->vm_start, vma->vm_pgoff);
    // if(remap_pfn_range(vma, vma->vm_start, pfn, 128 * PAGE_SIZE, PAGE_SHARED))
    //     return -EAGAIN;
    // return 0;



    unsigned long off = 0;
    while(off < 524288){
        unsigned long pfn = vmalloc_to_pfn(our_buffer + (sizeof(char)* off));


        if(remap_pfn_range(vma,vma->vm_start + (sizeof(char) * off), pfn, PAGE_SIZE, PAGE_SHARED)){
            return -EAGAIN;
        }
        off+=PAGE_SIZE;

    }
    return 0;

}

/*
* this gets invoked at every 50ms as per the instructions
* It loops through our current linked list and adds the total major,minor faults as well as
* all the process utilization time that occured between this and the last invocation
* It then writes the combined information of the processes into the buffer so that when
* the user calls mmap, it will be able to read this from the device
*/
void work_callback( struct work_struct *work){


    mutex_lock(&mp3_lock);
    if(list_empty(&task_list_head)){
        mutex_unlock(&mp3_lock);
        return;
    }
    struct list_head* ptr;
    mp3_struct* pos;    
    unsigned long total_min_flt = 0;
    unsigned long total_maj_flt = 0;
    unsigned long total_util =0;
    unsigned long total_utime = 0;
    unsigned long total_stime = 0;
    list_for_each(ptr,&task_list_head){
        pos = list_entry(ptr,mp3_struct,task_node);
        unsigned long min_flt;
        unsigned long maj_flt;
        unsigned long utime;
        unsigned long stime;

        if(get_cpu_use(pos->pid, &min_flt, &maj_flt, &utime, &stime) == 0){
            total_min_flt+= min_flt;
            total_maj_flt+=maj_flt;
            pos->minor_fault = min_flt;
            pos->major_fault = maj_flt;
            total_utime+= utime;
            total_stime+= stime;
            printk(KERN_ALERT "min_flt: %lu, maj_flt: %lu , utime: %lu, stime: %lu", min_flt,maj_flt,utime,stime);
            // pos->process_util = (utime + stime)/(jiffies-pos->curr_jiffies);
            // pos->curr_jiffies = jiffies;
            pos->process_util = (utime + stime)/(msecs_to_jiffies(50));
        }

    }
    mutex_unlock(&mp3_lock);
    // total_util = (total_utime + total_stime)  / (unsigned long)(msecs_to_jiffies(50));

    total_util = total_utime + total_stime;

    printk(KERN_ALERT "total_utime:%lu, total_stime:%lu, msecs_to_jiffies:%lu",total_utime,total_stime, msecs_to_jiffies(50));
    printk(KERN_ALERT "jiffies:%lu, total_min_flt:%lu, total_maj_flt:%lu, total_util:%lu",jiffies,total_min_flt,total_maj_flt,total_util);
    // offset+= sprintf(our_buffer + offset,"%lu%lu%lu%lu",jiffies,total_min_flt,total_maj_flt,total_util);
    memcpy(our_buffer + offset,&jiffies, sizeof(jiffies));
    offset+=sizeof(unsigned long);
    memcpy(our_buffer + offset, &total_min_flt, sizeof(total_min_flt));
    offset+=sizeof(unsigned long);
    memcpy(our_buffer + offset, &total_maj_flt, sizeof(total_maj_flt));
    offset+=sizeof(unsigned long);
    memcpy(our_buffer + offset, &total_util,sizeof(total_util));
    offset+=sizeof(unsigned long);


    // printk(KERN_ALERT "mybuffer:%s",our_buffer);

    queue_delayed_work(my_work,work,msecs_to_jiffies(50));
}

static const struct file_operations mp3_file = {
.owner = THIS_MODULE, 
// .read = mp3_read,
.write = mp3_write,
};

static const struct file_operations mp3_dev_file = {
.owner = THIS_MODULE, 
// .read = mp3_read,
.write = mp3_write,
.mmap = mp3_mmap,
};

int __init mp3_init(void)
{

    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE LOADING\n");
    #endif
    proc_dir =  proc_mkdir("mp3", NULL);
    proc_entry = proc_create("status", 0666, proc_dir, &mp3_file);
    offset = 0;
    our_buffer = vmalloc(524288);
    memset(our_buffer,0,524288);
    my_work = create_workqueue("myqueue");
    work = (struct delayed_work *)kmalloc(sizeof(struct delayed_work), GFP_KERNEL);
    INIT_DELAYED_WORK(work,work_callback);

    
    alloc_chrdev_region(&dev, 0, 1, "mp3_CDD" );
    my_cdev = cdev_alloc();
    cdev_init(my_cdev,&mp3_dev_file);
    cdev_add(my_cdev, dev, 1);
  
    printk(KERN_ALERT "MP3 MODULE LOADED\n");
    return 0;
}

void __exit mp3_exit(void){
    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
    #endif
    unregister_chrdev_region(dev, 1);
    cdev_del(my_cdev);
    remove_proc_entry("status", proc_dir);
    remove_proc_entry("mp3",NULL);
    vfree(our_buffer);
    kfree(work);
    // if(work != NULL){
    //     kfree(work);
    //     flush_workqueue(my_work);
    //     destroy_workqueue(my_work);
        
    // }
    printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}


module_init(mp3_init);
module_exit(mp3_exit);