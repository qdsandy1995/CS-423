#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include "mp2_given.h"
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_16");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
#define READY 1
#define RUNNING 2
#define SLEEPING 3
#define UPPER_BOUND 639

struct mp2_struct{
    struct list_head task_node;
    struct task_struct* linux_task;
    struct timer_list wakeup_timer;
    unsigned int task_state;
    uint64_t next_period;
    unsigned int pid;
    unsigned long relative_period; //milliseconds
    unsigned long slice;
} typedef mp2_struct;

mp2_struct* curr_running;
static struct task_struct *thread1;
spinlock_t mp2_spin;
static LIST_HEAD(task_list_head);
static struct kmem_cache* our_cache;
DEFINE_MUTEX(mp2_lock);

//Called when the timer wakes up
//Sets the task this timer is related to to Ready state
// and wakes up scheduling thread
void timer_callback(unsigned long stuff){
    mp2_struct* got_info = (mp2_struct*)stuff;
    unsigned long flags;
    spin_lock_irqsave(&mp2_spin,flags);
    got_info->task_state = READY;
    spin_unlock_irqrestore(&mp2_spin,flags);
    wake_up_process(thread1);
}

//Returns 0 if you shouldn't admit it
int admission_control(int slice, int period)
{
    mp2_struct* pos;
    int ub = (slice/period) * 1000;
    printk(KERN_ALERT "slice,period: %u,%u\n; ub: %u",slice,period,ub);
    if(!list_empty(&task_list_head)){
           list_for_each_entry(pos,&task_list_head,task_node)
    {
        ub += (pos->slice/pos->relative_period)*1000;
    }
    }
 
    if(ub > UPPER_BOUND)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

//Handles the registration for a new process, checks if it can be admitted
//If it can, it will insert into list in order of pid
void registration(unsigned int pid, unsigned long period, unsigned long computation){
     printk(KERN_ALERT "registering pid: %u\n",pid);
     mp2_struct* new_process;
     new_process = kmem_cache_alloc(our_cache, GFP_KERNEL);
     new_process->pid = pid;
     new_process->relative_period = period; //milliseconds
     new_process->slice = computation;
     new_process->task_state = SLEEPING;
     new_process->linux_task = find_task_by_pid(pid);
     new_process->next_period = jiffies + msecs_to_jiffies(period);
     struct list_head* ptr;
     mp2_struct* pos;

    if(0 == admission_control(computation,period)){
        printk(KERN_ALERT "%s\n","failed admission");
        kmem_cache_free(our_cache, new_process);
        return;
    }
     //the instructions said we should index our list by pid
     //Assuming in increasing order
     list_for_each(ptr,&task_list_head){
        pos = list_entry(ptr, mp2_struct, task_node);
        if(new_process->pid < pos->pid ){
            list_add_tail(&(new_process->task_node), ptr); //don't know if this adds before current pos or not
            setup_timer(&(new_process->wakeup_timer),timer_callback,(unsigned long)new_process);
            return;
        }


     }
    list_add_tail(&(new_process->task_node),&task_list_head);
    setup_timer(&(new_process->wakeup_timer),timer_callback,(unsigned long)new_process);

}

//When userapp calls yield, it will sleep and wake up the scheduler
//As well as assigns the next time it should wake up
void yield_process(unsigned int pid){
    mp2_struct* pos;
    mp2_struct* i;
    mutex_lock(&mp2_lock);
    list_for_each_entry_safe(pos,i,&task_list_head, task_node){
        if(pos->pid == pid){
            break;
        }
    }
    mutex_unlock(&mp2_lock);
    if(pos != NULL){
        pos->task_state = SLEEPING;
        if(pos->next_period > jiffies){ //if time hasn't happened yet
            printk(KERN_ALERT "calling the timer to wake up at: %lu, current time is: %lu\n, for pid:%u ",pos->next_period,jiffies, pid);

            mod_timer(&(pos->wakeup_timer), pos->next_period);
            pos->next_period = pos->next_period + msecs_to_jiffies(pos->relative_period);
            printk(KERN_ALERT "The next time it will wake up is: %lu\n",pos->next_period);
            set_task_state(pos->linux_task, TASK_UNINTERRUPTIBLE);
        }
    }
    wake_up_process(thread1);
    schedule();
}

//After process finishes jobs, it can be removed from list
//Deallocates all memory and deletes timers
void deregistration(unsigned int pid){
    mp2_struct* pos;
    mp2_struct* i;
    mutex_lock(&mp2_lock);
    list_for_each_entry_safe(pos,i,&task_list_head, task_node){
        if(pos->pid == pid){
            if(curr_running == pos){
                curr_running = NULL;
            }
            del_timer(&(pos->wakeup_timer));
            list_del_init(&(pos->task_node));
            kmem_cache_free(our_cache, pos);
        }
    }
    mutex_unlock(&mp2_lock);
    printk(KERN_ALERT "deregistered pid: %u\n",pid);
}

//Used when the userapp wants to check if it's in the list of tasks
static ssize_t mp2_read (struct file *file, char __user *buffer, size_t count, loff_t *data){
    printk(KERN_ALERT "called our read\n");
    int copied;
    char* buf;
    buf = (char *) kmalloc(count,GFP_KERNEL);
    copied = 0;
    mutex_lock(&mp2_lock);
    //return process PID, Period and Processing Time to user app
    mp2_struct* pos;
    list_for_each_entry(pos,&task_list_head, task_node){
        copied+= sprintf(buf+copied, "Pid:%u, Period:%lu, Processing Time:%lu\n", pos->pid, pos->relative_period, pos->slice);
    }
    mutex_unlock(&mp2_lock);
    copy_to_user(buffer,buf,copied);
    kfree(buf);
    return copied;
}

//Our write extracts what command the userapp has called, and either
//Registers, yields, or deregisters
static ssize_t mp2_write (struct file *file, const char __user *buffer, size_t count, loff_t *data){

    printk(KERN_ALERT "called our write\n");

    int copied = sizeof(unsigned int);
    char* buf;
    buf = (char*)kmalloc(count, GFP_KERNEL);
    copy_from_user((void*)buf, buffer, count);
    char command;
    unsigned int pid;
    unsigned long period;
    unsigned long computation;
    sscanf(buf, "%c", &command);
    switch(command){
        case 'R':
            printk(KERN_ALERT "called Registration \n");
            sscanf(buf,"%c, %u, %lu, %lu", &command,&pid,&period, &computation);
            registration(pid, period, computation);
            printk(KERN_ALERT "done Registration \n");
            break;
        case 'Y':
            printk(KERN_ALERT "called Yield\n");
            sscanf(buf,"%c, %u", &command,&pid);
            yield_process(pid);
        
            break;
        case 'D':
            printk(KERN_ALERT "called Deregistration\n");
            sscanf(buf,"%c, %u", &command,&pid);
            deregistration(pid);
            break;
    }
    kfree(buf);
    printk(KERN_ALERT "Finished Writing\n");
    return count;
}

//Our scheduling thread will call this function
//Loops through our linked list to see if there's a state read and if
//and choses the highest priority
//Also preempts current running task if task you found has a shorter period
void do_work(void){
    mutex_lock(&mp2_lock);
    //return process PID, Period and Processing Time to user app
    mp2_struct* pos;
    mp2_struct* highest_priority =NULL;

    list_for_each_entry(pos,&task_list_head, task_node){
        if(pos->task_state == READY){
            if(highest_priority == NULL){
                highest_priority = pos;
            }
            else{
                if(pos->relative_period < highest_priority->relative_period){
                    highest_priority = pos;
                }
            }
        }
    }
    mutex_unlock(&mp2_lock);
    if(highest_priority == NULL){ //if didn't find one state that's ready, preempt current running - curr_running
        if(curr_running != NULL){
            if (curr_running->task_state == RUNNING){
                curr_running->task_state = READY;
            }
    
            struct sched_param sparam1;
            sparam1.sched_priority=0;
            sched_setscheduler(curr_running->linux_task, SCHED_NORMAL, &sparam1);
            curr_running = NULL;
        }
    }
    else{
        if(curr_running!= NULL ){

            if (curr_running->task_state == RUNNING && highest_priority->relative_period < curr_running->relative_period){
                highest_priority->task_state = RUNNING;            
                struct sched_param sparam;
                wake_up_process(highest_priority->linux_task);
                sparam.sched_priority=99;
                sched_setscheduler(highest_priority->linux_task, SCHED_FIFO, &sparam);

                struct sched_param sparam1;
                sparam1.sched_priority=0;
                sched_setscheduler(curr_running->linux_task, SCHED_NORMAL, &sparam1);
                curr_running = highest_priority;

            }

        }
        else{ //if nothing is currently running, just start up highest_priority
            highest_priority->task_state = RUNNING;            
            struct sched_param sparam;
            wake_up_process(highest_priority->linux_task);
            sparam.sched_priority=99;
            sched_setscheduler(highest_priority->linux_task, SCHED_FIFO, &sparam);
            curr_running = highest_priority;
        }
    }
}

//Thread function handler, calls do_work function to do actual scheduling
int thread_work(void){
    printk(KERN_ALERT "called thread function\n");

    while(1){
        if(kthread_should_stop()) {
            do_exit(0);
        }
        do_work();
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }
    return 0;
}
static const struct file_operations mp2_file = {
.owner = THIS_MODULE, 
.read = mp2_read,
.write = mp2_write,
};

int __init mp2_init(void)
{
    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE LOADING\n");
    #endif
    proc_dir =  proc_mkdir("mp2", NULL);
    proc_entry = proc_create("status", 0666, proc_dir, &mp2_file);
    our_cache = KMEM_CACHE(mp2_struct, 0);
    spin_lock_init(&mp2_spin);
    char dispatcher[12] = "dispatcher1";
    thread1 = kthread_create((void*)thread_work, NULL, dispatcher);
    printk(KERN_ALERT "MP2 MODULE LOADED\n");
    return 0;
}
void __exit mp2_exit(void)
{
    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
    #endif
    remove_proc_entry("status", proc_dir);
    remove_proc_entry("mp2",NULL);
    mp2_struct* pos;
    mp2_struct* i;
    int ret;
    ret = kthread_stop(thread1);
    if(!ret)
        printk(KERN_ALERT "Thread stopped");

    if(!list_empty(&task_list_head)){
        list_for_each_entry_safe(pos,i,&task_list_head,task_node){
        list_del(&(pos->task_node));
        kmem_cache_free(our_cache, pos);
    }
  }
    kmem_cache_destroy(our_cache);
    printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}


module_init(mp2_init);
module_exit(mp2_exit);
