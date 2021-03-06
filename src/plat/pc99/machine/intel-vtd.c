/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#include <config.h>

#include <kernel/boot.h>
#include <machine.h>
#include <machine/io.h>
#include <arch/kernel/apic.h>
#include <arch/model/statedata.h>
#include <arch/linker.h>
#include <plat/machine/acpi.h>
#include <plat/machine/intel-vtd.h>
#include <util.h>

#define RTADDR_REG  0x20
#define GCMD_REG    0x18
#define GSTS_REG    0x1C
#define CCMD_REG    0x28
#define ECAP_REG    0x10
#define IOTLB_REG   0x08
#define FSTS_REG    0x34
#define FECTL_REG   0x38
#define FEDATA_REG  0x3C
#define FEADDR_REG  0x40
#define FEUADDR_REG 0x44
#define CAP_REG     0x08

/* Bit Positions within Registers */
#define SRTP        30  /* Set Root Table Pointer */
#define RTPS        30  /* Root Table Pointer Status */
#define TE          31  /* Translation Enable */
#define TES         31  /* Translation Enable Status */

/* ICC is 63rd bit in CCMD_REG, but since we will be
 * accessing this register as 4 byte word, ICC becomes
 * 31st bit in the upper 32bit word.
 */
#define ICC         31  /* Invalidate Context Cache */
#define CIRG        29  /* Context Invalidation Request Granularity */
#define CAIG        27  /* Context Actual Invalidation Granularity */
#define CAIG_MASK   0x3
#define IVO_MASK    0x3FF
#define IVT         31  /* Invalidate IOTLB */
#define IIRG        28  /* IOTLB Invalidation Request Granularity */
#define IAIG        25  /* IOTLB Actual Invalidation Granularity */
#define IAIG_MASK   0x7
#define IP          30  /* Interrupt Pending */
#define FRI         0x8 /* Fault Recording Index */
#define FRI_MASK    0xFF
#define FRO         24
#define FRO_MASK    0xFF
#define FI          12
#define SID_MASK    0xFFFF
#define FR_MASK     0xFF
#define FAULT_TYPE  30
#define FAULT       31
#define NFR         8   /* high word of CAP_REG */
#define NFR_MASK    0xff
#define PPF         1
#define PPF_MASK    1
#define PRESENT     1
#define WBF         27
#define WBFS        27
#define DID         8
#define RW          0x3

#define SAGAW         8
#define SAGAW_2_LEVEL 0x01
#define SAGAW_3_LEVEL 0x02
#define SAGAW_4_LEVEL 0x04
#define SAGAW_5_LEVEL 0x08
#define SAGAW_6_LEVEL 0x10

#define CONTEXT_GLOBAL_INVALIDATE 0x1
#define IOTLB_GLOBAL_INVALIDATE   0x1

#define DMA_TLB_READ_DRAIN  (1 << 17)
#define DMA_TLB_WRITE_DRAIN (1 << 16)

typedef uint32_t drhu_id_t;

static inline uint32_t vtd_read32(drhu_id_t drhu_id, uint32_t offset)
{
    return *(volatile uint32_t*)(PPTR_DRHU_START + (drhu_id << PAGE_BITS) + offset);
}

static inline void vtd_write32(drhu_id_t drhu_id, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(PPTR_DRHU_START + (drhu_id << PAGE_BITS) + offset) = value;
}

static inline uint32_t get_ivo(drhu_id_t drhu_id)
{
    return ((vtd_read32(drhu_id, ECAP_REG) >> 8) & IVO_MASK) * 16;
}

static inline int supports_passthrough(drhu_id_t drhu_id)
{
    return (vtd_read32(drhu_id, ECAP_REG) >> 6) & 1;
}

