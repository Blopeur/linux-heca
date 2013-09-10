#include <linux/pid_namespace.h>

#include "hutils.h"
#include "hspace.h"
#include "hproc.h"

#include "ioctl.h"
#include "task.h"
#include "base.h"
/*
 *  kernel/exit.c CODE removed ( line 814) note the code is broken and need to
 *  be fixed first .... ( break without releasing th hook ?? + other stuff... )
 *
#if defined(CONFIG_HECA) || defined(CONFIG_HECA_MODULE)
do {
const struct heca_hook_struct *heca =
heca_hook_read();
if (!heca || !heca->detach_task)
break;
heca->detach_task(tsk);
heca_hook_release(heca);
} while (0);
#endif

 *
 *
 *
 */


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


        rcu_read_lock();
        *tsk = find_task_by_pid_ns(pid, task_active_pid_ns(current));
        if (!*tsk){
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

/* relese doesn't work we need to fix the hspace release*/
int heca_detach_task(struct task_struct *tsk)
{
        int ret = 0, local_left;
        struct heca_space *hspace, *tmp_hspace;
        struct heca_process *hproc, *tmp_hproc;

        heca_printk(KERN_INFO "Heca Task detached, cleaning up ");
        list_for_each_entry_safe (hspace, tmp_hspace,
                        &get_heca_module_state()->hspaces_list, hspace_ptr) {
                local_left=0;
                list_for_each_entry_safe (hproc, tmp_hproc,
                                &hspace->hprocs_list, hproc_ptr) {
                        rcu_read_lock();
                        if (tsk == find_task_by_vpid(hproc->pid)) {
                                rcu_read_unlock();
                                teardown_hproc(hspace, hproc);
                        } else {
                                rcu_read_unlock();
                                local_left += is_hproc_local(hproc);
                        }
                }
                if(!local_left)
                        teardown_hspace(hspace);
        }
        return ret;
}

