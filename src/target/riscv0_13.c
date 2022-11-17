/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 + This file implements the
 * RISC-V External Debug Support Version 0.13
 */

#include <limits.h>
#include "general.h"
#include "exception.h"
#include "target.h"
#include "target_internal.h"

#include "riscv.h"
#include "rv32i_isa.h"

static const char tdesc_rv32[] =
"<?xml version=\"1.0\"?>"
"<target>"
"  <architecture>riscv:rv32</architecture>"
"</target>";

enum DMI_OP {
	DMI_OP_NOP   = 0,
	DMI_OP_READ  = 1,
	DMI_OP_WRITE = 2,
};

enum DMI_REG {
	DMI_REG_ABSTRACTDATA0      = 0x04,
	DMI_REG_ABSTRACTDATA1      = 0x05,
	DMI_REG_ABSTRACTDATA_END   = 0x0f,
	DMI_REG_DMCONTROL          = 0x10,
	DMI_REG_DMSTATUS           = 0x11,
	DMI_REG_HARTINFO           = 0x12,
	DMI_REG_HALTSUM1           = 0x13,
	DMI_REG_HAWINDOWSEL        = 0x14,
	DMI_REG_HAWINDOW           = 0x15,
	DMI_REG_ABSTRACT_CS        = 0x16,
	DMI_REG_ABSTRACT_CMD       = 0x17,
	DMI_REG_ABSTRACT_AUTOEXEC  = 0x18,
	DMI_REG_CONFSTR_PTR0       = 0x19,
	DMI_REG_CONFSTR_PTR1       = 0x1a,
	DMI_REG_CONFSTR_PTR2       = 0x1b,
	DMI_REG_CONFSTR_PTR3       = 0x1c,
	DMI_REG_NEXTDM_ADDR        = 0x1d,
	DMI_REG_PROGRAMBUF_BEGIN   = 0x20,
	DMI_REG_PROGRAMBUF_END     = 0x2f,
	DMI_REG_AUTHDATA		   = 0x30,
	DMI_REG_HALTSUM2           = 0x34,
	DMI_REG_HALTSUM3 		   = 0x35,
	DMI_REG_SBADDRESS3		   = 0x37,
	DMI_REG_SYSBUSCS           = 0x38,
	DMI_REG_SBADDRESS0		   = 0x39,
	DMI_REG_SBADDRESS1		   = 0x3a,
	DMI_REG_SBADDRESS2		   = 0x3b,
	DMI_REG_SBDATA0			   = 0x3c,
	DMI_REG_SBDATA1			   = 0x3d,
	DMI_REG_SBDATA2			   = 0x3e,
	DMI_REG_SBDATA3			   = 0x3f,
	DMI_REG_HALTSUM0	 	   = 0x40,
};

enum ABSTRACTCMD_TYPE {
	ABSTRACTCMD_TYPE_ACCESS_REGISTER = 0x0,
	ABSTRACTCMD_TYPE_QUICK_ACCESS    = 0x1,
	ABSTRACTCMD_TYPE_ACCESS_MEMORY   = (0x2U << 24),
};

enum ABSTRACTCMD_AAMSIZE {
	ABSTRACTCMD_AAMSIZE_8bit   = (0U << 20),
	ABSTRACTCMD_AAMSIZE_16bit  = (1U << 20),
	ABSTRACTCMD_AAMSIZE_32bit  = (2U << 20),
	ABSTRACTCMD_AAMSIZE_64bit  = (3U << 20),
	ABSTRACTCMD_AAMSIZE_128bit = (4U << 20),
};

#define ABSTRACTCMD_AAMPOSTINCREMENT (1U << 19)
#define ABSTRACTCMD_TRANSFER         (1U << 17)

enum ABSTRACTCMD_ERR {
	ABSTRACTCMD_ERR_NONE = 0x0,
	ABSTRACTCMD_ERR_BUSY = 0x1,
	ABSTRACTCMD_ERR_NOT_SUPPORTED = 0x2,
	ABSTRACTCMD_ERR_EXCEPTION = 0x3,
	ABSTRACTCMD_ERR_HALT_RESUME = 0x4,
	ABSTRACTCMD_ERR_BUS = 0x5,
	ABSTRACTCMD_ERR_OTHER = 0x7,
};

enum AUTOEXEC_STATE {
	AUTOEXEC_STATE_NONE, /* Ingnore autoexec */
	AUTOEXEC_STATE_INIT, /* Setup everything + AARAUTOINC */
	AUTOEXEC_STATE_CONT, /* Only access data0 register */
};

enum HART_REG {
	HART_REG_CSR_BEGIN   = 0x0000,
	HART_REG_CSR_MISA    = 0x0301,
	HART_REG_CSR_DCSR    = 0x07b0,
	HART_REG_CSR_DPC     = 0x07b1,
	HART_REG_CSR_MACHINE = 0x0f11,
	HART_REG_CSR_MHARTID = 0x0f14,
	HART_REG_CSR_END     = 0x0fff,
	HART_REG_GPR_BEGIN   = 0x1000,
	HART_REG_GPR_END     = 0x101f,
};

#define DMSTATUS_GET_VERSION(x)         DTMCS_GET_VERSION(x)
#define DMSTATUS_GET_CONFSTRPTRVALID(x) ((x >> 4) & 0x1)
#define DMSTATUS_GET_HASRESETHALTREQ(x) ((x >> 5) & 0x1)
#define DMSTATUS_GET_AUTHBUSY(x)		((x >> 6) & 0x1)
#define DMSTATUS_GET_AUTHENTICATED(x)   ((x >> 7) & 0x1)
#define DMSTATUS_GET_ANYNONEXISTENT(x)  ((x >> 14) & 0x1)
#define DMSTATUS_GET_ALLRESEUMSET(x)    ((x >> 17) & 0x1)
#define DMSTATUS_GET_ANYHAVERESET(x)    ((x >> 18) & 0x1)
#define DMSTATUS_GET_IMPEBREAK(x)	    ((x >> 22) & 0x1)
#define DMSTATUS_GET_ALLHALTED(x)       ((x >> 9) & 0x1)

#define DMCONTROL_GET_HARTSEL(x)      (((x >> 16) & 0x3ff) | (((x >> 6) & 0x3ff) << 10))
#define DMCONTROL_MK_HARTSEL(s)       (((s) & 0x3ff) << 16) | ((s) & (0x3ff << 10) >> 4)
#define DMCONTROL_HASEL               (0x1 << 26)
#define DMCONTROL_HALTREQ             (0x1U << 31)
#define DMCONTROL_RESUMEREQ           (0x1U << 30)
#define DMCONTROL_HARTRESET           (0x1U << 29)
#define DMCONTROL_DMACTIVE            (0x1)
#define DMCONTROL_NDMRESET		      (0x1U << 1)
#define DMCONTROL_ACKHAVERESET        (0x1 << 28)
#define DMCONTROL_SRESETHALTREQ       (0x1U << 3)
#define DMCONTROL_CRESETHALTREQ       (0x1U << 2)

