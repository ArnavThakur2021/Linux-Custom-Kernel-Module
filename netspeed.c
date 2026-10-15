#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Developer");
MODULE_DESCRIPTION("Live Network Monitor with Multi-IP Tracking");
MODULE_VERSION("0.5");

#define IP_HISTORY_SIZE 3

struct net_stat {
    unsigned long rx_bytes;
    unsigned long tx_bytes;
    __be32 rx_ips[IP_HISTORY_SIZE];
    __be32 tx_ips[IP_HISTORY_SIZE];
    int rx_idx;
    int tx_idx;
};

static DEFINE_PER_CPU(struct net_stat, netspeed_stats);

// Helper to add unique or recent IPs to the circular history buffer
static void add_rx_ip(__be32 ip) {
    int i;
    struct net_stat *stat = this_cpu_ptr(&netspeed_stats);
    if (!ip) return;
    
    // Check if already in recent list
    for (i = 0; i < IP_HISTORY_SIZE; i++) {
        if (stat->rx_ips[i] == ip) return;
    }
    
    stat->rx_ips[stat->rx_idx] = ip;
    stat->rx_idx = (stat->rx_idx + 1) % IP_HISTORY_SIZE;
}

static void add_tx_ip(__be32 ip) {
    int i;
    struct net_stat *stat = this_cpu_ptr(&netspeed_stats);
    if (!ip) return;

    for (i = 0; i < IP_HISTORY_SIZE; i++) {
        if (stat->tx_ips[i] == ip) return;
    }

    stat->tx_ips[stat->tx_idx] = ip;
    stat->tx_idx = (stat->tx_idx + 1) % IP_HISTORY_SIZE;
}

static unsigned int netspeed_rx_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct iphdr *iph;
    if (!skb || !state->in)
        return NF_ACCEPT;

    if (state->in->flags & IFF_LOOPBACK)
        return NF_ACCEPT;

    iph = ip_hdr(skb);
    if (iph) {
        struct net_stat *stat = this_cpu_ptr(&netspeed_stats);
        stat->rx_bytes += skb->len;
        add_rx_ip(iph->saddr);
    }
    return NF_ACCEPT;
}

static unsigned int netspeed_tx_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct iphdr *iph;
    if (!skb || !state->out)
        return NF_ACCEPT;

    if (state->out->flags & IFF_LOOPBACK)
        return NF_ACCEPT;

    iph = ip_hdr(skb);
    if (iph) {
        struct net_stat *stat = this_cpu_ptr(&netspeed_stats);
        stat->tx_bytes += skb->len;
        add_tx_ip(iph->daddr);
    }
    return NF_ACCEPT;
}

static struct nf_hook_ops netspeed_filters[] = {
    {
        .hook     = netspeed_rx_hook,
        .pf       = NFPROTO_IPV4,
        .hooknum  = NF_INET_PRE_ROUTING,
        .priority = NF_IP_PRI_FIRST,
    },
    {
        .hook     = netspeed_tx_hook,
        .pf       = NFPROTO_IPV4,
        .hooknum  = NF_INET_POST_ROUTING,
        .priority = NF_IP_PRI_FIRST,
    },
};

static int netspeed_netdevice_event(struct notifier_block *nb, unsigned long event, void *ptr) {
    struct net_device *dev = netdev_notifier_info_to_dev(ptr);
    if (dev->flags & IFF_LOOPBACK)
        return NOTIFY_DONE;

    if (event == NETDEV_UP)
        pr_info("netspeed: Interface %s UP\n", dev->name);
    else if (event == NETDEV_DOWN)
        pr_info("netspeed: Interface %s DOWN\n", dev->name);

    return NOTIFY_DONE;
}

static struct notifier_block netspeed_nb = {
    .notifier_call = netspeed_netdevice_event,
};

static int netspeed_show(struct seq_file *m, void *v) {
    unsigned long total_rx = 0;
    unsigned long total_tx = 0;
    int cpu, i;
    
    __be32 combined_rx[IP_HISTORY_SIZE] = {0};
    __be32 combined_tx[IP_HISTORY_SIZE] = {0};
    int rx_count = 0, tx_count = 0;

    for_each_online_cpu(cpu) {
        struct net_stat *stat = per_cpu_ptr(&netspeed_stats, cpu);
        total_rx += stat->rx_bytes;
        total_tx += stat->tx_bytes;

        for (i = 0; i < IP_HISTORY_SIZE; i++) {
            if (stat->rx_ips[i] && rx_count < IP_HISTORY_SIZE)
                combined_rx[rx_count++] = stat->rx_ips[i];
            if (stat->tx_ips[i] && tx_count < IP_HISTORY_SIZE)
                combined_tx[tx_count++] = stat->tx_ips[i];
        }
    }

    unsigned long long rx_mb = total_rx / 1000000ULL;
    unsigned long long tx_mb = total_tx / 1000000ULL;

    seq_printf(m, "TOTAL_RX:%lu\n", total_rx);
    seq_printf(m, "TOTAL_TX:%lu\n", total_tx);
    seq_printf(m, "RX_MB:%llu\n", rx_mb);
    seq_printf(m, "TX_MB:%llu\n", tx_mb);

    seq_printf(m, "RX_IPS:");
    for (i = 0; i < rx_count; i++) {
        seq_printf(m, "%pI4%s", &combined_rx[i], (i == rx_count - 1) ? "" : ",");
    }
    seq_printf(m, "\n");

    seq_printf(m, "TX_IPS:");
    for (i = 0; i < tx_count; i++) {
        seq_printf(m, "%pI4%s", &combined_tx[i], (i == tx_count - 1) ? "" : ",");
    }
    seq_printf(m, "\n");

    return 0;
}

static int netspeed_open(struct inode *inode, struct file *file) {
    return single_open(file, netspeed_show, NULL);
}

static const struct proc_ops netspeed_fops = {
    .proc_open    = netspeed_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int __init netspeed_init(void) {
    int ret;

    ret = register_netdevice_notifier(&netspeed_nb);
    if (ret)
        return ret;

    ret = nf_register_net_hooks(&init_net, netspeed_filters, ARRAY_SIZE(netspeed_filters));
    if (ret) {
        unregister_netdevice_notifier(&netspeed_nb);
        return ret;
    }

    if (!proc_create("netspeed", 0444, NULL, &netspeed_fops)) {
        nf_unregister_net_hooks(&init_net, netspeed_filters, ARRAY_SIZE(netspeed_filters));
        unregister_netdevice_notifier(&netspeed_nb);
        return -ENOMEM;
    }

    pr_info("netspeed: Loaded with Multi-IP tracking\n");
    return 0;
}

static void __exit netspeed_exit(void) {
    remove_proc_entry("netspeed", NULL);
    nf_unregister_net_hooks(&init_net, netspeed_filters, ARRAY_SIZE(netspeed_filters));
    unregister_netdevice_notifier(&netspeed_nb);
    pr_info("netspeed: Unloaded successfully\n");
}

module_init(netspeed_init);
module_exit(netspeed_exit);