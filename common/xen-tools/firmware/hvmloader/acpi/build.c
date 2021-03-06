/*
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2006, Keir Fraser, XenSource Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License, version 
 * 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more 
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */
/*
 * uXen changes:
 *
 * Copyright 2012-2018, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "acpi2_0.h"
#include "ssdt_s3.h"
#include "ssdt_s4.h"
// #include "ssdt_tpm.h"
#include "ssdt_pm.h"
#include "../config.h"
#include "../util.h"
#include "../modules.h"

#define ACPI_MAX_SECONDARY_TABLES 16
#define align16(sz)        (((sz) + 15) & ~15)
#define fixed_strcpy(d, s) strncpy((d), (s), sizeof(d))

extern struct acpi_20_rsdp Rsdp;
extern struct acpi_20_rsdt Rsdt;
extern struct acpi_20_xsdt Xsdt;
extern struct acpi_20_fadt Fadt;
extern struct acpi_20_facs Facs;
extern struct acpi_20_waet Waet;

/*
 * Located at ACPI_INFO_PHYSICAL_ADDRESS.
 *
 * This must match the Field("BIOS"....) definition in the DSDT.
 */
struct acpi_info {
    uint8_t  com1_present:1;    /* 0[0] - System has COM1? */
    uint8_t  com2_present:1;    /* 0[1] - System has COM2? */
    uint8_t  lpt1_present:1;    /* 0[2] - System has LPT1? */
    uint8_t  hpet_present:1;    /* 0[3] - System has HPET? */
    uint8_t  smc_present:1;     /* 0[4] - System has SMC? */
    uint32_t pci_min, pci_len;  /* 4, 8 - PCI I/O hole boundaries */
    uint32_t madt_csum_addr;    /* 12   - Address of MADT checksum */
    uint32_t madt_lapic0_addr;  /* 16   - Address of first MADT LAPIC struct */
};

/* Number of processor objects in the chosen DSDT. */
static unsigned int nr_processor_objects;

/* Pointer to SLIC table if there is one */
static struct acpi_header *slic_header = NULL;

static void set_checksum(
    void *table, uint32_t checksum_offset, uint32_t length)
{
    uint8_t *p, sum = 0;

    p = table;
    p[checksum_offset] = 0;

    while ( length-- )
        sum = sum + *p++;

    p = table;
    p[checksum_offset] = -sum;
}

static uint8_t battery_port_exists(void)
{
    return (inb(0x88) == 0x1F);
}

static void set_oem(struct acpi_header *header)
{
    memcpy(header->oem_id, hvm_info->oem_info.oem_id, 6);
    memcpy(header->oem_table_id, hvm_info->oem_info.oem_table_id, 8);
    header->oem_revision = hvm_info->oem_info.oem_revision;
    memcpy(&header->creator_id, hvm_info->oem_info.creator_id, 4);
    header->creator_revision = hvm_info->oem_info.creator_revision;
}

static void set_rsdt_oem(struct acpi_header *header)
{
    if (slic_header) {
        memcpy(header->oem_id, slic_header->oem_id, 6);
        memcpy(header->oem_table_id, slic_header->oem_table_id, 8);
        header->oem_revision = slic_header->oem_revision;
        header->creator_id = slic_header->creator_id;
        header->creator_revision = slic_header->creator_revision;
    } else
        set_oem(header);
}

static struct acpi_20_madt *construct_madt(struct acpi_info *info)
{
    struct acpi_20_madt           *madt;
    struct acpi_20_madt_intsrcovr *intsrcovr;
    struct acpi_20_madt_ioapic    *io_apic;
    struct acpi_20_madt_lapic     *lapic;
    int i, sz;

    sz  = sizeof(struct acpi_20_madt);
    sz += sizeof(struct acpi_20_madt_intsrcovr) * 16;
    sz += sizeof(struct acpi_20_madt_ioapic);
    sz += sizeof(struct acpi_20_madt_lapic) * nr_processor_objects;

    madt = mem_alloc(sz, 16);
    if (!madt) return NULL;

    memset(madt, 0, sizeof(*madt));
    madt->header.signature    = ACPI_2_0_MADT_SIGNATURE;
    madt->header.revision     = ACPI_2_0_MADT_REVISION;
    set_oem(&madt->header);
    madt->lapic_addr = LAPIC_BASE_ADDRESS;
    madt->flags      = ACPI_PCAT_COMPAT;

