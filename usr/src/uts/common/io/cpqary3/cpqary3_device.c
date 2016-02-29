/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Joyent, Inc.
 */

#include "cpqary3.h"

extern ddi_dma_attr_t cpqary3_dma_attr;
extern ddi_device_acc_attr_t cpqary3_dev_attributes;

/*
 * We must locate what the CISS specification describes as the "I2O
 * registers".  The Intelligent I/O (I2O) Architecture Specification describes
 * this somewhat more coherently as "the memory region specified by the first
 * base address configuration register indicating memory space (offset 10h,
 * 14h, and so forth)".
 */
static int
cpqary3_locate_bar(cpqary3_t *cpq, pci_regspec_t *regs, unsigned nregs,
    unsigned *i2o_bar)
{
	/*
	 * Locate the first memory-mapped BAR:
	 */
	for (unsigned i = 0; i < nregs; i++) {
		unsigned type = regs[i].pci_phys_hi & PCI_ADDR_MASK;
		unsigned bar = PCI_REG_REG_G(regs[i].pci_phys_hi);

		if (type == PCI_ADDR_MEM32 || type == PCI_ADDR_MEM64) {
			dev_err(cpq->dip, CE_WARN, "reg[%u]: bar found: %x",
			    i, bar);
			*i2o_bar = i;
			return (DDI_SUCCESS);
		}
	}

	return (DDI_FAILURE);
}

static int
cpqary3_locate_cfgtbl(cpqary3_t *cpq, pci_regspec_t *regs, unsigned nregs,
    unsigned *ct_bar, uint32_t *baseaddr)
{
	uint32_t cfg_offset, mem_offset;
	unsigned want_type;
	uint32_t want_bar;

	cfg_offset = cpqary3_get32(cpq, CISS_I2O_CFGTBL_CFG_OFFSET);
	mem_offset = cpqary3_get32(cpq, CISS_I2O_CFGTBL_MEM_OFFSET);

	VERIFY(cfg_offset != 0xffffffff);
	VERIFY(mem_offset != 0xffffffff);

	/*
	 * Locate the Configuration Table.  Three different values read
	 * from two I2O registers allow us to determine the location:
	 * 	- the correct PCI BAR offset is in the low 16 bits of
	 * 	  CISS_I2O_CFGTBL_CFG_OFFSET
	 *	- bit 16 is 0 for a 32-bit space, and 1 for 64-bit
	 *	- the memory offset from the base of this BAR is
	 *	  in CISS_I2O_CFGTBL_MEM_OFFSET
	 */
	want_bar = (cfg_offset & 0xffff);
	want_type = (cfg_offset & (1UL << 16)) ? PCI_ADDR_MEM64 :
	    PCI_ADDR_MEM32;

	dev_err(cpq->dip, CE_WARN, "want BAR %x of type %s; offset %x",
	    want_bar, want_type == PCI_ADDR_MEM64 ? "MEM64" : "MEM32",
	    mem_offset);

	for (unsigned i = 0; i < nregs; i++) {
		unsigned type = regs[i].pci_phys_hi & PCI_ADDR_MASK;
		unsigned bar = PCI_REG_REG_G(regs[i].pci_phys_hi);

		if (type != PCI_ADDR_MEM32 && type != PCI_ADDR_MEM64) {
			continue;
		}

		if (bar == want_bar) {
			dev_err(cpq->dip, CE_WARN, "reg[%u]: bar found: %x",
			    i, bar);

			*ct_bar = i;
			*baseaddr = mem_offset;
			return (DDI_SUCCESS);
		}
	}

	return (DDI_FAILURE);
}

