/*
 * Microsoft Hyper-V emulation. See Microsoft's
 * Hypervisor Top Level Functional Specification for more information.
 *
 * Copyright (C) 2019-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/guest/vm.h>
#include <logmsg.h>
#include <asm/vmx.h>
#include <asm/guest/hyperv.h>
#include <asm/tsc.h>

#define DBG_LEVEL_HYPERV		6U

/* Partition Reference Counter (HV_X64_MSR_TIME_REF_COUNT) */
#define CPUID3A_TIME_REF_COUNT_MSR	(1U << 1U)
/* Hypercall MSRs (HV_X64_MSR_GUEST_OS_ID and HV_X64_MSR_HYPERCALL) */
#define CPUID3A_HYPERCALL_MSR		(1U << 5U)
/* Access virtual processor index MSR (HV_X64_MSR_VP_INDEX) */
#define CPUID3A_VP_INDEX_MSR		(1U << 6U)
/* Partition reference TSC MSR (HV_X64_MSR_REFERENCE_TSC) */
#define CPUID3A_REFERENCE_TSC_MSR	(1U << 9U)
/* Partition local APIC and TSC frequency registers (HV_X64_MSR_TSC_FREQUENCY/HV_X64_MSR_APIC_FREQUENCY) */
#define CPUID3A_ACCESS_FREQUENCY_MSRS	(1U << 11U)
/* Frequency MSRs available */
#define CPUID3D_FREQ_MSRS_AVAILABLE	(1U << 8U)

struct HV_REFERENCE_TSC_PAGE {
	uint32_t tsc_sequence;
	uint32_t reserved1;
	uint64_t tsc_scale;
	uint64_t tsc_offset;
	uint64_t reserved2[509];
};

static inline uint64_t
u64_shl64_div_u64(uint64_t a, uint64_t divisor)
{
	uint64_t ret, tmp;

	asm volatile ("divq %2" :
		"=a" (ret), "=d" (tmp) :
		"rm" (divisor), "0" (0U), "1" (a));

	return ret;
}

static inline uint64_t
u64_mul_u64_shr64(uint64_t a, uint64_t b)
{
	uint64_t ret, disc;

	asm volatile ("mulq %3" :
		"=d" (ret), "=a" (disc) :
		"a" (a), "r" (b));

	return ret;
}

static void
hyperv_setup_tsc_page(const struct acrn_vcpu *vcpu, uint64_t val)
{
	union hyperv_ref_tsc_page_msr *ref_tsc_page = &vcpu->vm->arch_vm.hyperv.ref_tsc_page;
	struct HV_REFERENCE_TSC_PAGE *p;
	uint32_t tsc_seq;

	ref_tsc_page->val64 = val;

	if (ref_tsc_page->enabled == 1U) {
		p = (struct HV_REFERENCE_TSC_PAGE *)gpa2hva(vcpu->vm, ref_tsc_page->gpfn << PAGE_SHIFT);
		if (p != NULL) {
			stac();
			p->tsc_scale = vcpu->vm->arch_vm.hyperv.tsc_scale;
			p->tsc_offset = vcpu->vm->arch_vm.hyperv.tsc_offset;
			cpu_write_memory_barrier();
			tsc_seq = p->tsc_sequence + 1U;
			if ((tsc_seq == 0xFFFFFFFFU) || (tsc_seq == 0U)) {
				tsc_seq = 1U;
			}
			p->tsc_sequence = tsc_seq;
			clac();
		}
	}
}

static inline uint64_t
hyperv_scale_tsc(uint64_t scale)
{
	uint64_t tsc;

	tsc = rdtsc() + exec_vmread64(VMX_TSC_OFFSET_FULL);

	return u64_mul_u64_shr64(tsc, scale);
}

static inline uint64_t
hyperv_get_ReferenceTime(struct acrn_vm *vm)
{
	return hyperv_scale_tsc(vm->arch_vm.hyperv.tsc_scale) - vm->arch_vm.hyperv.tsc_offset;
}

