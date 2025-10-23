/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <microkit.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/linux.h>
#include <libvmm/arch/aarch64/fault.h>

/*
 * As this is just an example, for simplicity we just make the size of the
 * guest's "RAM" the same for all platforms. For just booting Linux with a
 * simple user-space, 0x10000000 bytes (256MB) is plenty.
 */
#define GUEST_RAM_SIZE              0x10000000
#define GUEST_DTB_VADDR             0x4f000000
#define GUEST_INIT_RAM_DISK_VADDR   0x4d000000

/* For simplicity we just enforce the serial IRQ channel number to be the same
 * across platforms. */
#define SERIAL_IRQ_CH 1
#define SERIAL_IRQ 33

/* Data for the guest's kernel image. */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
/* Data for the device tree to be passed to the kernel. */
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];
/* Data for the initial RAM disk to be passed to the kernel. */
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];
/* Microkit will set this variable to the start of the guest RAM memory region. */
uintptr_t guest_ram_vaddr;

static void serial_ack(size_t vcpu_id, int irq, void *cookie)
{
    /*
     * For now we by default simply ack the serial IRQ, we have not
     * come across a case yet where more than this needs to be done.
     */
    microkit_irq_ack(SERIAL_IRQ_CH);
}

void init(void)
{
    /* Initialise the VMM, the VCPU(s), and start the guest */
    LOG_VMM("starting \"%s\"\n", microkit_name);
    /* Place all the binaries in the right locations before starting the guest */
    size_t kernel_size = _guest_kernel_image_end - _guest_kernel_image;
    size_t dtb_size = _guest_dtb_image_end - _guest_dtb_image;
    size_t initrd_size = _guest_initrd_image_end - _guest_initrd_image;

    LOG_VMM("[DEBUG] Initialise guest images...\n");
    uintptr_t kernel_pc = linux_setup_images(
        guest_ram_vaddr, 
        (uintptr_t)_guest_kernel_image, 
        kernel_size,
        (uintptr_t)_guest_dtb_image,
        GUEST_DTB_VADDR, 
        dtb_size,
        (uintptr_t)_guest_initrd_image, 
        GUEST_INIT_RAM_DISK_VADDR, 
        initrd_size);

    if (!kernel_pc) {
        LOG_VMM_ERR("Failed to initialise guest images\n");
        return;
    }

    LOG_VMM("[DEBUG] Initialising the virtual GIC driver...\n");
    bool success = virq_controller_init();
    if (!success) {
        LOG_VMM_ERR("Failed to initialise emulated interrupt controller\n");
        return;
    }

    success = virq_register(GUEST_BOOT_VCPU_ID, SERIAL_IRQ, &serial_ack, NULL);
    
    LOG_VMM("[DEBUG] Acking interrupt...(just in case there is already an interrupt available to handle)\n");
    microkit_irq_ack(SERIAL_IRQ_CH);
    
    LOG_VMM("[DEBUG] Start Linux guest...\n");
    guest_start(kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
}

void notified(microkit_channel ch)
{
    switch (ch) {
    case SERIAL_IRQ_CH: {
        LOG_VMM("[DEBUG] Notification received from SERIAL_IRQ_CH=%u\n", SERIAL_IRQ_CH);
        LOG_VMM("[DEBUG] Injecting virq to SERIAL_IRQ=%u....\n", SERIAL_IRQ);
        bool success = virq_inject(SERIAL_IRQ);
        if (!success) {
            LOG_VMM_ERR("IRQ %d dropped\n", SERIAL_IRQ);
        }
        break;
    }
    default:
        printf("Unexpected channel, ch: 0x%lx\n", ch);
    }
}

static uint64_t count = 0;
/*
 * The primary purpose of the VMM after initialisation is to act as a fault-handler.
 * Whenever our guest causes an exception, it gets delivered to this entry point for
 * the VMM to handle.
 */
seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo)
{   
    count++;
    if (count % 100000 == 0) {
        LOG_VMM("[DEBUG] Handled %lu faults\n", count);
    }
    // LOG_VMM("Fault receivd from child=%u\n, handling it...", child);
    bool success = fault_handle(child, msginfo);
    if (success) {
        // LOG_VMM("Fault handled successfully, replying to it for the guest to resume execution\n");
        *reply_msginfo = microkit_msginfo_new(0, 0);
        return seL4_True;
    }

    return seL4_False;
}