static int
cpqary3_map_device(cpqary3_t *cpq)
{
	pci_regspec_t *regs;
	uint_t regslen, nregs;
	int r = DDI_FAILURE;

	/*
	 * Get the list of PCI registers from the DDI property "regs":
	 */
	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, cpq->dip,
	    DDI_PROP_DONTPASS, "reg", (int **)&regs, &regslen) !=
	    DDI_PROP_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not load \"reg\" DDI prop");
		return (DDI_FAILURE);
	}
	nregs = regslen * sizeof (int) / sizeof (pci_regspec_t);

	if (cpqary3_locate_bar(cpq, regs, nregs, &cpq->cpq_i2o_bar) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "did not find any memory BARs");
		goto out;
	}

	/*
	 * Map enough of the I2O memory space to enable us to talk to the
	 * device.
	 */
	if (ddi_regs_map_setup(cpq->dip, cpq->cpq_i2o_bar,
	    &cpq->cpq_i2o_space, CISS_I2O_MAP_BASE,
	    CISS_I2O_MAP_LIMIT - CISS_I2O_MAP_BASE,
	    &cpqary3_dev_attributes, &cpq->cpq_i2o_handle) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "failed to map I2O registers");
		goto out;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_I2O_MAPPED;

	if (cpqary3_locate_cfgtbl(cpq, regs, nregs, &cpq->cpq_ct_bar,
	    &cpq->cpq_ct_baseaddr) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not find config table");
		goto out;
	}

	/*
	 * Map the Configuration Table.
	 */
	if (ddi_regs_map_setup(cpq->dip, cpq->cpq_ct_bar,
	    (caddr_t *)&cpq->cpq_ct, cpq->cpq_ct_baseaddr,
	    sizeof (CfgTable_t), &cpqary3_dev_attributes,
	    &cpq->cpq_ct_handle) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not map config table");
		goto out;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_CFGTBL_MAPPED;

	r = DDI_SUCCESS;

out:
	ddi_prop_free(regs);
	return (r);
}

static int
cpqary3_identify_board(cpqary3_t *cpq)
{
	uint32_t board_id;
	int vid, sid;

	/*
	 * Identify the board.  The board ID is really a combination of the
	 * Subsystem Vendor ID and the Subsystem ID.  This is described in the
	 * CISS Specification, section "6.2 Initialization".
	 */
	if ((vid = ddi_prop_get_int(DDI_DEV_T_ANY, cpq->dip, DDI_PROP_DONTPASS,
	    "subsystem-vendor-id", -1)) == -1 ||
	    (sid = ddi_prop_get_int(DDI_DEV_T_ANY, cpq->dip, DDI_PROP_DONTPASS,
	    "subsystem-id", -1)) == -1) {
		dev_err(cpq->dip, CE_WARN, "could not get subsystem id");
		return (DDI_FAILURE);
	}
	board_id = ((vid & 0xffff) << 16) | (sid & 0xffff);

	if ((cpq->cpq_board = cpqary3_bd_getbybid(board_id)) == NULL) {
		dev_err(cpq->dip, CE_WARN, "unsupported controller; id %x",
		    board_id);
		return (DDI_FAILURE);
	}
	cpq->cpq_board_id = board_id;

	/*
	 * XXX
	 */
	dev_err(cpq->dip, CE_WARN, "controller: %s",
	    cpq->cpq_board->bd_dispname);

	return (DDI_SUCCESS);
}

int
cpqary3_device_setup(cpqary3_t *cpq)
{
	/*
	 * Ensure that the controller is installed in such a fashion that it
	 * may become a DMA master.
	 */
	if (ddi_slaveonly(cpq->dip) == DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "device cannot become DMA master");
		return (DDI_FAILURE);
	}

	if (cpqary3_identify_board(cpq) != DDI_SUCCESS ||
	    cpqary3_map_device(cpq) != DDI_SUCCESS) {
		goto fail;
	}

	return (DDI_SUCCESS);

fail:
	cpqary3_device_teardown(cpq);
	return (DDI_FAILURE);
}

void
cpqary3_device_teardown(cpqary3_t *cpq)
{
	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_CFGTBL_MAPPED) {
		ddi_regs_map_free(&cpq->cpq_ct_handle);
		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_CFGTBL_MAPPED;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_I2O_MAPPED) {
		ddi_regs_map_free(&cpq->cpq_i2o_handle);
		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_I2O_MAPPED;
	}
}

uint32_t
cpqary3_get32(cpqary3_t *cpq, offset_t off)
{
	uint32_t *addr = (uint32_t *)(cpq->cpq_i2o_space +
	    (off - CISS_I2O_MAP_BASE));

	VERIFY(off >= CISS_I2O_MAP_BASE);
	VERIFY(off < CISS_I2O_MAP_BASE + CISS_I2O_MAP_LIMIT);

	return (ddi_get32(cpq->cpq_i2o_handle, addr));
}

void
cpqary3_put32(cpqary3_t *cpq, offset_t off, uint32_t val)
{
	uint32_t *addr = (uint32_t *)(cpq->cpq_i2o_space +
	    (off - CISS_I2O_MAP_BASE));

	VERIFY(off >= CISS_I2O_MAP_BASE);
	VERIFY(off < CISS_I2O_MAP_BASE + CISS_I2O_MAP_LIMIT);

	return (ddi_put32(cpq->cpq_i2o_handle, addr, val));
}