    intsrcovr = (struct acpi_20_madt_intsrcovr *)(madt + 1);
    for ( i = 0; i < 16; i++ )
    {
        memset(intsrcovr, 0, sizeof(*intsrcovr));
        intsrcovr->type   = ACPI_INTERRUPT_SOURCE_OVERRIDE;
        intsrcovr->length = sizeof(*intsrcovr);
        intsrcovr->source = i;

        if ( i == 0 )
        {
            /* ISA IRQ0 routed to IOAPIC GSI 2. */
            intsrcovr->gsi    = 2;
            intsrcovr->flags  = 0x0;
        }
        else if ( PCI_ISA_IRQ_MASK & (1U << i) )
        {
            /* PCI: active-low level-triggered. */
            intsrcovr->gsi    = i;
            intsrcovr->flags  = 0xf;
        }
        else if ( i == 7 )  /* We take IRQ7 -> GSI7 for V4V so we can make it edge triggered */
        {
            intsrcovr->gsi    = i;
            intsrcovr->flags    = 0x1; /* Edge Triggered*/
        }
        else
        {
            /* No need for a INT source override structure. */
            continue;
        }

        intsrcovr++;
    }

    io_apic = (struct acpi_20_madt_ioapic *)intsrcovr;
    memset(io_apic, 0, sizeof(*io_apic));
    io_apic->type        = ACPI_IO_APIC;
    io_apic->length      = sizeof(*io_apic);
    io_apic->ioapic_id   = IOAPIC_ID;
    io_apic->ioapic_addr = IOAPIC_BASE_ADDRESS;

    lapic = (struct acpi_20_madt_lapic *)(io_apic + 1);
    info->madt_lapic0_addr = (uint32_t)lapic;
    for ( i = 0; i < nr_processor_objects; i++ )
    {
        memset(lapic, 0, sizeof(*lapic));
        lapic->type    = ACPI_PROCESSOR_LOCAL_APIC;
        lapic->length  = sizeof(*lapic);
        /* Processor ID must match processor-object IDs in the DSDT. */
        lapic->acpi_processor_id = i;
        lapic->apic_id = LAPIC_ID(i);
        lapic->flags = ((i < hvm_info->nr_vcpus) &&
                        test_bit(i, hvm_info->vcpu_online)
                        ? ACPI_LOCAL_APIC_ENABLED : 0);
        lapic++;
    }

    madt->header.length = (unsigned char *)lapic - (unsigned char *)madt;
    set_checksum(madt, offsetof(struct acpi_header, checksum),
                 madt->header.length);
    info->madt_csum_addr = (uint32_t)&madt->header.checksum;

    return madt;
}

static struct acpi_20_hpet *construct_hpet(void)
{
    struct acpi_20_hpet *hpet;

    hpet = mem_alloc(sizeof(*hpet), 16);
    if (!hpet) return NULL;

    memset(hpet, 0, sizeof(*hpet));
    hpet->header.signature    = ACPI_2_0_HPET_SIGNATURE;
    hpet->header.revision     = ACPI_2_0_HPET_REVISION;
    set_oem(&hpet->header);
    hpet->timer_block_id      = 0x8086a201;
    hpet->addr.address        = ACPI_HPET_ADDRESS;

    hpet->header.length = sizeof(*hpet);
    set_checksum(hpet, offsetof(struct acpi_header, checksum), sizeof(*hpet));
    return hpet;
}

static struct acpi_20_waet *construct_waet(void)
{
    struct acpi_20_waet *waet;

    waet = mem_alloc(sizeof(*waet), 16);
    if (!waet) return NULL;

    memcpy(waet, &Waet, sizeof(*waet));
    set_oem(&waet->header);

    waet->header.length = sizeof(*waet);
    set_checksum(waet, offsetof(struct acpi_header, checksum), sizeof(*waet));

    return waet;
}

