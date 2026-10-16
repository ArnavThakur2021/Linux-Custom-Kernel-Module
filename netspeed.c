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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project Team");
MODULE_DESCRIPTION("NetMLFQ: Self-deciding bandwidth scheduler");
MODULE_VERSION("1.1");

#define MY_HASH_BITS 6
DECLARE_HASHTABLE(process_hash, MY_HASH_BITS);
static spinlock_t process_lock;

// Control parameters
static bool mlfq_on = false;            // Default off for safety
static unsigned long link_capacity_bytes = 5 * 1024 * 1024; // 5 MB/s default link limit
static unsigned long demote_threshold_bytes = 1024 * 1024;  // 1 MB per tick threshold
static bool congested = false;

// Priority Boost Timer
static struct timer_list boost_timer;
static unsigned int boost_ticks = 0;

struct process_node {
    pid_t tgid;
    char comm[TASK_COMM_LEN];
    int queue;          // 0: Q0, 1: Q1, 2: Q2
    unsigned long bytes_this_tick;
    unsigned long tokens;
    int heavy_ticks;
    int light_ticks;
    unsigned long drops;
    bool pinned;
    int pinned_queue;
    struct hlist_node node;
};

// Token limits per queue per tick (Q0: unlimited, Q1: 50%, Q2: 10%)
static unsigned long get_queue_allowance(int q) {
    unsigned long share = link_capacity_bytes / 5; // per-tick slice approx
    if (q == 0) return ULONG_MAX;
    if (q == 1) return share / 2;
    return share / 10;
}

// Hash table lookup or create entry
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

    // Allocate new entry if not found
    e = kzalloc(sizeof(*e), GFP_ATOMIC);
    if (e) {
        e->tgid = tgid;
        strscpy(e->comm, comm, TASK_COMM_LEN);
        e->queue = 0;
        e->tokens = get_queue_allowance(0);
        hash_add(process_hash, &e->node, tgid);
    }
    spin_unlock_irqrestore(&process_lock, flags);
    return e;
}

// Netfilter TX Hook (POST_ROUTING)
static unsigned int netspeed_tx_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct process_node *e;
    struct tcphdr *th;

    if (!skb || !mlfq_on || !in_task())
        return NF_ACCEPT;

    // Exclude loopback for safety
    if (state->out && (state->out->flags & IFF_LOOPBACK))
        return NF_ACCEPT;

    // Exclude SSH (port 22) so you don't lock yourself out
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

// 1-Second Periodic Timer (MLFQ Logic)
static void mlfq_timer_callback(struct timer_list *t) {
    struct process_node *e;
    unsigned long flags;
    unsigned long total_tx = 0;
    int bkt;

    spin_lock_irqsave(&process_lock, flags);

    // 1. Calculate total TX and check congestion
    hash_for_each(process_hash, bkt, e, node) {
        total_tx += e->bytes_this_tick;
    }
    congested = (total_tx >= (link_capacity_bytes * 7 / 10)); // 70% threshold

    // 2. MLFQ Rules (Demote, Promote, Token Refill)
    hash_for_each(process_hash, bkt, e, node) {
        if (e->pinned) {
            e->queue = e->pinned_queue;
        } else {
            // Demote check
            if (e->bytes_this_tick > demote_threshold_bytes) {
                e->heavy_ticks++;
                e->light_ticks = 0;
                if (e->heavy_ticks >= 3 && e->queue < 2) {
                    int old_q = e->queue;
                    e->queue++;
                    e->heavy_ticks = 0;
                    pr_info("netmlfq: pid %d (%s) Q%d->Q2 (heavy traffic)\n", e->tgid, e->comm, old_q);
                }
            } else {
                // Promote / Aging check
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

    // 3. Priority Boost every 30 seconds
    boost_ticks++;
    if (boost_ticks >= 30) {
        boost_ticks = 0;
        hash_for_each(process_hash, bkt, e, node) {
            if (!e->pinned) e->queue = 0;
        }
        pr_info("netmlfq: Priority Boost! All processes reset to Q0.\n");
    }

    spin_unlock_irqrestore(&process_lock, flags);
    mod_timer(&boost_timer, jiffies + msecs_to_jiffies(1000));
}

// Procfs /proc/netspeed read handler
static int netspeed_show(struct seq_file *m, void *v) {
    struct process_node *e;
    int bkt;

    seq_printf(m, "CONGESTION:%s\n", congested ? "ACTIVE" : "IDLE");
    seq_printf(m, "LINK_CAPACITY:%lu\n", link_capacity_bytes);
    seq_printf(m, "MLFQ_MODE:%s\n", mlfq_on ? "ON" : "OFF");
    
    spin_lock_bh(&process_lock);
    hash_for_each(process_hash, bkt, e, node) {
        seq_printf(m, "PROC %d %s Q%d %lu %lu\n", e->tgid, e->comm, e->queue, e->tokens, e->drops);
    }
    spin_unlock_bh(&process_lock);

    return 0;
}

static int netspeed_open(struct inode *inode, struct file *file) {
    return single_open(file, netspeed_show, NULL);
}

// Procfs /proc/netspeed_ctl write handler
static ssize_t netspeed_ctl_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    char buf[128];
    if (count >= sizeof(buf)) return -EINVAL;
    if (copy_from_user(buf, buffer, count)) return -EFAULT;
    buf[count] = '\0';

    if (strncmp(buf, "mode mlfq", 9) == 0) {
        mlfq_on = true;
        pr_info("netmlfq: Mode set to MLFQ ACTIVE\n");
    } else if (strncmp(buf, "mode off", 8) == 0) {
        mlfq_on = false;
        pr_info("netmlfq: Mode set to OFF (Bypass)\n");
    } else if (sscanf(buf, "link %lu", &link_capacity_bytes) == 1) {
        link_capacity_bytes *= 1024 * 1024; // MB to bytes
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

    pr_info("netmlfq: Module loaded successfully.\n");
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