#ifndef KVM__KVM_BARRIER_H
#define KVM__KVM_BARRIER_H

/* XXX: This is Arm specific and should be avoided */

#define mb()	asm volatile ("dmb ish"		: : : "memory")
#define rmb()	asm volatile ("dmb ishld"	: : : "memory")
#define wmb()	asm volatile ("dmb ishst"	: : : "memory")

#endif /* KVM__KVM_BARRIER_H */