#define ABSTRACTCS_GET_DATACOUNT(x)   (x & 0xf)
#define ABSTRACTCS_GET_CMDERR(x)      ((x >> 8) & 0x7)
#define ABSTRACTCS_CLEAR_CMDERR(t) do { t |= (0x7 << 8);} while (0)
#define ABSTRACTCS_GET_BUSY(x)		  ((x >> 12) & 0x1)
#define ABSTRACTCS_GET_PROGBUFSIZE(x) ((x >> 24) & 0x1f)

#define ABSTRACTAUTO_AUTOEXECPROGBUF  (1U << 16)
#define ABSTRACTAUTO_AUTOEXECDATA     (1U <<  0)

#define SBCS_SBREADONDATA             (1U << 15)
#define SBCS_SBAUTOINCREMENT          (1U << 16)
#define SBCS_SBACCESS_8BIT            (0U << 17)
#define SBCS_SBACCESS_16BIT           (1U << 17)
#define SBCS_SBACCESS_32BIT           (2U << 17)
#define SBCD_SBREADONADDR             (1U << 20)

#define ABSTRACTCMD_SET_TYPE(t, s) do { \
	t &= ~(0xff << 24); \
	t |= (s & 0xff) << 24; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_AARSIZE(t, s) do { \
	t &= ~(0x7 << 20); \
	t |= (s & 0x7) << 20; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_AARPOSTINCREMENT(t, s) do { \
	t &= ~(0x1 << 19); \
	t |= (s & 0x1) << 19; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_POSTEXEC(t, s) do { \
	t &= ~(0x1 << 18); \
	t |= (s & 0x1) << 18; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_TRANSFER(t, s) do { \
	t &= ~(0x1 << 17); \
	t |= (s & 0x1) << 17; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_WRITE(t, s)    do { \
	t &= ~(0x1 << 16); \
	t |= (s & 0x1) << 16; } while (0)
#define ABSTRACTCMD_ACCESS_REGISTER_SET_REGNO(t, s)    do { \
	t &= ~(0xffff); \
	t |= s & 0xffff; } while (0)

#define ABSTRACTAUTO_SOME_PATTEN		(0b101010101010)
#define ABSTRACTAUTO_GET_DATA(x)        (x & 0xfff)
#define ABSTRACTAUTO_SET_DATA(t, s)     do { \
	t &= ~(0xfff); \
	t |= s & 0xfff; } while (0)

/* CSR Register bits */
#define CSR_DCSR_STEP (1U << 2)

#define RISCV_MAX_HARTS 32U

void rvdbd_dmi_ref(RVDBGv013_DMI_t *dtm)
{
    dtm->refcnt++;
}

void rvdbd_dmi_unref(RVDBGv013_DMI_t *dtm)
{
    if (--dtm->refcnt == 0) {
	dtm->rvdbg_dmi_free(dtm);
    }
}

/* Busy is only seen with the second dmi access */
static int rvdbg_dmi_write(RVDBGv013_DMI_t *dmi, uint32_t addr, uint32_t data)
{
	int res = -1;
	res = dmi->rvdbg_dmi_low_access(
		dmi, NULL, ((uint64_t)addr << DMI_BASE_BIT_COUNT) | ((uint64_t)data << 2) | DMI_OP_WRITE);
	DEBUG_TARGET("DMI write add %08" PRIx32 ", data %08" PRIx32 "\n", addr, data);
	return res;
}

static int rvdbg_dmi_read(RVDBGv013_DMI_t *dmi, uint32_t addr, uint32_t *data)
{
	int res = 0;
	dmi->rvdbg_dmi_low_access(dmi, NULL, ((uint64_t)addr << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
	res = dmi->rvdbg_dmi_low_access(dmi, data, DMI_OP_NOP);
	DEBUG_TARGET("DMI Read addr %08" PRIx32 "%s:data %x\n", addr, (res == -1) ? "failed" : "", *data);
	return res;
}

int rvdbg_set_debug_version(RVDBGv013_DMI_t *dmi, uint8_t version)
{
	switch (version) {
		case RISCV_DEBUG_VERSION_013:
			dmi->debug_version = version;
			break;
		case RISCV_DEBUG_VERSION_011:
			DEBUG_WARN("Error: RISC-V debug 0.11 not supported\n");
			return -1;
		case RISCV_DEBUG_VERSION_UNKNOWN:
		default:
			DEBUG_WARN("RISC-V target unknown debug spec verson: %d\n", version);
			return -1;
	}

	return 0;
}

#ifdef ENABLE_DEBUG
static const char* rvdbg_version_tostr(enum RISCV_DEBUG_VERSION version)
{
	switch (version) {
		case RISCV_DEBUG_VERSION_011:
			return "0.11";
		case RISCV_DEBUG_VERSION_013:
			return "0.13";
		case RISCV_DEBUG_VERSION_UNKNOWN:
		default:
			return "UNKNOWN";
	}
}
#endif /* ENABLE_DEBUG */

static int rvdbg_halt_current_hart(RVDBGv013_DMI_t *dmi)
{
	uint32_t dmcontrol;

	DEBUG_INFO("current hart = %d\n", dmi->current_hart);

	if (rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol) < 0)
			return -1;

	if (!(dmcontrol & DMCONTROL_DMACTIVE)) {
		/* Enable hart first */
			dmcontrol |= DMCONTROL_DMACTIVE;
			if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
				return -1;
	}
	/* Clear reset */
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol | DMCONTROL_ACKHAVERESET) < 0)
		return -1;
	dmcontrol |= DMCONTROL_HALTREQ;
	// Trigger the halt request
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
		return -1;
	platform_timeout timeout;
	/* The risc debug doc reads as if HALTREQ wakes up sleeping hart
	 * So assume a short time for reaction
	 */
	platform_timeout_set(&timeout, 50);
	uint32_t dmstatus = 0;
	// Now wait for the hart to halt
	while (!(DMSTATUS_GET_ALLHALTED(dmstatus))) {
		if (rvdbg_dmi_read(dmi, DMI_REG_DMSTATUS, &dmstatus) < 0)
			return -1;
		if (DMSTATUS_GET_ANYHAVERESET(dmstatus)) {
			DEBUG_WARN("RISC-V: got reset, while trying to halt\n");
			if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol | DMCONTROL_ACKHAVERESET) < 0)
				return -1;
		}
		if (DMSTATUS_GET_ALLHALTED(dmstatus))
			break;
		if (platform_timeout_is_expired(&timeout)) {
			DEBUG_WARN("Timeout waiting for halt\n");
			return -1;
		}
	}
	if (DMSTATUS_GET_HASRESETHALTREQ(dmstatus)) {
		/* Request halt on reset */
		dmcontrol |= DMCONTROL_SRESETHALTREQ;
		int res = rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol);
		if (res) {
			DEBUG_WARN("Write DMCONTROL failed\n");
			return -1;
		}
	} else {
		DEBUG_INFO("Debug Module does not supports halt-on-reset!\n");
	}
	return 0;
}