static void
hyperv_setup_hypercall_page(const struct acrn_vcpu *vcpu, uint64_t val)
{
	union hyperv_hypercall_msr hypercall;
	uint64_t page_gpa;
	void *page_hva;

	/*
	 * All enlightened versions of Windows operating systems invoke guest hypercalls on
	 * the basis of the recommendations presented by the hypervisor in CPUID.40000004:EAX.
	 * A conforming hypervisor must return HV_STATUS_INVALID_HYPERCALL_CODE for any
	 * unimplemented hypercalls.
	 * ACRN does not wish to handle any hypercalls at moment, the following hypercall 
	 * code page is implemented for this purpose.
	 * inst32[] for 32 bits:
	 * 	mov eax, 0x02 ; HV_STATUS_INVALID_HYPERCALL_CODE
	 * 	mov edx, 0
	 * 	ret
	 * inst64[] for 64 bits:
	 * 	mov rax, 0x02 ; HV_STATUS_INVALID_HYPERCALL_CODE
	 * 	ret
	 */
	const uint8_t inst32[11] = {0xb8U, 0x02U, 0x0U, 0x0U, 0x0U, 0xbaU, 0x0U, 0x0U, 0x0U, 0x0U, 0xc3U};
	const uint8_t inst64[8] = {0x48U, 0xc7U, 0xc0U, 0x02U, 0x0U, 0x0U, 0x0U, 0xc3U};

	hypercall.val64 = val;

	if (hypercall.enabled != 0UL) {
		page_gpa = hypercall.gpfn << PAGE_SHIFT;
		page_hva = gpa2hva(vcpu->vm, page_gpa);
		if (page_hva != NULL) {
			stac();
			(void)memset(page_hva, 0U, PAGE_SIZE);
			if (get_vcpu_mode(vcpu) == CPU_MODE_64BIT) {
				(void)memcpy_s(page_hva, 8U, inst64, 8U);
			} else {
				(void)memcpy_s(page_hva, 11U, inst32, 11U);
			}
			clac();
		}
	}
}

int32_t
hyperv_wrmsr(struct acrn_vcpu *vcpu, uint32_t msr, uint64_t wval)
{
	int32_t ret = 0;

	switch (msr) {
	case HV_X64_MSR_GUEST_OS_ID:
		vcpu->vm->arch_vm.hyperv.guest_os_id.val64 = wval;
		if (wval == 0UL) {
			vcpu->vm->arch_vm.hyperv.hypercall_page.enabled = 0UL;
		}
		break;
	case HV_X64_MSR_HYPERCALL:
		if (vcpu->vm->arch_vm.hyperv.guest_os_id.val64 == 0UL) {
			pr_warn("hv: %s: guest_os_id is 0", __func__);
			break;
		}
		vcpu->vm->arch_vm.hyperv.hypercall_page.val64 = wval;
		hyperv_setup_hypercall_page(vcpu, wval);
		break;
	case HV_X64_MSR_REFERENCE_TSC:
		hyperv_setup_tsc_page(vcpu, wval);
		break;
	case HV_X64_MSR_VP_INDEX:
	case HV_X64_MSR_TIME_REF_COUNT:
	case HV_X64_MSR_TSC_FREQUENCY:
	case HV_X64_MSR_APIC_FREQUENCY:
		/* read only */
		/* fallthrough */
	default:
		pr_err("hv: %s: unexpected MSR[0x%x] write", __func__, msr);
		ret = -1;
		break;
	}

	dev_dbg(DBG_LEVEL_HYPERV, "hv: %s: MSR=0x%x wval=0x%lx vcpuid=%d vmid=%d",
		__func__, msr, wval, vcpu->vcpu_id, vcpu->vm->vm_id);

	return ret;
}