static uint32_t get_fro_offset(drhu_id_t drhu_id)
{
    uint32_t fro_offset;

    /* Get bits 31 to 24 from lower Capability Register */
    fro_offset = (vtd_read32(drhu_id, CAP_REG) >> FRO) & FRO_MASK;

    /* Get bits 33 to 32 from higher Capability Register */
    fro_offset |= (vtd_read32(drhu_id, CAP_REG + 4) & 0x3) << 8;

    return fro_offset << 4;
}

void invalidate_context_cache(void)
{
    /* FIXME - bugzilla bug 172
     * 1. Instead of assuming global invalidation, this function should
     *    accept a parameter to control the granularity of invalidation
     *    request.
     * 2. Instead of doing invalidation for all the IOMMUs, it should
     *    only do it for the IOMMU responsible for the requesting PCI
     *    device.
     */

    uint8_t   invalidate_command = CONTEXT_GLOBAL_INVALIDATE;
    uint32_t  ccmd_reg_upper;
    drhu_id_t i;

    for (i = 0; i < x86KSnumDrhu; i++) {
        /* Wait till ICC bit is clear */
        while ((vtd_read32(i, CCMD_REG + 4) >> ICC) & 1);

        /* Program CIRG for Global Invalidation by setting bit 61 which
         * will be bit 29 in upper 32 bits of CCMD_REG
         */
        ccmd_reg_upper = invalidate_command << CIRG;

        /* Invalidate Context Cache */
        ccmd_reg_upper |= (1U << ICC);
        vtd_write32(i, CCMD_REG, 0);
        vtd_write32(i, CCMD_REG + 4, ccmd_reg_upper);

        /* Wait for the invalidation to complete */
        while ((vtd_read32(i, CCMD_REG + 4) >> ICC) & 1);
    }
}

void invalidate_iotlb(void)
{
    /* FIXME - bugzilla bug 172
     * 1. Instead of assuming global invalidation, this function should
     *    accept a parameter to control the granularity of invalidation
     *    request.
     * 2. Instead of doing invalidation for all the IOMMUs, it should
     *    only do it for the IOMMU responsible for the requesting PCI
     *    device.
     */

    uint8_t   invalidate_command = IOTLB_GLOBAL_INVALIDATE;
    uint32_t  iotlb_reg_upper;
    uint32_t  ivo_offset;
    drhu_id_t i;

    for (i = 0; i < x86KSnumDrhu; i++) {
        ivo_offset = get_ivo(i);

        /* Wait till IVT bit is clear */
        while ((vtd_read32(i, ivo_offset + IOTLB_REG + 4) >> IVT) & 1);

        /* Program IIRG for Global Invalidation by setting bit 60 which
         * will be bit 28 in upper 32 bits of IOTLB_REG
         */
        iotlb_reg_upper = invalidate_command << IIRG;

        /* Invalidate IOTLB */
        iotlb_reg_upper |= (1U << IVT);
        iotlb_reg_upper |= DMA_TLB_READ_DRAIN | DMA_TLB_WRITE_DRAIN;

        vtd_write32(i, ivo_offset + IOTLB_REG, 0);
        vtd_write32(i, ivo_offset + IOTLB_REG + 4, iotlb_reg_upper);

        /* Wait for the invalidation to complete */
        while ((vtd_read32(i, ivo_offset + IOTLB_REG + 4) >> IVT) & 1);
    }
}

static void vtd_clear_fault(drhu_id_t i, word_t fr_reg)
{
    /* Clear the 'F' (Fault) bit to indicate that this fault is processed */
    vtd_write32(i, fr_reg + 12, BIT(FAULT));
}

