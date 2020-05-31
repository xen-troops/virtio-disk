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
 * there must be no kvm, etc prefixes. Disk related stuff should
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

struct kvm {
	struct kvm_config	cfg;
	struct disk_image       **disks;
	int                     nr_disks;
};

void kvm__irq_trigger(struct kvm *kvm, int irq);

void *guest_flat_to_host(struct kvm *kvm, u64 offset, u32 size);

static inline void kvm__set_thread_name(const char *name)
{
	prctl(PR_SET_NAME, name);
}

#endif /* KVM__KVM_H */
