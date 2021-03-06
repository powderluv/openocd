/***************************************************************************
 *   Copyright (C) 2013 Synapse Product Development                        *
 *   Andrey Smirnov <andrew.smironv@gmail.com>                             *
 *   Angus Gratton <gus@projectgus.com>                                    *
 *   Erdem U. Altunyurt <spamjunkeater@gmail.com>                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "imp.h"
#include <target/algorithm.h>
#include <target/armv7m.h>
#include <helper/types.h>

/* nRF52 Register addresses used by openOCD. */
#define NRF52_FLASH_BASE_ADDR        (0x0)

#define NRF52_FICR_BASE_ADDR         (0x10000000)
#define NRF52_FICR_CODEPAGESIZE_ADDR (NRF52_FICR_BASE_ADDR | 0x010)
#define NRF52_FICR_CODESIZE_ADDR     (NRF52_FICR_BASE_ADDR | 0x014)

#define NRF52_UICR_BASE_ADDR         (0x10001000)

#define NRF52_NVMC_BASE_ADDR         (0x4001E000)
#define NRF52_NVMC_READY_ADDR        (NRF52_NVMC_BASE_ADDR | 0x400)
#define NRF52_NVMC_CONFIG_ADDR       (NRF52_NVMC_BASE_ADDR | 0x504)
#define NRF52_NVMC_ERASEPAGE_ADDR    (NRF52_NVMC_BASE_ADDR | 0x508)
#define NRF52_NVMC_ERASEALL_ADDR     (NRF52_NVMC_BASE_ADDR | 0x50C)
#define NRF52_NVMC_ERASEUICR_ADDR    (NRF52_NVMC_BASE_ADDR | 0x514)

/* nRF52 bit fields. */
enum nrf52_nvmc_config_bits {
	NRF52_NVMC_CONFIG_REN = 0x0,
	NRF52_NVMC_CONFIG_WEN = 0x01,
	NRF52_NVMC_CONFIG_EEN = 0x02
};

enum nrf52_nvmc_ready_bits {
	NRF52_NVMC_BUSY  = 0x0,
	NRF52_NVMC_READY = 0x01
};

/* nRF52 state information. */
struct nrf52_info {
	uint32_t code_page_size; /* Size of FLASH page in bytes. */
	uint32_t code_memory_size; /* Size of Code FLASH region in bytes. */

	struct {
		bool probed;
		int (*write) (struct flash_bank *bank,
				struct nrf52_info *chip,
				const uint8_t *buffer, uint32_t offset, uint32_t count);
	} bank[2]; /* There are two regions in nRF52 FLASH - Code and UICR. */
	struct target *target;
};

static int nrf52_protect_check(struct flash_bank *bank);

static int nrf52_probe(struct flash_bank *bank)
{
	int res;
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	res = target_read_u32(chip->target,
						NRF52_FICR_CODEPAGESIZE_ADDR,
						&chip->code_page_size);
	if (res != ERROR_OK) {
		LOG_ERROR("Couldn't read code page size");
		return res;
	}

	res = target_read_u32(chip->target,
						NRF52_FICR_CODESIZE_ADDR,
						&chip->code_memory_size);
	if (res != ERROR_OK) {
		LOG_ERROR("Couldn't read code memory size");
		return res;
	}

	chip->code_memory_size = chip->code_memory_size * chip->code_page_size;

	if (bank->base == NRF52_FLASH_BASE_ADDR) {
		bank->size = chip->code_memory_size;
		bank->num_sectors = bank->size / chip->code_page_size;
		bank->sectors = calloc(bank->num_sectors,
							sizeof((bank->sectors)[0]));
		if (!bank->sectors)
			return ERROR_FLASH_BANK_NOT_PROBED;

		/* Fill out the sector information: All nRF51 sectors are the same size. */
		for (int i = 0; i < bank->num_sectors; i++) {
			bank->sectors[i].size = chip->code_page_size;
			bank->sectors[i].offset	= i * chip->code_page_size;

			/* Mark as unknown. */
			bank->sectors[i].is_erased = -1;
			bank->sectors[i].is_protected = -1;
		}

		nrf52_protect_check(bank);

		chip->bank[0].probed = true;
	} else { /* This is the UICR bank. */
		bank->size = chip->code_page_size;
		bank->num_sectors = 1;
		bank->sectors = calloc(bank->num_sectors,
							sizeof((bank->sectors)[0]));
		if (!bank->sectors)
			return ERROR_FLASH_BANK_NOT_PROBED;

		bank->sectors[0].size = bank->size;
		bank->sectors[0].offset	= 0;

		bank->sectors[0].is_erased = -1;
		bank->sectors[0].is_protected = -1;

		chip->bank[1].probed = true;
	}

	return ERROR_OK;
}