static void vtd_process_faults(drhu_id_t i)
{
    /* Fault Recording register offset relative to the base register */
    uint32_t fro_offset;
    uint32_t source_id UNUSED;
    uint32_t fault_type UNUSED;
    uint32_t address[2] UNUSED;
    uint32_t reason UNUSED;
    uint32_t num_fault_regs;
    uint32_t fr_reg;
    uint32_t fault_status;
    uint32_t fault_record_index;

    /* Retrieves FRO by looking into Capability register bits 33 to 24 */
    fro_offset = get_fro_offset(i);
    fault_status = (vtd_read32(i, FSTS_REG) >> PPF) & PPF_MASK;

    if (fault_status) {
        num_fault_regs = ((vtd_read32(i, CAP_REG + 4) >> NFR) & NFR_MASK) + 1;
        fault_record_index = (vtd_read32(i, FSTS_REG) >> FRI) & FRI_MASK;
        fr_reg = fro_offset + 16 * fault_record_index;

        /* Traverse the fault register ring buffer */
        do {
            source_id = vtd_read32(i, fr_reg + 8) & SID_MASK;

            fault_type = (vtd_read32(i, fr_reg + 12) >> FAULT_TYPE) & 1;
            address[1] = vtd_read32(i, fr_reg + 4);
            address[0] = vtd_read32(i, fr_reg);
            reason = vtd_read32(i, fr_reg + 12) & FR_MASK;

            printf("IOMMU: DMA %s page fault ", fault_type ? "read" : "write");
            printf("from bus/dev/fun 0x%x ", source_id);
            printf("on address 0x%x:%x ", address[1], address[0]);
            printf("with reason code 0x%x\n", reason);

            vtd_clear_fault(i, fr_reg);

            fault_record_index = (fault_record_index + 1) % num_fault_regs;
            fr_reg = fro_offset + 16 * fault_record_index;
        } while ((vtd_read32(i, fr_reg + 12) >> FAULT) & 1);

        /* Check for Primary Fault Overflow */
        if (vtd_read32(i, FSTS_REG) & 1) {
            /* Clear PFO bit, so new faults will be generated again ! */
            vtd_write32(i, FSTS_REG, 1);
        }
    }
}

void vtd_handle_fault(void)
{
    drhu_id_t i;

    for (i = 0; i < x86KSnumDrhu; i++) {
        vtd_process_faults(i);
    }
}

BOOT_CODE static void
vtd_create_root_table(void)
{
    x86KSvtdRootTable = (void*)alloc_region(VTD_RT_SIZE_BITS);
    memzero((void*)x86KSvtdRootTable, 1 << VTD_RT_SIZE_BITS);
}

/* This function is a simplistic duplication of some of the logic
 * in iospace.c
 */
BOOT_CODE static void
vtd_map_reserved_page(vtd_cte_t *vtd_context_table, int context_index, paddr_t addr)
{
    int i;
    vtd_pte_t *iopt;
    vtd_pte_t *vtd_pte_slot;
    /* first check for the first page table */
    vtd_cte_t *vtd_context_slot = vtd_context_table + context_index;
    if (!vtd_cte_ptr_get_present(vtd_context_slot)) {
        iopt = (vtd_pte_t*)alloc_region(seL4_IOPageTableBits);
        if (!iopt) {
            fail("Failed to allocate IO page table");
        }
        memzero(iopt, 1 << seL4_IOPageTableBits);
        flushCacheRange(iopt, seL4_IOPageTableBits);

        vtd_cte_ptr_new(
            vtd_context_slot,
            x86KSFirstValidIODomain,  /* Domain ID                              */
            true,                     /* RMRR Mapping                           */
            x86KSnumIOPTLevels - 2,   /* Address Width                          */
            pptr_to_paddr(iopt),      /* Address Space Root                     */
            0,                        /* Translation Type                       */
            true);                    /* Present                                */
        x86KSFirstValidIODomain++;
        flushCacheRange(vtd_context_slot, VTD_CTE_SIZE_BITS);
    } else {
        iopt = (vtd_pte_t*)paddr_to_pptr(vtd_cte_ptr_get_asr(vtd_context_slot));
    }
    /* now recursively find and map page tables */
    for (i = x86KSnumIOPTLevels - 1; i >= 0; i--) {
        uint32_t iopt_index;
        /* If we are still looking up bits beyond the 32bit of physical
         * that we support then we select entry 0 in the current PT */
        if (VTD_PT_BITS * i >= 32) {
            iopt_index = 0;
        } else {
            iopt_index = ( (addr >> X86_4K_bits) >> (VTD_PT_BITS * i)) & MASK(VTD_PT_BITS);
        }
        vtd_pte_slot = iopt + iopt_index;
        if (i == 0) {
            /* Now put the mapping in */
            vtd_pte_ptr_new(vtd_pte_slot, addr, 1, 1);
            flushCacheRange(vtd_pte_slot, VTD_PTE_SIZE_BITS);
        } else {
            if (!vtd_pte_ptr_get_write(vtd_pte_slot)) {
                iopt = (vtd_pte_t*)alloc_region(seL4_IOPageTableBits);
                if (!iopt) {
                    fail("Failed to allocate IO page table");
                }
                memzero(iopt, BIT(seL4_IOPageTableBits));
                flushCacheRange(iopt, seL4_IOPageTableBits);

                vtd_pte_ptr_new(vtd_pte_slot, pptr_to_paddr(iopt), 1, 1);
                flushCacheRange(vtd_pte_slot, VTD_PTE_SIZE_BITS);
            } else {
                iopt = (vtd_pte_t*)paddr_to_pptr(vtd_pte_ptr_get_addr(vtd_pte_slot));
            }
        }
    }
}