static void rvdbg_halt_request(target *t)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	res = rvdbg_halt_current_hart(dmi);
	if (res) {
		DEBUG_WARN("Can not halt target\n");
		dmi->error = true;
	}
}

static int rvdbg_discover_hart(RVDBGv013_DMI_t *dmi)
{
	uint32_t hartinfo;
	HART_t *hart = &dmi->harts[dmi->current_hart];

	if (rvdbg_dmi_read(dmi, DMI_REG_HARTINFO, &hartinfo) < 0)
		return -1;

	hart->dataaddr = hartinfo & 0xfff;
	hart->datasize = (hartinfo >> 12) & 0xf;
	hart->dataaccess = (hartinfo >> 16) & 0x1;
	hart->nscratch = (hartinfo >> 20) & 0xf;

	return 0;
}

static int rvdbg_discover_harts(RVDBGv013_DMI_t *dmi)
{
	uint32_t hart_idx, hartsellen, dmstatus, dmcontrol;

	dmi->current_hart = 0;

	// Set all 20 bits of hartsel
	dmcontrol = DMCONTROL_DMACTIVE | DMCONTROL_MK_HARTSEL(0xfffff);

	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
		return -1;

	if (rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol) < 0)
		return -1;

	hartsellen = DMCONTROL_GET_HARTSEL(dmcontrol);
	dmi->hartsellen = 0;

	while (hartsellen & 0x1) {
		dmi->hartsellen++;
		hartsellen >>= 1;
	}

	DEBUG_INFO("hartsellen = %d\n", dmi->hartsellen);

	// Iterate over all possible harts
	for (hart_idx = 0; hart_idx < MIN(1U << dmi->hartsellen, RISCV_MAX_HARTS)
			&& dmi->num_harts < ARRAY_NUMELEM(dmi->harts); hart_idx++) {
		dmcontrol = DMCONTROL_DMACTIVE | DMCONTROL_MK_HARTSEL(hart_idx);
	dmi->current_hart = hart_idx;

		if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
			return -1;

		// Check if anynonexist is true -> abort
		if (rvdbg_dmi_read(dmi, DMI_REG_DMSTATUS, &dmstatus) < 0)
			return -1;

		if (DMSTATUS_GET_ANYNONEXISTENT(dmstatus)) {
			DEBUG_TARGET("Hart idx 0x%05x does not exist\n", hart_idx);
			break;
		}

		if (DMSTATUS_GET_ANYHAVERESET(dmstatus)) {
			DEBUG_WARN("Hart idx 0x%05x has reset, acknowledge\n", hart_idx);
			dmcontrol = DMCONTROL_DMACTIVE | DMCONTROL_MK_HARTSEL(hart_idx) | DMCONTROL_ACKHAVERESET;
			if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
				return -1;
		}

		if (rvdbg_discover_hart(dmi) < 0)
			return -1;

		dmi->num_harts++;
	}

	DEBUG_INFO("num_harts = %d\n", dmi->num_harts);

	// Select hart0 as current
	dmcontrol = DMCONTROL_DMACTIVE | DMCONTROL_MK_HARTSEL(0);
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
		return -1;
	dmi->current_hart = 0;

	return 0;
}

/**
 * Returns negative error, or positive cmderror
 */
static int rvdbg_abstract_command_run(RVDBGv013_DMI_t *dmi, uint32_t command)
{
	uint32_t abstractcs;
	uint8_t cmderror;

retry:
	if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_CMD, command) < 0)
		return -1;

	// Wait until the abstract command finished
	do {
		if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACT_CS, &abstractcs) < 0)
			return -1;
	} while (ABSTRACTCS_GET_BUSY(abstractcs));

	cmderror = ABSTRACTCS_GET_CMDERR(abstractcs);

	if (cmderror != ABSTRACTCMD_ERR_NONE) {
		// Clear the error
		abstractcs = 7 << 8;
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_CS, abstractcs) < 0)
			return -1;

		// Handle ERR_BUSY retries automatically
		switch (cmderror) {
		case 0:
			break;
		case ABSTRACTCMD_ERR_BUSY:
			DEBUG_WARN("RISC-V abstract command busy, retry...\n");
			goto retry;
		case ABSTRACTCMD_ERR_HALT_RESUME:
			DEBUG_WARN("RISC-V abstract command 0x%08x not supported in run/halt state\n", command);
			break;
		case ABSTRACTCMD_ERR_NOT_SUPPORTED:
			DEBUG_WARN("RISC-V abstract command 0x%08x not supported\n", command);
			break;
		default:
			DEBUG_WARN("RISC-V abstract command 0x%08x,err %d\n", command, cmderror);
		}
	}

	return cmderror;
}

static int rvdbg_read_single_reg(RVDBGv013_DMI_t *dmi, uint16_t reg_idx, uint32_t *out,
	enum AUTOEXEC_STATE astate)
{
	uint32_t command = 0;
	uint32_t abstractcs;
	int ret;

	// Construct abstract command
	// TODO: Do not expect XLEN of 32 by default
	ABSTRACTCMD_SET_TYPE(command, ABSTRACTCMD_TYPE_ACCESS_REGISTER);
	ABSTRACTCMD_ACCESS_REGISTER_SET_AARSIZE(command, BUS_ACCESS_32);
	ABSTRACTCMD_ACCESS_REGISTER_SET_TRANSFER(command, 1);
	ABSTRACTCMD_ACCESS_REGISTER_SET_REGNO(command, reg_idx);
	ABSTRACTCMD_ACCESS_REGISTER_SET_AARPOSTINCREMENT(command,
		astate == AUTOEXEC_STATE_INIT ? 1 : 0);

	// Avoid writing command, when in autoexec cont mode
	if (astate != AUTOEXEC_STATE_CONT) {
		// Initiate register read command
		if ((ret = rvdbg_abstract_command_run(dmi, command)) < 0)
			return -1;

		// Handle error
		switch (ret) {
			case ABSTRACTCMD_ERR_NONE:
				break;
			case ABSTRACTCMD_ERR_EXCEPTION:
				// TODO: This check becomes invalid as soon as postexec is set.
				DEBUG_WARN("RISC-V register 0x%"PRIx16"\n does not exist", reg_idx);
				return -1;
			default:
				DEBUG_WARN("RISC-V abstract command error: %d\n", ret);
				return -1;
		}
	}

	if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACTDATA0, out) < 0)
		return -1;

	if (astate == AUTOEXEC_STATE_CONT) {
		// In cont mode, only read when not busy (not guarded by rvdbg_abstract_command_run)
		do {
			if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACT_CS, &abstractcs) < 0)
				return -1;
		} while (ABSTRACTCS_GET_BUSY(abstractcs));
	}

	return 0;
}