static int nrf52_bank_is_probed(struct flash_bank *bank)
{
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	return chip->bank[bank->bank_number].probed;
}

static int nrf52_auto_probe(struct flash_bank *bank)
{
	if (!nrf52_bank_is_probed(bank))
		return nrf52_probe(bank);
	else
		return ERROR_OK;
}

static int nrf52_wait_for_nvmc(struct nrf52_info *chip)
{
	int res;
	uint32_t ready;
	int timeout = 100;

	do {
		res = target_read_u32(chip->target, NRF52_NVMC_READY_ADDR, &ready);
		if (res != ERROR_OK) {
			LOG_ERROR("Couldn't read NVMC_READY register");
			return res;
		}

		if (ready == NRF52_NVMC_READY)
			return ERROR_OK;

		alive_sleep(1);
	} while (timeout--);

	LOG_DEBUG("Timed out waiting for the NVMC to be ready");
	return ERROR_FLASH_BUSY;
}

static int nrf52_nvmc_erase_enable(struct nrf52_info *chip)
{
	int res;

	res = nrf52_wait_for_nvmc(chip);
	if (res != ERROR_OK)
		return res;

	res = target_write_u32(chip->target,
						NRF52_NVMC_CONFIG_ADDR,
						NRF52_NVMC_CONFIG_EEN);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to configure the NVMC for erasing");
		return res;
	}

	return res;
}

static int nrf52_nvmc_write_enable(struct nrf52_info *chip)
{
	int res;

	res = nrf52_wait_for_nvmc(chip);
	if (res != ERROR_OK)
		return res;

	res = target_write_u32(chip->target,
						NRF52_NVMC_CONFIG_ADDR,
						NRF52_NVMC_CONFIG_WEN);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to configure the NVMC for writing");
		return res;
	}

	return res;
}

static int nrf52_nvmc_read_only(struct nrf52_info *chip)
{
	int res;

	res = nrf52_wait_for_nvmc(chip);
	if (res != ERROR_OK)
		return res;

	res = target_write_u32(chip->target,
						NRF52_NVMC_CONFIG_ADDR,
						NRF52_NVMC_CONFIG_REN);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to configure the NVMC for read-only");
		return res;
	}

	return res;
}

static int nrf52_nvmc_generic_erase(struct nrf52_info *chip,
								uint32_t erase_register,
								uint32_t erase_value)
{
	int res;

	res = nrf52_nvmc_erase_enable(chip);
	if (res != ERROR_OK)
		return res;

	res = target_write_u32(chip->target,
						erase_register,
						erase_value);
	if (res != ERROR_OK)
		LOG_ERROR("Failed to write NVMC erase register");

	return nrf52_nvmc_read_only(chip);
}

static int nrf52_protect_check(struct flash_bank *bank)
{
	LOG_WARNING("nrf52_protect_check() is not implemented for nRF52 series devices yet");
	return ERROR_OK;
}

static int nrf52_protect(struct flash_bank *bank, int set, int first, int last)
{
	LOG_WARNING("nrf52_protect() is not implemented for nRF52 series devices yet");
	return ERROR_OK;
}

static struct flash_sector *nrf52_find_sector_by_address(struct flash_bank *bank, uint32_t address)
{
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	for (int i = 0; i < bank->num_sectors; i++)
		if (bank->sectors[i].offset <= address &&
			address < (bank->sectors[i].offset + chip->code_page_size)) {
			return &bank->sectors[i];
		}

	return NULL;
}

static int nrf52_erase_all(struct nrf52_info *chip)
{
	LOG_DEBUG("Erasing all non-volatile memory");
	return nrf52_nvmc_generic_erase(chip,
								NRF52_NVMC_ERASEALL_ADDR,
								0x01);
}

static int nrf52_erase_page(struct flash_bank *bank,
							struct nrf52_info *chip,
							struct flash_sector *sector)
{
	int res;

	LOG_DEBUG("Erasing page at 0x%"PRIx32, sector->offset);
	if (sector->is_protected == 1) {
		LOG_ERROR("Cannot erase protected sector at 0x%" PRIx32, sector->offset);
		return ERROR_FAIL;
	}