BOOT_CODE static void
vtd_create_context_table(
    uint8_t   bus,
    uint32_t  max_num_iopt_levels,
    acpi_rmrr_list_t *rmrr_list
)
{
    word_t i;
    vtd_cte_t* vtd_context_table = (vtd_cte_t*)alloc_region(VTD_CT_SIZE_BITS);
    if (!vtd_context_table) {
        fail("Failed to allocate context table");
    }

    printf("IOMMU: Create VTD context table for PCI bus 0x%x (pptr=%p)\n", bus, vtd_context_table);
    memzero(vtd_context_table, 1 << VTD_CT_SIZE_BITS);
    flushCacheRange(vtd_context_table, VTD_CT_SIZE_BITS);

    x86KSvtdRootTable[bus] =
        vtd_rte_new(
            pptr_to_paddr(vtd_context_table), /* Context Table Pointer */
            true                              /* Present               */
        );

    /* map in any RMRR regions */
    for (i = 0; i < rmrr_list->num; i++) {
        if (vtd_get_root_index(rmrr_list->entries[i].device) == bus) {
            uint32_t addr;
            for (addr = rmrr_list->entries[i].base; addr < rmrr_list->entries[i].limit; addr += BIT(X86_4K_bits)) {
                (void)vtd_map_reserved_page;
                vtd_map_reserved_page(vtd_context_table, vtd_get_context_index(rmrr_list->entries[i].device), addr);
            }
        }
    }
}

BOOT_CODE static bool_t
vtd_enable(cpu_id_t cpu_id)
{
    drhu_id_t i;

    for (i = 0; i < x86KSnumDrhu; i++) {
        /* Set the Root Table Register */
        vtd_write32(i, RTADDR_REG, pptr_to_paddr((void*)x86KSvtdRootTable));
        vtd_write32(i, RTADDR_REG + 4, 0);

        /* Set SRTP bit in GCMD_REG */
        vtd_write32(i, GCMD_REG, (1 << SRTP));

        /* Wait for SRTP operation to complete by polling
         * RTPS bit from GSTS_REG
         */
        while (!((vtd_read32(i, GSTS_REG) >> RTPS) & 1));
    }

    /* Globally invalidate context cache of all IOMMUs */
    invalidate_context_cache();

    /* Globally invalidate IOTLB of all IOMMUs */
    invalidate_iotlb();

    for (i = 0; i < x86KSnumDrhu; i++) {
        uint32_t data, addr;

        data = int_iommu;
        addr = apic_get_base_paddr();
        if (!addr) {
            return false;
        }
        addr |= (cpu_id << 12);

        vtd_process_faults(i);
        vtd_write32(i, FECTL_REG, 0);
        vtd_write32(i, FEDATA_REG, data);
        vtd_write32(i, FEADDR_REG, addr);
        vtd_write32(i, FEUADDR_REG, 0);

        /*flush IOMMU write buffer */
        vtd_write32(i, GCMD_REG, BIT(WBF));
        while (((vtd_read32(i, GSTS_REG) >> WBFS) & 1));

        printf("IOMMU 0x%x: enabling...", i);

        /* Enable the DMA translation by setting TE bit in GCMD_REG */
        vtd_write32(i, GCMD_REG, (1U << TE));

        /* Wait for Translation Enable operation to complete by polling
         * TES bit from GSTS_REG
         */
        while (!((vtd_read32(i, GSTS_REG) >> TES) & 1));

        printf(" enabled\n");
    }
    return true;
}

