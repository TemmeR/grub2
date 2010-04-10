/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2004,2005,2007,2008  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GRUB_INIT_MACHINE_HEADER
#define GRUB_INIT_MACHINE_HEADER	1

#include <grub/types.h>
#include <grub/symbol.h>
#include <grub/machine/memory.h>

/* Get a memory map entry. Return next continuation value. Zero means
   the end.  */
grub_uint32_t grub_get_mmap_entry (struct grub_machine_mmap_entry *entry,
				   grub_uint32_t cont);

/* Turn on/off Gate A20.  */
void grub_gate_a20 (int on);

void EXPORT_FUNC(grub_stop_floppy) (void);

#endif /* ! GRUB_INIT_MACHINE_HEADER */