int32_t
hyperv_rdmsr(struct acrn_vcpu *vcpu, uint32_t msr, uint64_t *rval)
{
	int32_t ret = 0;

	switch (msr) {
	case HV_X64_MSR_GUEST_OS_ID:
		*rval = vcpu->vm->arch_vm.hyperv.guest_os_id.val64;
		break;
	case HV_X64_MSR_HYPERCALL:
		*rval = vcpu->vm->arch_vm.hyperv.hypercall_page.val64;
		break;
	case HV_X64_MSR_VP_INDEX:
		*rval = vcpu->vcpu_id;
		break;
	case HV_X64_MSR_TIME_REF_COUNT:
		*rval = hyperv_get_ReferenceTime(vcpu->vm);
		break;
	case HV_X64_MSR_REFERENCE_TSC:
		*rval = vcpu->vm->arch_vm.hyperv.ref_tsc_page.val64;
		break;
	case HV_X64_MSR_TSC_FREQUENCY:
		*rval = get_tsc_khz() * 1000UL;
		break;
	case HV_X64_MSR_APIC_FREQUENCY:
		/* vLAPIC freq is the same as TSC freq */
		*rval = get_tsc_khz() * 1000UL;
		break;
	default:
		pr_err("hv: %s: unexpected MSR[0x%x] read", __func__, msr);
		ret = -1;
		break;
	}

	dev_dbg(DBG_LEVEL_HYPERV, "hv: %s: MSR=0x%x rval=0x%lx vcpuid=%d vmid=%d",
		__func__, msr, *rval, vcpu->vcpu_id, vcpu->vm->vm_id);

	return ret;
}

void
hyperv_init_time(struct acrn_vm *vm)
{
	uint64_t tsc_scale, tsc_khz = get_tsc_khz();
	uint64_t tsc_offset;

	/*
	 * The partition reference time is computed by the following formula:
	 * ReferenceTime = ((VirtualTsc * TscScale) >> 64) + TscOffset
	 * ReferenceTime is in 100ns units
	 *
	 * ReferenceTime =
	 *     VirtualTsc / (get_tsc_khz() * 1000) * 1000000000 / 100
	 *     + TscOffset
	 *
	 * TscScale = (10000U << 64U) / get_tsc_khz()
	 */
	tsc_scale = u64_shl64_div_u64(10000U, tsc_khz);
	tsc_offset = hyperv_scale_tsc(tsc_scale);

	vm->arch_vm.hyperv.tsc_scale = tsc_scale;
	vm->arch_vm.hyperv.tsc_offset = tsc_offset;

	dev_dbg(DBG_LEVEL_HYPERV, "%s, tsc_scale = 0x%lx, tsc_offset = %ld",
		__func__, tsc_scale, tsc_offset);
}

void
hyperv_init_vcpuid_entry(uint32_t leaf, uint32_t subleaf, uint32_t flags,
			 struct vcpuid_entry *entry)
{
	entry->leaf = leaf;
	entry->subleaf = subleaf;
	entry->flags = flags;

	switch (leaf) {
	case 0x40000001U: /* HV interface version */
		entry->eax = 0x31237648U; /* "Hv#1" */
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = 0U;
		break;
	case 0x40000002U: /* HV system identity */
		entry->eax = 0U;
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = 0U;
		break;
	case 0x40000003U: /* HV supported feature */
		entry->eax = CPUID3A_HYPERCALL_MSR | CPUID3A_VP_INDEX_MSR |
			CPUID3A_TIME_REF_COUNT_MSR | CPUID3A_REFERENCE_TSC_MSR |
			CPUID3A_ACCESS_FREQUENCY_MSRS;
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = CPUID3D_FREQ_MSRS_AVAILABLE;
		break;
	case 0x40000004U: /* HV Recommended hypercall usage */
		entry->eax = 0U;
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = 0U;
		break;
	case 0x40000005U: /* HV Maximum Supported Virtual & logical Processors */
		entry->eax = MAX_VCPUS_PER_VM;
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = 0U;
		break;
	case 0x40000006U: /* Implementation Hardware Features */
		entry->eax = 0U;
		entry->ebx = 0U;
		entry->ecx = 0U;
		entry->edx = 0U;
		break;
	default:
		/* do nothing */
		break;
	}

	dev_dbg(DBG_LEVEL_HYPERV, "hv: %s: leaf=%x subleaf=%x flags=%x eax=%x ebx=%x ecx=%x edx=%x",
		__func__, leaf, subleaf, flags, entry->eax, entry->ebx, entry->ecx, entry->edx);
}

void
hyperv_page_destory(struct acrn_vm *vm)
{
	/* Reset the hypercall page */
	vm->arch_vm.hyperv.hypercall_page.enabled = 0U;
	/* Reset OS id */
	vm->arch_vm.hyperv.guest_os_id.val64 = 0UL;
	/* Reset the TSC page */
	vm->arch_vm.hyperv.ref_tsc_page.enabled = 0UL;
}