	if (bank->base == NRF52_UICR_BASE_ADDR) {
		res = nrf52_nvmc_generic_erase(chip,
									NRF52_NVMC_ERASEUICR_ADDR,
									0x00000001);
	} else {
		res = nrf52_nvmc_generic_erase(chip,
									NRF52_NVMC_ERASEPAGE_ADDR,
									sector->offset);
	}

	if (res == ERROR_OK)
		sector->is_erased = 1;
	return res;
}

static const uint8_t nrf52_flash_write_code[] = {
	/* See contrib/loaders/flash/cortex-m0.S */
	/* <wait_fifo>: */
	0x0d, 0x68,		/* ldr	r5,	[r1,	#0] */
	0x00, 0x2d,		/* cmp	r5,	#0 */
	0x0b, 0xd0,		/* beq.n	1e <exit> */
	0x4c, 0x68,		/* ldr	r4,	[r1,	#4] */
	0xac, 0x42,		/* cmp	r4,	r5 */
	0xf9, 0xd0,		/* beq.n	0 <wait_fifo> */
	0x20, 0xcc,		/* ldmia	r4!,	{r5} */
	0x20, 0xc3,		/* stmia	r3!,	{r5} */
	0x94, 0x42,		/* cmp	r4,	r2 */
	0x01, 0xd3,		/* bcc.n	18 <no_wrap> */
	0x0c, 0x46,		/* mov	r4,	r1 */
	0x08, 0x34,		/* adds	r4,	#8 */
	/* <no_wrap>: */
	0x4c, 0x60,		/* str	r4, [r1,	#4] */
	0x04, 0x38,		/* subs	r0, #4 */
	0xf0, 0xd1,		/* bne.n	0 <wait_fifo> */
	/* <exit>: */
	0x00, 0xbe		/* bkpt	0x0000 */
};


