#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/disk-image.h"
#include "kvm/util-init.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>
#include <signal.h>
#include <sys/prctl.h>
#include <limits.h>

/*
 * XXX:
 * 1. This should be completely refactored including corresponding sources,
 * there must be no kvm, kvm_cpu, etc prefixes. Disk related stuff should
 * be moved to proper place.
 */

#ifndef PAGE_SIZE
#define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#endif

struct kvm_config {
	struct disk_image_params disk_image[MAX_DISK_IMAGES];
	u8  image_count;
	int debug_iodelay;
};

struct kvm_cpu {
	int dummy;
};

struct kvm {
	struct kvm_config	cfg;
	struct disk_image       **disks;
	int                     nr_disks;

#ifdef KVM_BRLOCK_DEBUG
	pthread_rwlock_t	brlock_sem;
#endif
};

void kvm__irq_trigger(struct kvm *kvm, int irq);
bool kvm__emulate_mmio(struct kvm_cpu *vcpu, u64 phys_addr, u8 *data, u32 len, u8 is_write);
int kvm__register_mmio(struct kvm *kvm, u64 phys_addr, u64 phys_addr_len, bool coalesce,
		       void (*mmio_fn)(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
			void *ptr);
bool kvm__deregister_mmio(struct kvm *kvm, u64 phys_addr);

void *guest_flat_to_host(struct kvm *kvm, u64 offset, u32 size);

static inline void kvm__set_thread_name(const char *name)
{
	prctl(PR_SET_NAME, name);
}

#endif /* KVM__KVM_H */