BOOT_CODE bool_t
vtd_init(
    cpu_id_t  cpu_id,
    uint32_t  num_drhu,
    acpi_rmrr_list_t *rmrr_list
)
{
    drhu_id_t i;
    uint32_t  bus;
    uint32_t  aw_bitmask = 0xffffffff;
    uint32_t  max_num_iopt_levels;
    /* Start the number of domains at 16 bits */
    uint32_t  num_domain_id_bits = 16;

    x86KSnumDrhu = num_drhu;
    x86KSFirstValidIODomain = 0;

    if (x86KSnumDrhu == 0) {
        return true;
    }

    for (i = 0; i < x86KSnumDrhu; i++) {
        uint32_t bits_supported = 4 + 2 * (vtd_read32(i, CAP_REG) & 7);
        aw_bitmask &= vtd_read32(i, CAP_REG) >> SAGAW;
        printf("IOMMU 0x%x: %d-bit domain IDs supported\n", i, bits_supported);
        if (bits_supported < num_domain_id_bits) {
            num_domain_id_bits = bits_supported;
        }
    }

    x86KSnumIODomainIDBits = num_domain_id_bits;

    if (aw_bitmask & SAGAW_6_LEVEL) {
        max_num_iopt_levels = 6;
    } else if (aw_bitmask & SAGAW_5_LEVEL) {
        max_num_iopt_levels = 5;
    } else if (aw_bitmask & SAGAW_4_LEVEL) {
        max_num_iopt_levels = 4;
    } else if (aw_bitmask & SAGAW_3_LEVEL) {
        max_num_iopt_levels = 3;
    } else if (aw_bitmask & SAGAW_2_LEVEL) {
        max_num_iopt_levels = 2;
    } else {
        printf("IOMMU: mismatch of supported number of PT levels between IOMMUs\n");
        return false;
    }

    if (aw_bitmask & SAGAW_3_LEVEL) {
        x86KSnumIOPTLevels = 3;
    } else if (aw_bitmask & SAGAW_4_LEVEL) {
        x86KSnumIOPTLevels = 4;
    } else if (aw_bitmask & SAGAW_5_LEVEL) {
        x86KSnumIOPTLevels = 5;
    } else if (aw_bitmask & SAGAW_6_LEVEL) {
        x86KSnumIOPTLevels = 6;
    } else if (aw_bitmask & SAGAW_2_LEVEL) {
        x86KSnumIOPTLevels = 2;
    } else {
        printf("IOMMU: mismatch of supported number of PT levels between IOMMUs\n");
        return false;
    }

    printf("IOMMU: Using %d page-table levels (max. supported: %d)\n", x86KSnumIOPTLevels, max_num_iopt_levels);

    vtd_create_root_table();

    for (bus = 0; bus < 256; bus++) {
        vtd_create_context_table(
            bus,
            max_num_iopt_levels,
            rmrr_list
        );
    }

    flushCacheRange(x86KSvtdRootTable, VTD_RT_SIZE_BITS);

    if (!vtd_enable(cpu_id)) {
        return false;
    }
    return true;
}