static int rvdbg_write_single_reg(RVDBGv013_DMI_t *dmi, uint16_t reg_id, uint32_t value,
	enum AUTOEXEC_STATE astate)
{
	uint32_t command = 0;
	uint32_t abstractcs;
	int ret;

	// Write value to data0
	if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACTDATA0, value) < 0)
		return -1;

	// Construct abstract command
	// TODO: Do not expect XLEN of 32 by default
	ABSTRACTCMD_SET_TYPE(command, ABSTRACTCMD_TYPE_ACCESS_REGISTER);
	ABSTRACTCMD_ACCESS_REGISTER_SET_AARSIZE(command, BUS_ACCESS_32);
	ABSTRACTCMD_ACCESS_REGISTER_SET_TRANSFER(command, 1);
	ABSTRACTCMD_ACCESS_REGISTER_SET_WRITE(command, 1);
	ABSTRACTCMD_ACCESS_REGISTER_SET_REGNO(command, reg_id);
	ABSTRACTCMD_ACCESS_REGISTER_SET_AARPOSTINCREMENT(command,
		astate == AUTOEXEC_STATE_INIT ? 1 : 0);

	// Only initiate the write, if not in autoexec cont state
	if (astate != AUTOEXEC_STATE_CONT) {
		// Initiate register write command
		if ((ret = rvdbg_abstract_command_run(dmi, command)) < 0)
			return -1;

		// Handle error
		switch (ret) {
			case ABSTRACTCMD_ERR_NONE:
				break;
			case ABSTRACTCMD_ERR_EXCEPTION:
				// TODO: This check becomes invalid as soon as postexec is set.
				DEBUG_WARN("RISC-V register 0x%"PRIx16"\n does not exist", reg_id);
				return -1;
			default:
				DEBUG_WARN("RISC-V abstract command error: %d\n", ret);
				return -1;
		}
	} else {
		// When in cont state, make sure to wait until write is done
		do {
			if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACT_CS, &abstractcs) < 0)
				return -1;
		} while (ABSTRACTCS_GET_BUSY(abstractcs));
	}


	return 0;
}

static int rvdbg_write_regs(RVDBGv013_DMI_t *dmi, uint16_t reg_id, const uint32_t *values,
	uint16_t len)
{
	enum AUTOEXEC_STATE astate = AUTOEXEC_STATE_NONE;
	uint32_t abstractauto;
	uint16_t i;
	int err = 0;

	// When more than one reg written and autoexec support
	if (len > 1  && dmi->support_autoexecdata) {
		astate = AUTOEXEC_STATE_INIT;
		abstractauto = 0;
		ABSTRACTAUTO_SET_DATA(abstractauto, ABSTRACTAUTO_SOME_PATTEN);
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
			return -1;
	}

	for (i = 0; i < len; i++) {
		if (rvdbg_write_single_reg(dmi, reg_id + i, values[i], astate) < 0) {
			err = -1;
			break;
		}
		if (astate == AUTOEXEC_STATE_INIT)
			astate = AUTOEXEC_STATE_CONT;
	}

	// Reset auto exec state
	if (astate != AUTOEXEC_STATE_NONE) {
		abstractauto = 0;
		ABSTRACTAUTO_SET_DATA(abstractauto, 0);
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
			return -1;
	}

	return err;
}

static int rvdbg_read_regs(RVDBGv013_DMI_t *dmi, uint16_t reg_id, uint32_t *values,
	uint16_t len)
{
	enum AUTOEXEC_STATE astate = AUTOEXEC_STATE_NONE;
	uint32_t abstractauto;
	uint16_t i;
	int err = 0;

	// When more than one reg read and autoexec support
	if (len > 1  && dmi->support_autoexecdata) {
		astate = AUTOEXEC_STATE_INIT;
		abstractauto = 0;
		ABSTRACTAUTO_SET_DATA(abstractauto, ABSTRACTAUTO_SOME_PATTEN);
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
			return -1;
	}

	for (i = 0; i < len; i++) {
		if (rvdbg_read_single_reg(dmi, reg_id + i, &values[i], astate) < 0) {
			err = -1;
			break;
		}
		if (astate == AUTOEXEC_STATE_INIT)
			astate = AUTOEXEC_STATE_CONT;
	}

	// Reset auto exec state
	if (astate != AUTOEXEC_STATE_NONE) {
		abstractauto = 0;
		ABSTRACTAUTO_SET_DATA(abstractauto, 0);
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
			return -1;
	}

	return err;
}

static void rvdbg_regs_read(target *t, void *regs_data)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	res = rvdbg_read_regs(dmi, 0x1000, regs_data, (t->regs_size / 4) - 1);
	if (res) {
		DEBUG_INFO("rvdbg_read_regs failed\n");
		dmi->error = true;
		return;
	}
	res = rvdbg_read_single_reg(dmi, HART_REG_CSR_DPC, regs_data + t->regs_size -4, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_INFO("rvdbg_read_regs PC failed\n");
		dmi->error = true;
		return;
	}
	return;
}

static void rvdbg_regs_write(target *t, const void *regs_data)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	res = rvdbg_write_regs(dmi, 0x1000, regs_data, (t->regs_size / 4) - 1);
	if (res) {
		DEBUG_INFO("rvdbg_write_regs failed\n");
		dmi->error = true;
		return;
	}
	uint32_t *data = (uint32_t *)regs_data;
	res = rvdbg_write_single_reg(dmi, HART_REG_CSR_DPC, data[t->regs_size / 4 - 4], AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_INFO("rvdbg_write_reg PC failed\n");
		dmi->error = true;
		return;
	}
	return;
}

static ssize_t rvdbg_reg_read(target *t, int reg, void *data, size_t max)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	if (max < 4) /* assume all registers 4 byte*/
		return -1;
	res = rvdbg_read_single_reg(dmi, reg, data, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_INFO("rvdbg_reg_read  failed\n");
		return -1;
	}
	return 4;
}

static ssize_t rvdbg_reg_write(target *t, int reg, const void *data, size_t max)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	if (max < 4) /* assume all registers 4 byte*/
		return -1;
	res = rvdbg_write_single_reg(dmi,reg, *(uint32_t*)data, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_INFO("rvdbg_reg_write failed\n");
		return -1;
	}
	return 4;
}

static int rvdbg_progbuf_upload(RVDBGv013_DMI_t *dmi, const uint32_t* buffer, uint8_t buffer_len)
{
	uint8_t i;
	uint8_t total_avail_size = dmi->progbuf_size - (dmi->impebreak ? 0 : 1);

	if (buffer_len > total_avail_size) {
		DEBUG_WARN("RISC-V: progbuf upload size %d too big\n", buffer_len);
		return -1;
	}

	for (i = DMI_REG_PROGRAMBUF_BEGIN; i < buffer_len; i++) {
		if (rvdbg_dmi_write(dmi, DMI_REG_PROGRAMBUF_BEGIN + i, buffer[i]) < 0)
			return -1;
	}

	// Add ebreak, if there is extra space.
	if (i < total_avail_size) {
		if (rvdbg_dmi_write(dmi, DMI_REG_PROGRAMBUF_BEGIN + i, RV32I_ISA_EBREAK) < 0)
			return -1;
	}

	return 0;
}

