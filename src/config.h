/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2011  Ingo Korb <ingo@akana.de>

   Inspiration and low-level SD/MMC access based on code from MMC2IEC
     by Lars Pontoppidan et al., see sdcard.c|h and config.h.

   FAT filesystem access based on code from ChaN and Jim Brain, see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


   config.h: The main configuration header file which actually doesn't have
             any configuration options in it anymore.

*/

#ifndef CONFIG_H
#define CONFIG_H

#include "autoconf.h"
#include "arch-config.h"

/* Disable COMMAND_CHANNEL_DUMP if UART_DEBUG is disabled */
#ifndef CONFIG_UART_DEBUG
#  undef CONFIG_COMMAND_CHANNEL_DUMP
#endif

/* An interrupt for detecting card changes implies hotplugging capability */
#if defined(SD_CHANGE_HANDLER) || defined (CF_CHANGE_HANDLER)
#  define HAVE_HOTPLUG
#endif

/* Generate a dummy function for the Power-LED if required */
#ifndef HAVE_POWER_LED
static inline void set_power_led(uint8_t state) {
  return;
}
#endif

/* ----- Translate CONFIG_ADD symbols to HAVE symbols ----- */
/* By using two symbols for this purpose it's easier to determine if */
/* support was enabled by default or added in the config file.       */
#if defined(CONFIG_ADD_SD) && !defined(HAVE_SD)
#  define HAVE_SD
#endif

#if defined(CONFIG_ADD_ATA) && !defined(HAVE_ATA)
#  define HAVE_ATA
#endif

/* Create some temporary symbols so we can calculate the number of */
/* enabled storage devices.                                        */
#ifdef HAVE_SD
#  define TMP_SD 1
#endif
#ifdef HAVE_ATA
#  define TMP_ATA 1
#endif

/* Enable the diskmux if more than one storage device is enabled. */
#if !defined(NEED_DISKMUX) && (TMP_SD + TMP_ATA) > 1
#  define NEED_DISKMUX
#endif

/* Remove the temporary symbols */
#undef TMP_SD
#undef TMP_ATA

/* Hardcoded maximum - reducing this won't save any ram */
#define MAX_DRIVES 8

/* SD access LED dummy */
#ifndef HAVE_SD_LED
# define set_sd_led(x) do {} while (0)
#endif

/* Sanity check */
#if defined(CONFIG_LOADER_WHEELS) && !defined(CONFIG_LOADER_GEOS)
#  error "CONFIG_LOADER_GEOS must be enabled for Wheels support!"
#endif

#endif