/* Start a low level flash write for the specified region */
static int nrf52_ll_flash_write(struct nrf52_info *chip, uint32_t offset, const uint8_t *buffer, uint32_t bytes)
{
	struct target *target = chip->target;
	uint32_t buffer_size = 8192;
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = NRF52_FLASH_BASE_ADDR + offset;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;

	LOG_DEBUG("Writing buffer to flash offset=0x%"PRIx32" bytes=0x%"PRIx32, offset, bytes);
	assert(bytes % 4 == 0);

	/* allocate working area with flash programming code */
	if (target_alloc_working_area(target, sizeof(nrf52_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, falling back to slow memory writes");

		for (; bytes > 0; bytes -= 4) {
			retval = target_write_memory(chip->target,
										offset, 4, 1, buffer);
			if (retval != ERROR_OK)
				return retval;

			retval = nrf52_wait_for_nvmc(chip);
			if (retval != ERROR_OK)
				return retval;

			offset += 4;
			buffer += 4;
		}

		return ERROR_OK;
	}

	LOG_WARNING("using fast async flash loader. This is currently supported");
	LOG_WARNING("only with ST-Link and CMSIS-DAP. If you have issues, add");
	LOG_WARNING("\"set WORKAREASIZE 0\" before sourcing nrf52.cfg to disable it");

	retval = target_write_buffer(target, write_algorithm->address,
				sizeof(nrf52_flash_write_code),
				nrf52_flash_write_code);
	if (retval != ERROR_OK)
		return retval;

	/* memory buffer */
	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~3UL; /* Make sure it's 4 byte aligned */
		if (buffer_size <= 256) {
			/* free working area, write algorithm already allocated */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("No large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* byte count */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* buffer start */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* buffer end */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_IN_OUT);	/* target address */

	buf_set_u32(reg_params[0].value, 0, 32, bytes);
	buf_set_u32(reg_params[1].value, 0, 32, source->address);
	buf_set_u32(reg_params[2].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[3].value, 0, 32, address);

	retval = target_run_flash_async_algorithm(target, buffer, bytes/4, 4,
			0, NULL,
			4, reg_params,
			source->address, source->size,
			write_algorithm->address, 0,
			&armv7m_info);

	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	return retval;
}

/* Check and erase flash sectors in specified range, then start a low level page write.
   start/end must be sector aligned.
*/
static int nrf52_write_pages(struct flash_bank *bank, uint32_t start, uint32_t end, const uint8_t *buffer)
{
	int res;
	uint32_t offset;
	struct flash_sector *sector;
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	assert(start % chip->code_page_size == 0);
	assert(end % chip->code_page_size == 0);

	/* Erase all sectors */
	for (offset = start; offset < end; offset += chip->code_page_size) {
		sector = nrf52_find_sector_by_address(bank, offset);

		if (sector == NULL) {
			LOG_ERROR("Invalid sector @ 0x%08"PRIx32, offset);
			return ERROR_FLASH_SECTOR_INVALID;
		}

		if (sector->is_protected == 1) {
			LOG_ERROR("Can't erase protected sector @ 0x%08"PRIx32, offset);
			return ERROR_FAIL;
		}

		if (sector->is_erased != 1) {	/* 1 = erased, 0= not erased, -1 = unknown */
			res = nrf52_erase_page(bank, chip, sector);
			if (res != ERROR_OK) {
				LOG_ERROR("Failed to erase sector @ 0x%08"PRIx32, sector->offset);
				return res;
			}
		}
		sector->is_erased = 1;
	}

	res = nrf52_nvmc_write_enable(chip);
	if (res != ERROR_OK)
		return res;

	res = nrf52_ll_flash_write(chip, start, buffer, (end - start));
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to write FLASH");
		nrf52_nvmc_read_only(chip);
		return res;
	}

	return nrf52_nvmc_read_only(chip);
}

static int nrf52_erase(struct flash_bank *bank, int first, int last)
{
	int res = ERROR_OK;
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	/* For each sector to be erased */
	for (int s = first; s <= last && res == ERROR_OK; s++)
		res = nrf52_erase_page(bank, chip, &bank->sectors[s]);

	return res;
}

static int nrf52_code_flash_write(struct flash_bank *bank,
								struct nrf52_info *chip,
								const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int res;
	/* Need to perform reads to fill any gaps we need to preserve in the first page,
	   before the start of buffer, or in the last page, after the end of buffer */
	uint32_t first_page = offset / chip->code_page_size;
	uint32_t last_page = DIV_ROUND_UP(offset+count, chip->code_page_size);

	uint32_t first_page_offset = first_page * chip->code_page_size;
	uint32_t last_page_offset = last_page * chip->code_page_size;

	LOG_DEBUG("Padding write from 0x%08"PRIx32"-0x%08"PRIx32" as 0x%08"PRIx32"-0x%08"PRIx32,
			offset, offset+count, first_page_offset, last_page_offset);

	uint32_t page_cnt = last_page - first_page;
	uint8_t buffer_to_flash[page_cnt * chip->code_page_size];

	/* Fill in any space between start of first page and start of buffer */
	uint32_t pre = offset - first_page_offset;
	if (pre > 0) {
		res = target_read_memory(bank->target, first_page_offset, 1, pre, buffer_to_flash);
		if (res != ERROR_OK)
			return res;
	}

	/* Fill in main contents of buffer */
	memcpy(buffer_to_flash + pre, buffer, count);

	/* Fill in any space between end of buffer and end of last page */
	uint32_t post = last_page_offset - (offset + count);
	if (post > 0) {
		/* Retrieve the full row contents from Flash */
		res = target_read_memory(bank->target, offset + count, 1, post, buffer_to_flash + pre + count);
		if (res != ERROR_OK)
			return res;
	}

	return nrf52_write_pages(bank, first_page_offset, last_page_offset, buffer_to_flash);
}

static int nrf52_uicr_flash_write(struct flash_bank *bank,
								struct nrf52_info *chip,
								const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int res;
	uint32_t nrf52_uicr_size = chip->code_page_size;
	uint8_t uicr[nrf52_uicr_size];
	struct flash_sector *sector = &bank->sectors[0];

	if ((offset + count) > nrf52_uicr_size)
		return ERROR_FAIL;

	res = target_read_memory(bank->target, NRF52_UICR_BASE_ADDR, 1, nrf52_uicr_size, uicr);

	if (res != ERROR_OK)
		return res;

	if (sector->is_erased != 1) {
		res = nrf52_erase_page(bank, chip, sector);
		if (res != ERROR_OK)
			return res;
	}

	memcpy(&uicr[offset], buffer, count);

	res = nrf52_nvmc_write_enable(chip);
	if (res != ERROR_OK)
		return res;

	res = nrf52_ll_flash_write(chip, NRF52_UICR_BASE_ADDR, uicr, nrf52_uicr_size);
	if (res != ERROR_OK) {
		nrf52_nvmc_read_only(chip);
		return res;
	}

	return nrf52_nvmc_read_only(chip);
}


static int nrf52_write(struct flash_bank *bank, const uint8_t *buffer,
					uint32_t offset, uint32_t count)
{
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	return chip->bank[bank->bank_number].write(bank, chip, buffer, offset, count);
}


FLASH_BANK_COMMAND_HANDLER(nrf52_flash_bank_command)
{
	static struct nrf52_info *chip;

	assert(bank != NULL);

	switch (bank->base) {
	case NRF52_FLASH_BASE_ADDR:
		bank->bank_number = 0;
		break;
	case NRF52_UICR_BASE_ADDR:
		bank->bank_number = 1;
		break;
	default:
		LOG_ERROR("Invalid bank address 0x%08" PRIx32, bank->base);
		return ERROR_FAIL;
	}

	if (!chip) {
		/* Create a new chip */
		chip = calloc(1, sizeof(*chip));
		assert(chip != NULL);

		chip->target = bank->target;
	}

	switch (bank->base) {
	case NRF52_FLASH_BASE_ADDR:
		chip->bank[bank->bank_number].write = nrf52_code_flash_write;
		break;
	case NRF52_UICR_BASE_ADDR:
		chip->bank[bank->bank_number].write = nrf52_uicr_flash_write;
		break;
	}

	chip->bank[bank->bank_number].probed = false;
	bank->driver_priv = chip;

	return ERROR_OK;
}

COMMAND_HANDLER(nrf52_handle_mass_erase_command)
{
	int res;
	struct flash_bank *bank = NULL;
	struct target *target = get_current_target(CMD_CTX);

	res = get_flash_bank_by_addr(target, NRF52_FLASH_BASE_ADDR, true, &bank);
	if (res != ERROR_OK)
		return res;

	assert(bank != NULL);

	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	res = nrf52_erase_all(chip);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to erase the chip");
		nrf52_protect_check(bank);
		return res;
	}

	for (int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_erased = 1;

	res = nrf52_protect_check(bank);
	if (res != ERROR_OK) {
		LOG_ERROR("Failed to check chip's write protection");
		return res;
	}

	res = get_flash_bank_by_addr(target, NRF52_UICR_BASE_ADDR, true, &bank);
	if (res != ERROR_OK)
		return res;

	bank->sectors[0].is_erased = 1;

	return ERROR_OK;
}

static int nrf52_info(struct flash_bank *bank, char *buf, int buf_size)
{
	int res;
	uint32_t ficr[2];
	struct nrf52_info *chip = bank->driver_priv;
	assert(chip != NULL);

	res = target_read_u32(chip->target, NRF52_FICR_CODEPAGESIZE_ADDR, &ficr[0]);
		if (res != ERROR_OK) {
			LOG_ERROR("Couldn't read NVMC_READY register");
			return res;
		}

	res = target_read_u32(chip->target, NRF52_FICR_CODESIZE_ADDR, &ficr[1]);
		if (res != ERROR_OK) {
			LOG_ERROR("Couldn't read NVMC_READY register");
			return res;
		}

	snprintf(buf, buf_size,
			"\n--------nRF52 Series Device--------\n\n"
			"\n[factory information control block]\n"
			"code page size: %"PRIu32"B\n"
			"code memory size: %"PRIu32"kB\n",
			ficr[0],
			(ficr[1] * ficr[0]) / 1024);

	return ERROR_OK;
}

static const struct command_registration nrf52_exec_command_handlers[] = {
	{
		.name		= "mass_erase",
		.handler	= nrf52_handle_mass_erase_command,
		.mode		= COMMAND_EXEC,
		.help		= "Erase all flash contents of the chip.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration nrf52_command_handlers[] = {
	{
		.name	= "nrf52",
		.mode	= COMMAND_ANY,
		.help	= "nrf52 flash command group",
		.usage	= "",
		.chain	= nrf52_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver nrf52_flash = {
	.name			= "nrf52",
	.commands		= nrf52_command_handlers,
	.flash_bank_command	= nrf52_flash_bank_command,
	.info			= nrf52_info,
	.erase			= nrf52_erase,
	.protect		= nrf52_protect,
	.write			= nrf52_write,
	.read			= default_flash_read,
	.probe			= nrf52_probe,
	.auto_probe		= nrf52_auto_probe,
	.erase_check	= default_flash_blank_check,
	.protect_check	= nrf52_protect_check,
};