// TODO: Backup and restore registers externally for performance opt
static int rvdbg_progbuf_exec(RVDBGv013_DMI_t *dmi, uint32_t *args, uint8_t argin_len,
	uint8_t argout_len)
{
	int ret;
	uint8_t backup_len;
	uint32_t command = 0;
	// Back up registers for progbuf communication (excludes x0)
	// TODO: Do not assume XLEN 32
	uint32_t gp_register_backup[31];
	ABSTRACTCMD_SET_TYPE(command, ABSTRACTCMD_TYPE_ACCESS_REGISTER);
	ABSTRACTCMD_ACCESS_REGISTER_SET_POSTEXEC(command, 1);
	DEBUG_INFO("rvdbg_progbuf_exec:");
	for (int i = 0; i < argin_len; i++)
		DEBUG_INFO(" %" PRIx32, args[i]);
	DEBUG_INFO("\n");
	// How many registers have to be backed up?
	backup_len = MAX(argin_len, argout_len);

	if (backup_len > 31) {
		DEBUG_WARN("RISC-V: Too many requested argument registers\n");
		return -1;
	}

	// Backup argument registers
	if (rvdbg_read_regs(dmi, HART_REG_GPR_BEGIN + 1, gp_register_backup,
			backup_len) < 0)
		return -1;

	// Write all in arguments to GPRs
	if (rvdbg_write_regs(dmi, HART_REG_GPR_BEGIN + 1, args, argin_len) < 0)
		return -1;

	// Start command
	if ((ret = rvdbg_abstract_command_run(dmi, command)) < 0)
		return -1;

	// Handle cmderror
	switch (ret) {
		case ABSTRACTCMD_ERR_EXCEPTION:
			DEBUG_WARN("RISC-V: Exception in progbuf execution\n");
			return -1;
		default:
			DEBUG_WARN("RISC-V: Failed to execute progbuf, error %d\n", ret);
			return -1;
	}

	// Copy result
	if (rvdbg_read_regs(dmi, HART_REG_GPR_BEGIN + 1, args, argout_len) < 0)
		return -1;

	// Restore backup regs
	if (rvdbg_write_regs(dmi, HART_REG_GPR_BEGIN + 1,
			gp_register_backup,
			backup_len) < 0)
		return -1;

	return 0;
}

static int rvdbg_read_csr_progbuf(RVDBGv013_DMI_t *dmi, uint16_t reg_id, uint32_t* value)
{
	// Store result in x1
	uint32_t program[] = {
		RV32I_ISA_CSRRS(1, reg_id, 0)
	};

	if (rvdbg_progbuf_upload(dmi, program, ARRAY_NUMELEM(program)) < 0)
		return -1;

	// exec with 0 in registers and 1 out register, this reserves x1 as an output register
	if (rvdbg_progbuf_exec(dmi, value, 0, 1) < 0)
		return -1;

	return 0;
}

// static int rvdbg_write_csr_progbuf(RVDBGv013_DMI_t *dmi, uint16_t reg_id, uint32_t value) { }

