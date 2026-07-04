#if defined(_AARCH64_)

#include <linux_compat/time/linux_time_arch.h>
#include <modules/dtb/dev_tree.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>

#define PL031_COMPATIBLE "arm,pl031"
#define PL031_REG_RTCDR  0x00u

static void linux_time_aarch64_map_device_mmio(u64 phy_base, u64 len)
{
        ENTRY_FLAGS_t flags = PAGE_ENTRY_DEVICE | PAGE_ENTRY_GLOBAL
                              | PAGE_ENTRY_READ | PAGE_ENTRY_VALID
                              | PAGE_ENTRY_WRITE;

        for (paddr page = phy_base; page < phy_base + len; page += PAGE_SIZE) {
                (void)map(percpu(current_vspace),
                          PPN(page),
                          VPN(KERNEL_PHY_TO_VIRT(page)),
                          3,
                          flags,
                          &percpu(Map_Handler));
        }
}

static u64 linux_time_aarch64_read_pl031_seconds(u64 phy_base)
{
        volatile u32 *rtcdr = (volatile u32 *)(KERNEL_PHY_TO_VIRT(phy_base)
                                               + PL031_REG_RTCDR);

        return (u64)(*rtcdr);
}

u64 linux_time_arch_boot_realtime_us(void)
{
        struct device_node *rtc_node;
        struct property *reg_prop;
        u64 reg_pair[2];
        u64 phy_base;
        u64 map_len;
        u64 seconds;
        static u64 cached_boot_us;
        static bool cached;

        if (cached) {
                return cached_boot_us;
        }

        if (!device_root) {
                cached = true;
                return 0;
        }

        rtc_node = dev_node_find_by_compatible(NULL, PL031_COMPATIBLE);
        if (!rtc_node) {
                cached = true;
                return 0;
        }

        reg_prop = dev_node_find_property(rtc_node, "reg", 4);
        if (!reg_prop || reg_prop->len < (int)(2 * sizeof(u32))) {
                cached = true;
                return 0;
        }

        if (property_read_u64_arr(reg_prop, reg_pair, 2) != REND_SUCCESS) {
                cached = true;
                return 0;
        }

        phy_base = reg_pair[0];
        map_len = reg_pair[1] ? reg_pair[1] : PAGE_SIZE;
        if (map_len > PAGE_SIZE) {
                map_len = PAGE_SIZE;
        }

        linux_time_aarch64_map_device_mmio(phy_base, map_len);
        seconds = linux_time_aarch64_read_pl031_seconds(phy_base);
        cached_boot_us = seconds ? (seconds * 1000000ull) : 0;
        cached = true;
        return cached_boot_us;
}

#endif /* _AARCH64_ */