static int construct_passthrough_tables(unsigned long *table_ptrs,
                                        int nr_tables)
{
    struct hvm_modules_iterator iter;
    struct acpi_header *header;
    int nr_added;
    int nr_max = (ACPI_MAX_SECONDARY_TABLES - nr_tables - 1);
    uint8_t *buffer;
    void *table;
    uint32_t length;

    init_acpi_module_iterator(&iter, NULL, 1);

    for ( nr_added = 0; nr_added < nr_max; nr_added++ )
    {
        table = hvm_find_module_entry(&iter, &length);
        if ( (table == NULL)||(length < sizeof(struct acpi_header)) )
            break;

        buffer = mem_alloc(length, 16);
        if (!buffer)
            break;
        memcpy(buffer, table, length);

        /* Update the header values to match the rest of the table set. */
        header = (struct acpi_header*)buffer;

        printf("Found ACPI passthrough table with signature \'%c%c%c%c\'\n",
               ((char *)&header->signature)[0],
               ((char *)&header->signature)[1],
               ((char *)&header->signature)[2],
               ((char *)&header->signature)[3]);

        /*
         * Overwrite the OEM IDs of the table, unless if it is a SLIC table,
         * in which case, we save the header and reuse the OEM IDs in the
         * RSDT and XSDT tables.
         */
        if (header->signature == ASCII32('S', 'L', 'I', 'C'))
            slic_header = header;
        else if (header->signature != ASCII32('M', 'S', 'D', 'M'))
            set_oem(header);

        set_checksum(buffer, offsetof(struct acpi_header, checksum), length);

        table_ptrs[nr_tables++] = (unsigned long)buffer;
    }

    return nr_added;
}

static int construct_secondary_tables(unsigned long *table_ptrs,
                                      struct acpi_info *info)
{
    int nr_tables = 0;
    struct acpi_20_madt *madt;
    struct acpi_20_hpet *hpet;
    struct acpi_20_waet *waet;
#if 0                           /* TPM */
    struct acpi_20_tcpa *tcpa;
#endif
    unsigned char *ssdt;
#if 0                           /* TPM */
    static const uint16_t tis_signature[] = {0x0001, 0x0001, 0x0001};
    uint16_t *tis_hdr;
    void *lasa;
#endif

    /* MADT. */
    if ( (hvm_info->nr_vcpus > 1) || hvm_info->apic_mode )
    {
        madt = construct_madt(info);
        if (!madt) return -1;
        table_ptrs[nr_tables++] = (unsigned long)madt;
    }

    /* HPET. Always included in DSDT, so always include it here too. */
    /* (And it's unconditionally required by Windows SVVP tests.) */
    hpet = construct_hpet();
    if (!hpet) return -1;
    table_ptrs[nr_tables++] = (unsigned long)hpet;

    /* WAET. */
    waet = construct_waet();
    if (!waet) return -1;
    table_ptrs[nr_tables++] = (unsigned long)waet;

    if ( battery_port_exists() )
    {
        ssdt = mem_alloc(sizeof(ssdt_pm), 16);
        if (!ssdt) return -1;
        memcpy(ssdt, ssdt_pm, sizeof(ssdt_pm));
        table_ptrs[nr_tables++] = (unsigned long)ssdt;
    }

    if ( !strncmp(xenstore_read("platform/acpi_s3", "0"), "1", 1) )
    {
        ssdt = mem_alloc(sizeof(ssdt_s3), 16);
        if (!ssdt) return -1;
        memcpy(ssdt, ssdt_s3, sizeof(ssdt_s3));
        table_ptrs[nr_tables++] = (unsigned long)ssdt;
    } else {
        printf("S3 disabled\n");
    }