static void rvdbg_mem_read_abstract(target *t, void* dest, target_addr address, size_t len)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	if (!dest) {
		DEBUG_WARN("rvdbg_mem_read_abstract invalid buffer\n");
		dmi->error = true;
		return;
	}
	if (!len)
		return;
	int res;
	if (address & 3) {
		DEBUG_WARN("abstract unaligned!\n");
		/* Align start address */
		uint8_t preread[4], *p = preread;
		rvdbg_mem_read_abstract(t, preread, address & ~3, 4);
		if (dmi->error) {
			DEBUG_WARN("rvdbg_mem_read_abstract preread failed\n");
			return;
		}
		int pre_run = (address & 3);
		p += pre_run;
		unsigned int remainder = 4 - pre_run;
		int count = MIN(remainder, len);
		memcpy(dest, p, count);
		address += count;
		len -= count;
	}
	if (!len)
		return;
	if (len > 4) {
		uint32_t abstractauto = ABSTRACTAUTO_AUTOEXECDATA;
		res  = rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto);
		if (res) {
			DEBUG_INFO("rvdbg_mem_read_abstract: write abstractauto failed\n");
			dmi->error = true;
			return;
		}
	}
	res  = rvdbg_dmi_write(dmi, DMI_REG_ABSTRACTDATA1, address);
	if (res) {
		DEBUG_INFO("rvdbg_mem_read_abstract: write address failed\n");
		dmi->error = true;
		return;
	}
	uint32_t command = ABSTRACTCMD_TYPE_ACCESS_MEMORY | ABSTRACTCMD_AAMSIZE_32bit;
	if (len > 4)
		command |=  ABSTRACTCMD_AAMPOSTINCREMENT;
	res  = rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_CMD, command);
	if (res) {
		DEBUG_INFO("rvdbg_mem_read_abstract: write command failed\n");
		dmi->error = true;
		return;
	}
	res = dmi->rvdbg_dmi_low_access(dmi, NULL, ((uint64_t)DMI_REG_ABSTRACTDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
	if (res) {
		DEBUG_WARN("Read start %d failed\n", len);
		dmi->error = true;
		return;
	}
	uint32_t data;
	while (len) {
		res = dmi->rvdbg_dmi_low_access(dmi, &data, ((uint64_t)DMI_REG_ABSTRACTDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
		if (res) {
			DEBUG_WARN("Read at len %d failed\n", len);
			dmi->error = true;
			return;
		}
		size_t chunk = MIN(len, 4);
		memcpy(dest, &data, chunk);
		dest += chunk;
		len -= chunk;
		if (!len) {
			res  = rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_CMD, 0);
			if (res) {
				DEBUG_INFO("rvdbg_mem_read_disable autoexec failed\n");
				dmi->error = true;
				return;
			}
		}
	}
	res = dmi->rvdbg_dmi_low_access(dmi, &data, ((uint64_t)DMI_REG_ABSTRACTDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
	if (res) {
		DEBUG_WARN("Last read failed\n");
		dmi->error = true;
		return;
	}
	memcpy(dest, &data, len);
}

static void rvdbg_mem_read_systembus(target *t,  void* dest, target_addr address, size_t len)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	if (!dest) {
		DEBUG_WARN("rvdbg_mem_read_systembus invalid buffer\n");
		dmi->error = true;
		return;
	}
	if (!len)
		return;
	int res;
	if (address & 3) {
		/* Align start address */
		uint8_t preread[4], *p = preread;
		rvdbg_mem_read_systembus(t, preread, address & ~3, 4);
		if (dmi->error) {
			DEBUG_WARN("rvdbg_mem_read_systembus preread failed\n");
			return;
		}
		int pre_run = (address & 3);
		p += pre_run;
		unsigned int remainder = 4 - pre_run;
		int count = MIN(remainder, len);
		memcpy(dest, p, count);
		address += count;
		len -= count;
	}
	if (!len)
		return;
	uint32_t sbcs = SBCS_SBACCESS_32BIT | SBCD_SBREADONADDR;
	if (len > 4)
		sbcs |= SBCS_SBREADONDATA | SBCS_SBAUTOINCREMENT;
	res  = rvdbg_dmi_write(dmi, DMI_REG_SYSBUSCS, sbcs);
	if (res) {
		DEBUG_INFO("rvdbg_mem_read_systembus: SBCS write failed\n");
		dmi->error = true;
		return;
	}
	res = rvdbg_dmi_write(dmi, DMI_REG_SBADDRESS0, address);
	if (res) {
		DEBUG_INFO("rvdbg_mem_read_systembus: Address write failed\n");
		dmi->error = true;
		return;
	}
	res = dmi->rvdbg_dmi_low_access(dmi, NULL, ((uint64_t)DMI_REG_SBDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
	if (res) {
		DEBUG_WARN("Read start %d failed\n", len);
		dmi->error = true;
		return;
	}
	uint32_t data;
	while (len > 4) {
		res = dmi->rvdbg_dmi_low_access(dmi, &data, ((uint64_t)DMI_REG_SBDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
		if (res) {
			DEBUG_WARN("Write read at len %d failed\n", len);
			dmi->error = true;
			return;
		}
		size_t chunk = MIN(len, 4);
		memcpy(dest, &data, chunk);
		dest += chunk;
		len -= chunk;
		if (!len) {
			res  = rvdbg_dmi_write(dmi, DMI_REG_SYSBUSCS, 0);
			if (res) {
				DEBUG_INFO("rvdbg_mem_read_disable autoread: SBCS write failed\n");
				dmi->error = true;
				return;
			}
		}
	}
	res = dmi->rvdbg_dmi_low_access(dmi, &data, ((uint64_t)DMI_REG_SBDATA0 << DMI_BASE_BIT_COUNT) | DMI_OP_READ);
	if (res) {
		DEBUG_WARN("Last read failed\n");
		dmi->error = true;
		return;
	}
	memcpy(dest, &data, len);
}

static int rvdbg_read_mem_progbuf(RVDBGv013_DMI_t *dmi, uint32_t address, uint32_t len, uint8_t* value)
{
	// Select optimal transfer size
	enum BUS_ACCESS width;
	uint32_t width_bytes;
	uint32_t i;
	uint32_t args[2];

	if (address % 4 == 0 && len > 4) {
		width = BUS_ACCESS_32;
		width_bytes = 4;
	} else if (address % 2 == 0 && len > 2) {
		width = BUS_ACCESS_16;
		width_bytes = 2;
	} else {
		width = BUS_ACCESS_8;
		width_bytes = 1;
	}

	// Load instruction with zero extend, x1 is target for data,
	// x2 is load address.
	uint32_t program[] = {
		RV32I_ISA_LOAD(1, width, RV32I_ISA_LOAD_ZERO_EXTEND, 2, 0),
	};
	DEBUG_WARN("RV32I_ISA_LOAD 0x%08" PRIx32 "\n", program[0]);
	if (rvdbg_progbuf_upload(dmi, program, ARRAY_NUMELEM(program)) < 0)
		dmi->error = true;

	// Go over memory addresses in width steps, copy from x1
	// result to value.
	for (i = 0; i < len; i += width_bytes) {
		// Set x2
		args[1] = address + i;
		if (rvdbg_progbuf_exec(dmi, args, 1, 2) < 0)
			dmi->error = true;
		memcpy(value + i, &args[0], width_bytes);
	}

	// If i is not exactly len some spare bytes are left,
	// call function recursively with remainder.
	if (i != len) {
		i -= width_bytes;
		return rvdbg_read_mem_progbuf(dmi, address + i, len - i, value + i);
	}

	return 0;
}

// static void rvdbg_write_mem_progbuf(RVDBGv013_DMI_t *dmi, uint32_t address, uint32_t len, const uint8_t *value); { }


static int rvdbg_select_mem_and_csr_access_impl(RVDBGv013_DMI_t *dmi)
{
	uint32_t abstractcs, abstractauto;
	uint8_t total_avail_progbuf;

	if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACT_CS, &abstractcs) < 0)
		return -1;

	dmi->progbuf_size = ABSTRACTCS_GET_PROGBUFSIZE(abstractcs);
	dmi->abstract_data_count = ABSTRACTCS_GET_DATACOUNT(abstractcs);

	if (dmi->abstract_data_count < 1 || dmi->abstract_data_count > 12) {
		// Invalid count of abstract data
		DEBUG_WARN("RISC-V: Invalid count of abstract data: %d\n", dmi->abstract_data_count);
		return -1;
	}

	if (dmi->progbuf_size > 16) {
		// Invalid progbuf size
		DEBUG_WARN("RISC-V: progbufsize is too large: %d\n", dmi->progbuf_size);
		return -1;
	} else if (dmi->progbuf_size == 1 && !dmi->impebreak) {
		// When progbufsize is 1, impebreak is required.
		DEBUG_WARN("RISC-V: progbufsize 1 requires impebreak feature\n");
		return -1;
	}

	DEBUG_INFO("datacount = %d\n", dmi->abstract_data_count);

	// Check if a program buffer is supported, and it is sufficient for accessing
	// CSR and / or MEMORY.
	// At minimum one available instruction required for csr and mem access over
	// progbuf.
	// --------------------------------------------------------------------------
	total_avail_progbuf = dmi->progbuf_size - (dmi->impebreak ? 0 : 1);
	if (total_avail_progbuf >= 1) {
		// PROGBUF supported
		DEBUG_INFO("RISC-V: Program buffer with available size %d supported.\n", total_avail_progbuf);

		dmi->read_csr = rvdbg_read_csr_progbuf;
		// dmi->write_csr = rvdbg_write_csr_progbuf;
		dmi->read_mem = rvdbg_read_mem_progbuf;
		// dmi->write_mem = rvdbg_write_mem_progbuf;
	}

	// Check if autoexecdata feature can be used
	// -----------------------------------------
	abstractauto = 0;
	ABSTRACTAUTO_SET_DATA(abstractauto, ABSTRACTAUTO_SOME_PATTEN);
	if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
		return -1;
	if (rvdbg_dmi_read(dmi, DMI_REG_ABSTRACT_AUTOEXEC, &abstractauto) < 0)
		return -1;

	if (ABSTRACTAUTO_GET_DATA(abstractauto) == ABSTRACTAUTO_SOME_PATTEN) {
		DEBUG_INFO("RISC-V: autoexecdata feature supported\n");
		dmi->support_autoexecdata = true;

		ABSTRACTAUTO_SET_DATA(abstractauto, 0);
		if (rvdbg_dmi_write(dmi, DMI_REG_ABSTRACT_AUTOEXEC, abstractauto) < 0)
			return -1;
	}

	return 0;
}

static bool rvdbg_check_error(target *t) {
	RVDBGv013_DMI_t *dmi = t->priv;

	bool res = dmi->error;
	dmi->error = false;
	return res;
}

static bool rvdbg_attach(target *t) {
	DEBUG_TARGET("Attach\n");
	/* Clear any pending fault condition */
	rvdbg_check_error(t);
	rvdbg_halt_request(t);
	// TODO: Implement
	return true;
}

static void rvdbg_detach(target *t) {
	RVDBGv013_DMI_t *dmi = t->priv;

	// Deactivate the debug module
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, 0) < 0)
		dmi->error = true;
}

static void rvdbg_reset(target *t)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	/* Try Hartreset first*/
	res = rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HARTRESET);
	if (res)
		DEBUG_WARN("Reset write HARTRESET failed\n");
	uint32_t dmcontrol = 0;
	res = rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol);
	if (res)
		DEBUG_WARN("Reset read dmcontrol failed\n");
	if (!(dmcontrol & DMCONTROL_HARTRESET)) {
		DEBUG_WARN("Optional HARTRESET not implemented, using NDMRESET\n");
		res = rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_NDMRESET);
		if (res)
			DEBUG_WARN("Reset write NDMRESET failed\n");
	}
	res = rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE);
	if (res)
		DEBUG_WARN("Reset release RESET failed\n");
}

