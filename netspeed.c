#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 84)
#define del_timer_sync timer_delete_sync
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project Team");
MODULE_DESCRIPTION("NetMLFQ: Eco-Conscious & RuralLink Bandwidth Scheduler");
MODULE_VERSION("2.0");

#define MY_HASH_BITS 6
DECLARE_HASHTABLE(process_hash, MY_HASH_BITS);
static spinlock_t process_lock;

static bool mlfq_on = false;
static bool eco_mode = false;               /* NOBLE 1: Green Routing Toggle */
static unsigned long link_capacity_bytes = 5 * 1024 * 1024; // 5 MB/s
static unsigned long demote_threshold_bytes = 1024 * 1024;  // 1 MB per tick
static bool congested = false;

static struct timer_list boost_timer;
static unsigned int boost_ticks = 0;

struct process_node {
    pid_t tgid;
    char comm[TASK_COMM_LEN];
    int queue;          // 0, 1, 2
    unsigned long bytes_this_tick;
    unsigned long total_bytes;  // Cumulative data consumed
    unsigned long tokens;
    int heavy_ticks;
    int light_ticks;
    unsigned long drops;
    bool pinned;        /* NOBLE 2: RuralLink Equity Pinning */
    int pinned_queue;   /* NOBLE 2: Target pinned queue */
    struct hlist_node node;
};

static unsigned long get_queue_allowance(int q) {
    unsigned long share = link_capacity_bytes / 5;
    if (q == 0) return ULONG_MAX;
    if (q == 1) return share / 2;
    return share / 10;
}

static struct process_node *lookup_or_create(pid_t tgid, const char *comm) {
    struct process_node *e;
    unsigned long flags;

    spin_lock_irqsave(&process_lock, flags);
    hash_for_each_possible(process_hash, e, node, tgid) {
        if (e->tgid == tgid) {
            spin_unlock_irqrestore(&process_lock, flags);
            return e;
        }
    }

    e = kzalloc(sizeof(*e), GFP_ATOMIC);
    if (e) {
        e->tgid = tgid;
        strscpy(e->comm, comm, TASK_COMM_LEN);
        e->queue = 0;
        e->pinned = false;
        e->pinned_queue = 0;
        e->tokens = get_queue_allowance(0);
        hash_add(process_hash, &e->node, tgid);
    }
    spin_unlock_irqrestore(&process_lock, flags);
    return e;
}

static unsigned int netspeed_tx_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct process_node *e;
    struct tcphdr *th;

    if (!skb || !mlfq_on || !in_task())
        return NF_ACCEPT;

    if (state->out && (state->out->flags & IFF_LOOPBACK))
        return NF_ACCEPT;

    if (skb->protocol == htons(ETH_P_IP)) {
        struct iphdr *iph = ip_hdr(skb);
        if (iph && iph->protocol == IPPROTO_TCP) {
            th = tcp_hdr(skb);
            if (th && (ntohs(th->dest) == 22 || ntohs(th->source) == 22))
                return NF_ACCEPT;
        }
    }

    e = lookup_or_create(current->tgid, current->comm);
    if (!e)
        return NF_ACCEPT;

    spin_lock_bh(&process_lock);
    e->bytes_this_tick += skb->len;
    e->total_bytes += skb->len;

    if (congested && e->queue > 0 && !e->pinned) {
        if (e->tokens < skb->len) {
            e->drops++;
            spin_unlock_bh(&process_lock);
            return NF_DROP;
        }
        e->tokens -= skb->len;
    }
    spin_unlock_bh(&process_lock);

    return NF_ACCEPT;
}

static struct nf_hook_ops netspeed_nf_op = {
    .hook     = netspeed_tx_hook,
    .pf       = NFPROTO_IPV4,
    .hooknum  = NF_INET_POST_ROUTING,
    .priority = NF_IP_PRI_FIRST,
};