    if ( !strncmp(xenstore_read("platform/acpi_s4", "0"), "1", 1) )
    {
        ssdt = mem_alloc(sizeof(ssdt_s4), 16);
        if (!ssdt) return -1;
        memcpy(ssdt, ssdt_s4, sizeof(ssdt_s4));
        table_ptrs[nr_tables++] = (unsigned long)ssdt;
    } else {
        printf("S4 disabled\n");
    }

#if 0                           /* TPM */
    /* TPM TCPA and SSDT. */
    tis_hdr = (uint16_t *)0xFED40F00;
    if ( (tis_hdr[0] == tis_signature[0]) &&
         (tis_hdr[1] == tis_signature[1]) &&
         (tis_hdr[2] == tis_signature[2]) )
    {
        ssdt = mem_alloc(sizeof(ssdt_tpm), 16);
        if (!ssdt) return -1;
        memcpy(ssdt, ssdt_tpm, sizeof(ssdt_tpm));
        table_ptrs[nr_tables++] = (unsigned long)ssdt;

        tcpa = mem_alloc(sizeof(struct acpi_20_tcpa), 16);
        if (!tcpa) return -1;
        memset(tcpa, 0, sizeof(*tcpa));
        table_ptrs[nr_tables++] = (unsigned long)tcpa;

        tcpa->header.signature = ACPI_2_0_TCPA_SIGNATURE;
        tcpa->header.length    = sizeof(*tcpa);
        tcpa->header.revision  = ACPI_2_0_TCPA_REVISION;
        set_oem(&tcpa->header);
        if ( (lasa = mem_alloc(ACPI_2_0_TCPA_LAML_SIZE, 16)) != NULL )
        {
            tcpa->lasa = virt_to_phys(lasa);
            tcpa->laml = ACPI_2_0_TCPA_LAML_SIZE;
            memset(lasa, 0, tcpa->laml);
            set_checksum(tcpa,
                         offsetof(struct acpi_header, checksum),
                         tcpa->header.length);
        }
    }
#endif

    /* Load any tables passed through in HVM modules. */
    nr_tables += construct_passthrough_tables(table_ptrs, nr_tables);

    table_ptrs[nr_tables] = 0;
    return nr_tables;
}