static void rvdbg_halt_resume(target *t, bool step)
{
	RVDBGv013_DMI_t *dmi = t->priv;
	int res;
	/* Handle single step in DCSR*/
	uint32_t dcsr;
	res = rvdbg_read_single_reg(dmi, HART_REG_CSR_DCSR, &dcsr, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_WARN("Read DCSR failed\n");
	} else {
		DEBUG_TARGET("DCSR start 0x%08" PRIx32 "\n", dcsr);
		if (step)
			dcsr |= CSR_DCSR_STEP;
		else
			dcsr &= ~CSR_DCSR_STEP;
		res = rvdbg_write_single_reg(dmi, HART_REG_CSR_DCSR, dcsr, AUTOEXEC_STATE_NONE);
		if (res)
			DEBUG_WARN("Write DCSR failed\n");
	}
	uint32_t dmstatus;
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE| DMCONTROL_RESUMEREQ) < 0) {
		DEBUG_WARN("Can not write resumereq\n");
		dmi->error = true;
	}
	dmstatus = 0;
	platform_timeout timeout;
	platform_timeout_set(&timeout, 1050); /* Hart should resume in less than 1 sec*/
	while (!DMSTATUS_GET_ALLRESEUMSET(dmstatus)) {
		if (rvdbg_dmi_read(dmi, DMI_REG_DMSTATUS, &dmstatus) < 0) {
			DEBUG_WARN("Can not read dmstatua\n");
			dmi->error = true;
		}
		if (platform_timeout_is_expired(&timeout)) {
			DEBUG_WARN("Timeout waiting for resume, dmstatus 0x%08" PRIx32 "\n", dmstatus);
			dmi->error = true;
		}
	}
}

static enum target_halt_reason rvdbg_halt_poll(target *t, target_addr *watch)
{
	(void)watch;
	RVDBGv013_DMI_t *dmi = t->priv;
	int res = rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE);
	if (res)
		DEBUG_WARN("Poll write dmcontrol failed\n");
	uint32_t dmstatus;
	res = rvdbg_dmi_read(dmi, DMI_REG_DMSTATUS, &dmstatus);
	if (res)
		DEBUG_WARN("POLL read dmstatus failed\n");
	if (! DMSTATUS_GET_ALLHALTED(dmstatus))
		return TARGET_HALT_RUNNING;

	uint32_t dcsr;
	res = rvdbg_read_single_reg(dmi, HART_REG_CSR_DCSR, &dcsr, AUTOEXEC_STATE_NONE);
	uint8_t cause = (dcsr >> 6) & 7;
	DEBUG_INFO("cause = %d\n", cause);
	if (cause == 0)
		return TARGET_HALT_RUNNING;
	switch (cause) {
	case 1: /* Software breakpoint */
	case 2: /* Hardware trigger breakpoint */
		return TARGET_HALT_BREAKPOINT;
	case 3: return TARGET_HALT_REQUEST;
	case 4: return TARGET_HALT_STEPPING;
	case 5: return TARGET_HALT_REQUEST;
	default:
		return TARGET_HALT_ERROR;
	}
}