static void mlfq_timer_callback(struct timer_list *t) {
    struct process_node *e;
    unsigned long flags;
    unsigned long total_tx = 0;
    int bkt;

    spin_lock_irqsave(&process_lock, flags);

    hash_for_each(process_hash, bkt, e, node) {
        total_tx += e->bytes_this_tick;
    }

    /* NOBLE 1: Eco-Mode lowers congestion trigger threshold from 70% to 40% */
    if (eco_mode)
        congested = (total_tx >= (link_capacity_bytes * 4 / 10));
    else
        congested = (total_tx >= (link_capacity_bytes * 7 / 10));

    hash_for_each(process_hash, bkt, e, node) {
        /* NOBLE 2: RuralLink Permanent Queue Pinning */
        if (e->pinned) {
            e->queue = e->pinned_queue;
        } else {
            if (e->bytes_this_tick > demote_threshold_bytes) {
                e->heavy_ticks++;
                e->light_ticks = 0;
                /* NOBLE 1: Eco-mode demotes faster (after 1 heavy tick instead of 3) */
                int demote_trigger = eco_mode ? 1 : 3;
                if (e->heavy_ticks >= demote_trigger && e->queue < 2) {
                    int old_q = e->queue;
                    e->queue = eco_mode ? 2 : e->queue + 1; // Instant Q2 demotion in Eco Mode
                    e->heavy_ticks = 0;
                    pr_info("netmlfq: pid %d (%s) Q%d->Q%d (heavy traffic)\n", e->tgid, e->comm, old_q, e->queue);
                }
            } else {
                e->light_ticks++;
                e->heavy_ticks = 0;
                if (e->light_ticks >= 5 && e->queue > 0) {
                    int old_q = e->queue;
                    e->queue--;
                    e->light_ticks = 0;
                    pr_info("netmlfq: pid %d (%s) Q%d->Q%d (aging promote)\n", e->tgid, e->comm, old_q, e->queue);
                }
            }
        }
        e->tokens = get_queue_allowance(e->queue);
        e->bytes_this_tick = 0;
    }

    boost_ticks++;
    if (boost_ticks >= 30) {
        boost_ticks = 0;
        hash_for_each(process_hash, bkt, e, node) {
            if (!e->pinned) e->queue = 0;
        }
        pr_info("netmlfq: Priority Boost! Unpinned processes reset to Q0.\n");
    }

    spin_unlock_irqrestore(&process_lock, flags);
    mod_timer(&boost_timer, jiffies + msecs_to_jiffies(1000));
}

static int netspeed_show(struct seq_file *m, void *v) {
    struct process_node *e;
    int bkt;

    seq_printf(m, "CONGESTION:%s\n", congested ? "ACTIVE" : "IDLE");
    seq_printf(m, "LINK_CAPACITY:%lu\n", link_capacity_bytes);
    seq_printf(m, "MLFQ_MODE:%s\n", mlfq_on ? (eco_mode ? "ECO_GREEN" : "ON") : "OFF");
    
    spin_lock_bh(&process_lock);
    hash_for_each(process_hash, bkt, e, node) {
        seq_printf(m, "PROC %d %s Q%d %lu %lu %lu %s\n", 
                   e->tgid, e->comm, e->queue, e->total_bytes, e->tokens, e->drops,
                   e->pinned ? "PINNED" : "AUTO");
    }
    spin_unlock_bh(&process_lock);

    return 0;
}

static int netspeed_open(struct inode *inode, struct file *file) {
    return single_open(file, netspeed_show, NULL);
}