void acpi_build_tables(struct acpi_config *config, unsigned int physical)
{
    struct acpi_info *acpi_info;
    struct acpi_20_rsdp *rsdp;
    struct acpi_20_rsdt *rsdt;
    struct acpi_20_xsdt *xsdt;
    struct acpi_20_fadt *fadt;
    struct acpi_10_fadt *fadt_10;
    struct acpi_20_facs *facs;
    unsigned char       *dsdt;
    unsigned long        secondary_tables[ACPI_MAX_SECONDARY_TABLES];
    int                  nr_secondaries, i;

    /* Allocate and initialise the acpi info area. */
    /* hyperv: assume preallocated by dm */
    mem_hole_populate_ram(ACPI_INFO_PHYSICAL_ADDRESS >> PAGE_SHIFT, 1);
    acpi_info = (struct acpi_info *)ACPI_INFO_PHYSICAL_ADDRESS;
    memset(acpi_info, 0, sizeof(*acpi_info));

    /*
     * Fill in high-memory data structures, starting at @buf.
     */

    facs = mem_alloc(sizeof(struct acpi_20_facs), 16);
    if (!facs) goto oom;
    memcpy(facs, &Facs, sizeof(struct acpi_20_facs));

    /*
     * Alternative DSDTs we get linked against. A cover-all DSDT for up to the
     * implementation-defined maximum number of VCPUs, and an alternative for use
     * when a guest can only have up to 15 VCPUs.
     *
     * The latter is required for Windows 2000, which experiences a BSOD of
     * KMODE_EXCEPTION_NOT_HANDLED if it sees more than 15 processor objects.
     */
    if ( hvm_info->nr_vcpus <= 15 && config->dsdt_15cpu)
    {
        dsdt = mem_alloc(config->dsdt_15cpu_len, 16);
        if (!dsdt) goto oom;
        memcpy(dsdt, config->dsdt_15cpu, config->dsdt_15cpu_len);
        nr_processor_objects = 15;
    }
    else
    {
        dsdt = mem_alloc(config->dsdt_anycpu_len, 16);
        if (!dsdt) goto oom;
        memcpy(dsdt, config->dsdt_anycpu, config->dsdt_anycpu_len);
        nr_processor_objects = HVM_MAX_VCPUS;
    }

    /*
     * N.B. ACPI 1.0 operating systems may not handle FADT with revision 2
     * or above properly, notably Windows 2000, which tries to copy FADT
     * into a 116 bytes buffer thus causing an overflow. The solution is to
     * link the higher revision FADT with the XSDT only and introduce a
     * compatible revision 1 FADT that is linked with the RSDT. Refer to:
     *     http://www.acpi.info/presentations/S01USMOBS169_OS%20new.ppt
     */
    fadt_10 = mem_alloc(sizeof(struct acpi_10_fadt), 16);
    if (!fadt_10) goto oom;
    memcpy(fadt_10, &Fadt, sizeof(struct acpi_10_fadt));
    set_oem(&fadt_10->header);
    fadt_10->header.length = sizeof(struct acpi_10_fadt);
    fadt_10->header.revision = ACPI_1_0_FADT_REVISION;
    fadt_10->dsdt          = (unsigned long)dsdt;
    fadt_10->firmware_ctrl = (unsigned long)facs;
    set_checksum(fadt_10,
                 offsetof(struct acpi_header, checksum),
                 sizeof(struct acpi_10_fadt));

    fadt = mem_alloc(sizeof(struct acpi_20_fadt), 16);
    if (!fadt) goto oom;
    memcpy(fadt, &Fadt, sizeof(struct acpi_20_fadt));
    set_oem(&fadt->header);
    fadt->dsdt   = (unsigned long)dsdt;
    fadt->x_dsdt = (unsigned long)dsdt;
    fadt->firmware_ctrl   = (unsigned long)facs;
    fadt->x_firmware_ctrl = (unsigned long)facs;
    set_checksum(fadt,
                 offsetof(struct acpi_header, checksum),
                 sizeof(struct acpi_20_fadt));

    nr_secondaries = construct_secondary_tables(secondary_tables, acpi_info);
    if ( nr_secondaries < 0 )
        goto oom;

    xsdt = mem_alloc(sizeof(struct acpi_20_xsdt)+
                     sizeof(uint64_t)*nr_secondaries,
                     16);
    if (!xsdt) goto oom;
    memcpy(xsdt, &Xsdt, sizeof(struct acpi_header));
    set_rsdt_oem(&xsdt->header);
    xsdt->entry[0] = (unsigned long)fadt;
    for ( i = 0; secondary_tables[i]; i++ )
        xsdt->entry[i+1] = secondary_tables[i];
    xsdt->header.length = sizeof(struct acpi_header) + (i+1)*sizeof(uint64_t);
    set_checksum(xsdt,
                 offsetof(struct acpi_header, checksum),
                 xsdt->header.length);

    rsdt = mem_alloc(sizeof(struct acpi_20_rsdt)+
                     sizeof(uint32_t)*nr_secondaries,
                     16);
    if (!rsdt) goto oom;
    memcpy(rsdt, &Rsdt, sizeof(struct acpi_header));
    set_rsdt_oem(&rsdt->header);
    rsdt->entry[0] = (unsigned long)fadt_10;
    for ( i = 0; secondary_tables[i]; i++ )
        rsdt->entry[i+1] = secondary_tables[i];
    rsdt->header.length = sizeof(struct acpi_header) + (i+1)*sizeof(uint32_t);
    set_checksum(rsdt,
                 offsetof(struct acpi_header, checksum),
                 rsdt->header.length);

    /*
     * Fill in low-memory data structures: acpi_info and RSDP.
     */
    rsdp = (struct acpi_20_rsdp *)physical;

    memcpy(rsdp, &Rsdp, sizeof(struct acpi_20_rsdp));
    memcpy(rsdp->oem_id, hvm_info->oem_info.oem_id, 6);
    rsdp->rsdt_address = (unsigned long)rsdt;
    rsdp->xsdt_address = (unsigned long)xsdt;
    set_checksum(rsdp,
                 offsetof(struct acpi_10_rsdp, checksum),
                 sizeof(struct acpi_10_rsdp));
    set_checksum(rsdp,
                 offsetof(struct acpi_20_rsdp, extended_checksum),
                 sizeof(struct acpi_20_rsdp));

    acpi_info->com1_present = uart_exists(0x3f8);
    acpi_info->com2_present = uart_exists(0x2f8);
    acpi_info->lpt1_present = lpt_exists(0x378);
    acpi_info->hpet_present = hpet_exists(ACPI_HPET_ADDRESS);
    acpi_info->smc_present = smc_exists();
    acpi_info->pci_min = pci_mem_start;
    acpi_info->pci_len = pci_mem_end - pci_mem_start;

    return;

oom:
    printf("unable to build ACPI tables: out of memory\n");

}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