int rvdbg_dmi_init(RVDBGv013_DMI_t *dmi)
{
	uint8_t version;
	uint32_t dmstatus, nextdmaddr, dmcontrol;
	target *t;

    DEBUG_INFO("  debug version = %s\n  abits = %d\n  idle = ",
		rvdbg_version_tostr(dmi->debug_version), dmi->abits);

	dmi->error = false;

	switch (dmi->idle) {
		case 0:
			DEBUG_INFO("no run/test state\n");
			break;
		case 1:
			DEBUG_INFO("leave run/test immediately\n");
			break;
		default:
			DEBUG_INFO("stay %d cycles in run/test\n", dmi->idle - 1);
			break;
	}

	dmi->rvdbg_dmi_reset(dmi, false);

	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, 0) < 0)
			return -1;
	do {
		rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol);
	} while (dmcontrol & DMCONTROL_DMACTIVE);

	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, DMCONTROL_DMACTIVE) < 0)
			return -1;
	// Read dmcontrol and store for reference
	if (rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol) < 0)
		return -1;
	DEBUG_INFO("dmactive = %d\n", !!(dmcontrol & DMCONTROL_DMACTIVE));

	// Activate when not already activated
	if (!(dmcontrol & DMCONTROL_DMACTIVE)) {
		DEBUG_INFO("RISC-V: dmactive disabled, enabling...\n");
		dmcontrol |= DMCONTROL_DMACTIVE;
		if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, dmcontrol) < 0)
			return -1;
	}

	if (rvdbg_dmi_read(dmi, DMI_REG_DMSTATUS, &dmstatus) < 0)
		return -1;

	DEBUG_INFO("dmstatus = 0x%08x\n", dmstatus);

	version = DMSTATUS_GET_VERSION(dmstatus);
	if (version == 0) {
		DEBUG_WARN("No debug module present\n");
	} else if ((uint8_t)(version - 1) != dmi->debug_version) {
		DEBUG_WARN("dtmcs and dmstatus debug version mismatch\n");
		// Trust the dmstatus register. Ignore error, and leave
		// previous version active
		// ----------------------------------------------------
		if (version != (uint8_t)RISCV_DEBUG_VERSION_UNKNOWN)
			rvdbg_set_debug_version(dmi, version - 1);
	}

	// TODO: Implement authentification plugins
	if (!DMSTATUS_GET_AUTHENTICATED(dmstatus)) {
		// Not authentificated -> not supported
		DEBUG_WARN("RISC-V DM requires authentification!\n");
		return -1;
	}

	if (DMSTATUS_GET_CONFSTRPTRVALID(dmstatus)) {
		DEBUG_INFO("RISC-V configuration string available\n");
	}

	dmi->support_resethaltreq = DMSTATUS_GET_HASRESETHALTREQ(dmstatus);
	if (dmi->support_resethaltreq) {
		DEBUG_INFO("Supports set/clear-resethaltreq\n");
	}

	if (rvdbg_dmi_read(dmi, DMI_REG_NEXTDM_ADDR, &nextdmaddr) < 0)
		return -1;
	if (nextdmaddr) {
		// Multiple DM per DMI not yet supported
		DEBUG_WARN("Warning: Detected multiple RISC-V debug modules, only one supported!\n");
	}

	// Get impebreak before selecting mem and csr access impl
	dmi->impebreak = DMSTATUS_GET_IMPEBREAK(dmstatus);

	if (rvdbg_select_mem_and_csr_access_impl(dmi) < 0) {
		DEBUG_WARN("RISC-V: no compatible MEM / CSR access implementation detected.\n");
		return -1;
	}

    // Discover harts, add targets
	if (rvdbg_discover_harts(dmi) < 0)
		return -1;
	if (rvdbg_dmi_read(dmi, DMI_REG_DMCONTROL, &dmcontrol) < 0)
		return -1;
	/* Start to fill out target */
	t = target_new();

	rvdbd_dmi_ref(dmi);

	t->priv = dmi;
	t->priv_free = (void (*)(void *))rvdbd_dmi_unref;
	t->driver = dmi->descr;
	t->core = "Generic RVDBG 0.13";
	/* Register access functions */
	t->regs_size = 33 * 4;
	t->regs_read = rvdbg_regs_read;
	t->regs_write = rvdbg_regs_write;
	t->reg_read = rvdbg_reg_read;
	t->reg_write = rvdbg_reg_write;

	t->tdesc = tdesc_rv32;
	/*Halt/resume functions */
	t->reset = rvdbg_reset;
	t->halt_request = rvdbg_halt_request;
	t->halt_resume = rvdbg_halt_resume;
	t->halt_poll = rvdbg_halt_poll;

	t->attach = rvdbg_attach;
	t->detach = rvdbg_detach;
	t->check_error = rvdbg_check_error;
	/* We need to halt the core to poke around */
	int res;
	res = rvdbg_halt_current_hart(dmi);
	if (res)
		DEBUG_WARN("Halt failed\n");

	uint32_t misa[1];
	res = rvdbg_read_single_reg(dmi, HART_REG_CSR_MISA, misa, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_WARN("Read MISA failed\n");
	} else {
		DEBUG_INFO("MISA %"PRIx32 ", XLEN %d bits\n",  misa[0], ((misa[0] >> 30) << 5));
	}

	uint32_t machine[4];
	res = rvdbg_read_regs(dmi, HART_REG_CSR_MACHINE, machine, 4);
	if (res) {
		DEBUG_WARN("Read machine failed\n");
	} else {
		DEBUG_INFO("Machine %"PRIx32 ", %"PRIx32 ", %"PRIx32 ", %"PRIx32 "\n",
				   machine[0], machine[1], machine[2], machine[3]);
		switch (machine[0]) {
		case 0x612:
			t->mem_read = rvdbg_mem_read_systembus;
			t->driver = "ESP32-C3";
			break;
		case 0x31e:
			t->mem_read = rvdbg_mem_read_abstract;
			t->driver = "GD32VF103";
			break;
		default:
			DEBUG_WARN("Unhandled device\n");
			rvdbd_dmi_unref(dmi);
			free(t);
			return -1;
		}
	}
	uint32_t dcsr[1];
	res = rvdbg_read_single_reg(dmi, HART_REG_CSR_DCSR, dcsr, AUTOEXEC_STATE_NONE);
	if (res) {
		DEBUG_WARN("Read DCSR failed\n");
	} else {
		DEBUG_WARN("DCSR 0x%08" PRIx32 "\n", dcsr[0]);
	}
#if 0
	/* Try to read memory*/
	uint32_t sbcs;
	res = rvdbg_dmi_read(dmi, DMI_REG_SYSBUSCS, &sbcs);
	if (res) {
		DEBUG_WARN("READ SBSC failed\n");
		return -1;
	} else {
		if (sbcs)
			DEBUG_WARN("SCS: %" PRIx32 ", sbasize %d, sbaccess %d\n", sbcs, (sbcs >>5) & 0x3f,  8 << ((sbcs >>17) & 7));
		else
			DEBUG_WARN("No system bus access\n");
	}
	uint32_t addr = (sbcs) ? 0x420caec0 : 0x08000fc0;
	addr++;
	uint8_t mem8[1];
	t->mem_read(t, mem8, addr, sizeof(mem8));
	if (dmi->error) {
		DEBUG_WARN("Read MEM unaligned 1 byte failed\n");
	} else {
		DEBUG_INFO("MEM @ 0x%08" PRIx32 ": %02x\n", addr, mem8[0]);
	}
	addr--;

	uint32_t mem32[4] ={0};
	t->mem_read(t, mem32, addr, 4);
	if (dmi->error) {
		DEBUG_WARN("Read MEM aligned 1 word failed\n");
	} else {
		DEBUG_INFO("MEM @ 0x%08" PRIx32 ": %08x\n", addr, mem32[0]);
	}
	DEBUG_WARN("read 16 Bytes\n");
	t->mem_read(t, mem32, addr, 16);
	if (dmi->error) {
		DEBUG_WARN("Read MEM aligned 4 word failed\n");
	} else {
		DEBUG_INFO("MEM @ 0x%08" PRIx32 ": %08x %08x %08x %08x\n", addr,
				   mem32[0], mem32[1], mem32[2], mem32[3]);
	}
	/* dump registers */
	uint32_t regs[t->regs_size / 4];
	t->regs_read(t, regs);
	for (size_t i = 0; i < t->regs_size; i = i+4) {
		DEBUG_WARN("reg %2d: 0x%08x\n", i/4, regs[i/4]);
	}
	DEBUG_TARGET("Resume single step\n");
	rvdbg_halt_resume(t, true);
	DEBUG_TARGET("Poll\n");
	res = rvdbg_halt_poll(t, NULL);
	DEBUG_WARN("Poll res %d\n", res);
#endif
	rvdbg_halt_resume(t, false);
	// Disable the debug module
	if (rvdbg_dmi_write(dmi, DMI_REG_DMCONTROL, 0) < 0)
		return -1;


	return 0;
}