static ssize_t netspeed_ctl_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    char buf[128];
    pid_t target_pid;
    int target_q;

    if (count >= sizeof(buf)) return -EINVAL;
    if (copy_from_user(buf, buffer, count)) return -EFAULT;
    buf[count] = '\0';

    if (strncmp(buf, "mode mlfq", 9) == 0) {
        mlfq_on = true;
        eco_mode = false;
        pr_info("netmlfq: Mode set to MLFQ ACTIVE\n");
    } else if (strncmp(buf, "mode eco", 8) == 0) {  /* NOBLE 1: Eco Mode toggle */
        mlfq_on = true;
        eco_mode = true;
        pr_info("netmlfq: Mode set to ECO GREEN ROUTING\n");
    } else if (strncmp(buf, "mode off", 8) == 0) {
        mlfq_on = false;
        eco_mode = false;
        pr_info("netmlfq: Mode set to OFF (Bypass)\n");
    } else if (sscanf(buf, "pin %d %d", &target_pid, &target_q) == 2) { /* NOBLE 2: Pin process */
        struct process_node *e;
        unsigned long flags;
        spin_lock_irqsave(&process_lock, flags);
        hash_for_each_possible(process_hash, e, node, target_pid) {
            if (e->tgid == target_pid) {
                e->pinned = true;
                e->pinned_queue = (target_q >= 0 && target_q <= 2) ? target_q : 0;
                e->queue = e->pinned_queue;
                pr_info("netmlfq: Process %d pinned to Q%d\n", target_pid, e->pinned_queue);
                break;
            }
        }
        spin_unlock_irqrestore(&process_lock, flags);
    } else if (sscanf(buf, "unpin %d", &target_pid) == 1) { /* NOBLE 2: Unpin process */
        struct process_node *e;
        unsigned long flags;
        spin_lock_irqsave(&process_lock, flags);
        hash_for_each_possible(process_hash, e, node, target_pid) {
            if (e->tgid == target_pid) {
                e->pinned = false;
                pr_info("netmlfq: Process %d unpinned\n", target_pid);
                break;
            }
        }
        spin_unlock_irqrestore(&process_lock, flags);
    } else if (sscanf(buf, "link %lu", &link_capacity_bytes) == 1) {
        link_capacity_bytes *= 1024 * 1024;
        pr_info("netmlfq: Link capacity set to %lu bytes/s\n", link_capacity_bytes);
    } else if (sscanf(buf, "demote %lu", &demote_threshold_bytes) == 1) {
        demote_threshold_bytes *= 1024 * 1024;
        pr_info("netmlfq: Demote threshold set to %lu MB/tick\n", demote_threshold_bytes);
    }
    return count;
}

static const struct proc_ops netspeed_fops = {
    .proc_open    = netspeed_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static const struct proc_ops netspeed_ctl_fops = {
    .proc_write   = netspeed_ctl_write,
};

static int __init netspeed_init(void) {
    spin_lock_init(&process_lock);
    hash_init(process_hash);

    if (!proc_create("netspeed", 0444, NULL, &netspeed_fops))
        return -ENOMEM;

    if (!proc_create("netspeed_ctl", 0222, NULL, &netspeed_ctl_fops)) {
        remove_proc_entry("netspeed", NULL);
        return -ENOMEM;
    }

    if (nf_register_net_hooks(&init_net, &netspeed_nf_op, 1)) {
        remove_proc_entry("netspeed_ctl", NULL);
        remove_proc_entry("netspeed", NULL);
        return -EINVAL;
    }

    timer_setup(&boost_timer, mlfq_timer_callback, 0);
    mod_timer(&boost_timer, jiffies + msecs_to_jiffies(1000));

    pr_info("netmlfq: Eco & RuralLink Module loaded successfully.\n");
    return 0;
}

static void __exit netspeed_exit(void) {
    struct process_node *e;
    struct hlist_node *tmp;
    int bkt;

    del_timer_sync(&boost_timer);
    nf_unregister_net_hooks(&init_net, &netspeed_nf_op, 1);
    remove_proc_entry("netspeed_ctl", NULL);
    remove_proc_entry("netspeed", NULL);

    spin_lock_bh(&process_lock);
    hash_for_each_safe(process_hash, bkt, tmp, e, node) {
        hash_del(&e->node);
        kfree(e);
    }
    spin_unlock_bh(&process_lock);

    pr_info("netmlfq: Module unloaded.\n");
}

module_init(netspeed_init);
module_exit(netspeed_exit);