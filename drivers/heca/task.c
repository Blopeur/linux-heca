#include <linux/pid_namespace.h>
#include "ioctl.h"
#include "task.h"
#include "base.h"

pid_t get_current_pid(void)
{
        pid_t pid;
        rcu_read_lock();
        pid = task_pid_nr_ns(current, task_active_pid_ns(current));
        rcu_read_unlock();
        return pid;
}

static int get_task_struct_by_pid(pid_t pid, struct task_struct **tsk)
{
        int ret = 0;
        const struct cred *cred = current_cred(), *tcred;

        heca_printk(KERN_DEBUG "<enter>");

        rcu_read_lock();
        *tsk = find_task_by_pid_ns(pid, task_active_pid_ns(current));
        if (!*tsk) {
                heca_printk(KERN_ERR "can't find pid %d", pid);
                ret = -ESRCH;
                goto done;
        }

        tcred = __task_cred(*tsk);
        if (!uid_eq(cred->euid, GLOBAL_ROOT_UID) &&
                        !uid_eq(cred->euid, tcred->uid) &&
                        !uid_eq(cred->euid, tcred->suid)) {
                ret = -EACCES;
                goto done;
        }

        get_task_struct(*tsk);
done:
        rcu_read_unlock();
        heca_printk(KERN_DEBUG "<exit> %d", ret);
        return ret;
}

struct mm_struct *find_mm_by_pid(pid_t pid)
{
        struct task_struct *tsk;
        struct mm_struct *mm;

        if (get_task_struct_by_pid(pid, &tsk))
                return NULL;
        mm = tsk->mm;
        BUG_ON(!mm);
        put_task_struct(tsk);
        return mm;
}

int heca_attach_task(struct task_struct *tsk)
{
        return 0;
}

int heca_detach_task(struct task_struct *tsk)
{
        int ret = 0;
        struct heca_space *hspace;
        struct heca_process *hproc;
        struct list_head *pos, *n, *it;

        list_for_each (pos, &get_heca_module_state()->hspaces_list) {
                hspace = list_entry(pos, struct heca_space, hspace_ptr);
                list_for_each_safe (it, n, &hspace->hprocs_list) {
                        hproc = list_entry(it, struct heca_process, hproc_ptr);
                        if (tsk == find_task_by_vpid(hproc->pid)) {
                                heca_printk(KERN_DEBUG "removing SVM associated with pid %d",
                                                hproc->pid);
                                remove_hproc(hspace->hspace_id,
                                                hproc->hproc_id);
                        }
                }
        }
        return ret;
}

