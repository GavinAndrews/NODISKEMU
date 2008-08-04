/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007,2008  Ingo Korb <ingo@akana.de>

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


   iec.c: IEC handling code, stateful version

   This code is a close reimplementation of the bus handling in a 1571
   to be as compatible to original drives as possible. Hex addresses in
   comments refer to the part of the 1571 rom that particular section
   is based on.

*/

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "avrcompat.h"
#include "buffers.h"
#include "diskchange.h"
#include "diskio.h"
#include "doscmd.h"
#include "errormsg.h"
#include "fastloader.h"
#include "fastloader-ll.h"
#include "fatops.h"
#include "flags.h"
#include "fileops.h"
#include "iec-ll.h"
#include "led.h"
#include "timer.h"
#include "uart.h"
#include "iec.h"

/* ------------------------------------------------------------------------- */
/*  Global variables                                                         */
/* ------------------------------------------------------------------------- */

/* Current device address */
uint8_t device_address;

/**
 * struct iecflags_t - Bitfield of various flags, mostly IEC-related
 * @eoi_recvd      : Received EOI with the last byte read
 * @command_recvd  : Command or filename received
 * @jiffy_active   : JiffyDOS-capable master detected
 * @jiffy_load     : JiffyDOS LOAD operation detected
 *
 * This is a bitfield for a number of boolean variables used 
 */

#define EOI_RECVD       (1<<0)
#define COMMAND_RECVD   (1<<1)
#define JIFFY_ACTIVE    (1<<2)
#define JIFFY_LOAD      (1<<3)

struct {
  uint8_t iecflags;
  enum { BUS_IDLE = 0, BUS_ATNACTIVE, BUS_FOUNDATN, BUS_FORME, BUS_NOTFORME, BUS_ATNFINISH, BUS_ATNPROCESS, BUS_CLEANUP, BUS_SLEEP } bus_state;
  enum { DEVICE_IDLE = 0, DEVICE_LISTEN, DEVICE_TALK } device_state;
  uint8_t secondary_address;
} iec_data;


/* ------------------------------------------------------------------------- */
/*  Very low-level bus handling                                              */
/* ------------------------------------------------------------------------- */

/// Debounce IEC input - see E9C0
static uint8_t iec_pin(void) {
  uint8_t tmp;

  do {
    tmp = IEC_PIN & (IEC_BIT_ATN | IEC_BIT_DATA | IEC_BIT_CLOCK | IEC_BIT_SRQ);
    _delay_us(2); /* 1571 uses LDA/CMP/BNE, approximate by waiting 2us */
  } while (tmp != (IEC_PIN & (IEC_BIT_ATN | IEC_BIT_DATA | IEC_BIT_CLOCK | IEC_BIT_SRQ)));
  return tmp;
}

/// Checks if ATN has changed and changes state to match (EA59)
static uint8_t check_atn(void) {
  if (iec_data.bus_state == BUS_ATNACTIVE)
    if (IEC_ATN) {
      iec_data.bus_state = BUS_ATNPROCESS; // A9AC
      return 1;
    } else
      return 0;
  else
    if (!IEC_ATN) {
      iec_data.bus_state = BUS_FOUNDATN;   // A7B3
      return 1;
    } else
      return 0;
}

#ifndef IEC_INT_VECT

/// Interrupt routine that simulates the hardware-auto-acknowledge of ATN
/* This currently runs once every 500 microseconds, keep small! */
ISR(TIMER2_COMPA_vect) {
  if (!IEC_ATN) {
    set_data(0);
  }
}

#endif

/* ------------------------------------------------------------------------- */
/*  Byte transfer routines                                                   */
/* ------------------------------------------------------------------------- */

/**
 * _iec_getc - receive one byte from the CBM serial bus (E9C9)
 *
 * This function tries receives one byte from the serial bus and returns it
 * if successful. Returns -1 instead if the device state has changed, the
 * caller should return to the main loop immediately in that case.
 */
static int16_t _iec_getc(void) {
  uint8_t i,val,tmp;

  val = 0;

  do {                                                 // E9CD-E9D5
    if (check_atn()) return -1;
  } while (!(iec_pin() & IEC_BIT_CLOCK));

  set_data(1);                                         // E9D7
  /* Wait until all other devices released the data line    */
  while (!IEC_DATA) ;                                  // FF20

  /* Timer for EOI detection */
  start_timeout(TIMEOUT_US(256));

  do {
    if (check_atn()) return -1;                        // E9DF
    tmp = has_timed_out();                             // E9EE
  } while ((iec_pin() & IEC_BIT_CLOCK) && !tmp);

  /* See if timeout happened -> EOI */
  if (tmp) {
    set_data(0);                                       // E9F2
    _delay_us(73);                      // E9F5-E9F8, delay calculated from all
    set_data(1);                        //   instructions between IO accesses

    uart_putc('E');

    do {
      if (check_atn())                                 // E9FD
        return -1;
    } while (iec_pin() & IEC_BIT_CLOCK);

    iec_data.iecflags|=EOI_RECVD;                      // EA07
  }

  for (i=0;i<8;i++) {
    /* Check for JiffyDOS                                       */
    /*   Source: http://home.arcor.de/jochen.adler/ajnjil-t.htm */
    if (iec_data.bus_state == BUS_ATNACTIVE && (globalflags & JIFFY_ENABLED) && i == 7) {
      start_timeout(TIMEOUT_US(218));

      do {
        tmp = IEC_PIN & (IEC_BIT_ATN | IEC_BIT_DATA | IEC_BIT_CLOCK | IEC_BIT_SRQ);

        /* If there is a delay before the last bit, the controller uses JiffyDOS */
        if (!(iec_data.iecflags & JIFFY_ACTIVE) && has_timed_out()) {
          if ((val>>1) < 0x60 && ((val>>1) & 0x1f) == device_address) {
            /* If it's for us, notify controller that we support Jiffy too */
            set_data(0);
            _delay_us(101); // nlq says 405us, but the code shows only 101
            set_data(1);
            iec_data.iecflags |= JIFFY_ACTIVE;
          }
        }
      } while (!(tmp & IEC_BIT_CLOCK));
    } else {
      /* Capture data on rising edge */
      do {                                             // EA0B
        tmp = IEC_PIN & (IEC_BIT_ATN | IEC_BIT_DATA | IEC_BIT_CLOCK | IEC_BIT_SRQ);
      } while (!(tmp & IEC_BIT_CLOCK));
    }

    val = (val >> 1) | (!!(tmp & IEC_BIT_DATA) << 7);  // EA18

    do {                                               // EA1A
      if (check_atn()) return -1;
    } while (iec_pin() & IEC_BIT_CLOCK);
  }

  _delay_us(5); // Test
  set_data(0);                                         // EA28
  _delay_us(50);  /* Slow down a little bit, may or may not fix some problems */
  return val;
}

/**
 * iec_getc - wrapper around _iec_getc to disable interrupts
 *
 * This function wraps iec_getc to disable interrupts there and is completely
 * inlined by the compiler. It could be inlined in the C code too, but is kept
 * seperately for clarity.
 */
static int16_t iec_getc(void) {
  int16_t val;

  cli();
  val = _iec_getc();
  sei();
  return val;
}


/**
 * iec_putc - send a byte over the serial bus (E916)
 * @data    : byte to be sent
 * @with_eoi: Flags if the byte should be send with an EOI condition
 *
 * This function sends the byte data over the serial bus, optionally including
 * a marker for the EOI condition. Returns 0 normally or -1 if the bus state has
 * changed, the caller should return to the main loop in that case.
 */
static uint8_t iec_putc(uint8_t data, const uint8_t with_eoi) {
  uint8_t i;

  if (check_atn()) return -1;                          // E916

  if (iec_data.iecflags & JIFFY_ACTIVE) {
    /* This is the non-load Jiffy case */
    if (jiffy_send(data, with_eoi, 0)) {
      check_atn();
      return -1;
    }
    return 0;
  }

  i = iec_pin();

  _delay_us(60); // Fudged delay
  set_clock(1);

  if (i & IEC_BIT_DATA) { // E923
    /* The 1571 jumps to E937 at this point, but I think            */
    /* this is not necessary - the following loop will fall through */
  }

  do {
    if (check_atn()) return -1;                        // E925
  } while (!(iec_pin() & IEC_BIT_DATA));

  if (with_eoi || (i & IEC_BIT_DATA)) {
    do {
      if (check_atn()) return -1;                      // E937
    } while (!(iec_pin() & IEC_BIT_DATA));

    do {
      if (check_atn()) return -1;                      // E941
    } while (iec_pin() & IEC_BIT_DATA);
  }

  set_clock(0);                                        // E94B
  _delay_us(60); // Yet another "looked at the bus trace and guessed until it worked" delay
  do {
    if (check_atn()) return -1;
  } while (!(iec_pin() & IEC_BIT_DATA));

  for (i=0;i<8;i++) {
    if (!(iec_pin() & IEC_BIT_DATA)) {
      iec_data.bus_state = BUS_CLEANUP;
      return -1;
    }

    set_data(data & 1<<i);
    _delay_us(70);    // Implicid delay, fudged
    set_clock(1);
    if (globalflags & VC20MODE)
      _delay_us(34);  // Calculated delay
    else
      _delay_us(69);  // Calculated delay

    set_clock(0);
    set_data(1);
    _delay_us(5);     // Settle time
  }

  do {
    if (check_atn()) return -1;
  } while (iec_pin() & IEC_BIT_DATA);

  /* More stuff that's not in the original rom:
   *   Wait for 250us or until DATA is high or ATN is low.
   * This fixes a problem with Castle Wolfenstein.
   * Bus traces seem to indicate that a real 1541 needs
   * about 350us between two bytes, sd2iec is usually WAY faster.
   */
  start_timeout(TIMEOUT_US(250));
  while (!IEC_DATA && IEC_ATN && !has_timed_out()) ;

  return 0;
}


/* ------------------------------------------------------------------------- */
/*  Listen+Talk-Handling                                                     */
/* ------------------------------------------------------------------------- */

/**
 * iec_listen_handler - handle an incoming LISTEN request (EA2E)
 * @cmd: command byte received from the bus
 *
 * This function handles a listen request from the computer.
 */
static uint8_t iec_listen_handler(const uint8_t cmd) {
  int16_t c;
  buffer_t *buf;

  uart_putc('L');

  buf = find_buffer(cmd & 0x0f);

  /* Abort if there is no buffer or it's not open for writing */
  /* and it isn't an OPEN command                             */
  if ((buf == NULL || !buf->write) && (cmd & 0xf0) != 0xf0) {
    uart_putc('c');
    iec_data.bus_state = BUS_CLEANUP;
    return 1;
  }

  while (1) {
    if (iec_data.iecflags & JIFFY_ACTIVE) {
      uint8_t flags;
      set_atn_irq(1);
      _delay_us(50); /* Slow down or we'll see garbage from the C64 */
                     /* The time was guessed from bus traces.       */
      c = jiffy_receive(&flags);
      if (!(flags & IEC_BIT_ATN))
        /* ATN was active at the end of the transfer */
        c = iec_getc();
      else
        if(flags & IEC_BIT_CLOCK)
          iec_data.iecflags |= EOI_RECVD;
        else
          iec_data.iecflags &= (uint8_t)~EOI_RECVD;
    } else
      c = iec_getc();
    if (c < 0) return 1;

    if ((cmd & 0x0f) == 0x0f || (cmd & 0xf0) == 0xf0) {
      if (command_length < CONFIG_COMMAND_BUFFER_SIZE)
        command_buffer[command_length++] = c;
      if (iec_data.iecflags & EOI_RECVD)
        // Filenames are just a special type of command =)
        iec_data.iecflags |= COMMAND_RECVD;
    } else {
      /* Flush buffer if full */
      if (buf->mustflush)
        if (buf->refill(buf))
          return 1;

      buf->data[buf->position] = c;
      buf->dirty = 1;

      if (buf->lastused < buf->position)
        buf->lastused = buf->position;
      buf->position++;

      /* Mark buffer for flushing if position wrapped */
      if (buf->position == 0)
        buf->mustflush = 1;

      /* REL files must be syncronized on EOI */
      if(buf->recordlen && (iec_data.iecflags & EOI_RECVD))
        if (buf->refill(buf))
          return 1;
    }
  }
}

/**
 * iec_talk_handler - handle an incoming TALK request (E909)
 * @cmd: command byte received from the bus
 *
 * This function handles a talk request from the computer.
 */
static uint8_t iec_talk_handler(uint8_t cmd) {
  buffer_t *buf;

  uart_putc('T');

  buf = find_buffer(cmd & 0x0f);
  if (buf == NULL)
    return 0; /* 0 because we didn't change the state here */

  if (iec_data.iecflags & JIFFY_ACTIVE)
    /* wait 360us (J1541 E781) to make sure the C64 is at fbb7/fb0c */
    _delay_ms(0.36);

  if (iec_data.iecflags & JIFFY_LOAD) {
    /* See if the C64 has passed fb06 or if we should abort */
    do {                /* J1541 FF30 - wait until DATA inactive/high */
      if (check_atn()) return -1;
    } while (!IEC_DATA);
    /* The LOAD path is only used after the first two bytes have been */
    /* read. Reset the buffer position because there is a chance that */
    /* the third byte has slipped through.                            */
    buf->position = 4;
  }

  while (buf->read) {
    if (iec_data.iecflags & JIFFY_LOAD) {
      /* Signal to the C64 that we're ready to send the next block */
      set_data(0);
      set_clock(1);
      /* FFA0 - this delay is required so the C64 can see data low even */
      /*        if it hits a badline at the worst possible moment       */
      _delay_us(55);
    }

    do {
      uint8_t finalbyte = (buf->position == buf->lastused);
      if (iec_data.iecflags & JIFFY_LOAD) {
        /* Send a byte using the LOAD protocol variant */
        /* The final byte in the buffer must be sent with Clock low   */
        /* to signal that the next transfer will take some time.      */
        /* The C64 samples this just after it has set Data Low before */
        /* the first bitpair. If this marker is not set the time      */
        /* between two bytes outside the assembler function must not  */
        /* exceed ~38 C64 cycles (estimated) or the computer may      */
        /* see a previous data bit as the marker.                     */
        if (jiffy_send(buf->data[buf->position],0,128 | !finalbyte)) {
          /* Abort if ATN was seen */
          check_atn();
          return -1;
        }

        if (finalbyte && buf->sendeoi) {
          /* Send EOI marker */
          _delay_us(100);
          set_clock(1);
          _delay_us(100);
          set_clock(0);
          _delay_us(100);
          set_clock(1);
        }
      } else {
        if (finalbyte && buf->sendeoi) {
          /* Send with EOI */
          uint8_t res = iec_putc(buf->data[buf->position],1);
          if (iec_data.iecflags & JIFFY_ACTIVE) {
            /* Jiffy resets the EOI condition on the bus after 30-40us. */
            /* We use 50 to play it safe.                               */
            _delay_us(50);
            set_data(1);
            set_clock(0);
          }
          if (res) {
            uart_putc('Q');
            return 1;
          }
        } else {
          /* Send without EOI */
          if (iec_putc(buf->data[buf->position],0)) {
            uart_putc('V');
            return 1;
          }
        }
      }
    } while (buf->position++ < buf->lastused);

    if (buf->sendeoi && (cmd & 0x0f) != 0x0f && !buf->recordlen)
      break;

    if (buf->refill(buf)) {
      iec_data.bus_state = BUS_CLEANUP;
      return 1;
    }
  }

  return 0;
}



/* ------------------------------------------------------------------------- */
/*  Initialization and main loop                                             */
/* ------------------------------------------------------------------------- */


void init_iec(void) {
#ifdef IEC_SEPARATE_OUT
  /* Set up the port: Output bits as output, all others as input */
  IEC_DDROUT |=            IEC_OBIT_ATN | IEC_OBIT_CLOCK | IEC_OBIT_DATA | IEC_OBIT_SRQ;
  IEC_DDRIN  &= (uint8_t)~(IEC_BIT_ATN  | IEC_BIT_CLOCK  | IEC_BIT_DATA  | IEC_BIT_SRQ);
  /* Enable pullups on the input pins and set the output lines to high */
  IEC_PORT   &= (uint8_t)~(IEC_OBIT_ATN | IEC_OBIT_CLOCK | IEC_OBIT_DATA | IEC_OBIT_SRQ);
  IEC_PORTIN |= IEC_BIT_ATN | IEC_BIT_CLOCK | IEC_BIT_DATA | IEC_BIT_SRQ;
#else
  /* Pullups would be nice, but AVR can't switch from */
  /* low output to hi-z input directly                */
  IEC_DDR  &= (uint8_t)~(IEC_BIT_ATN | IEC_BIT_CLOCK | IEC_BIT_DATA | IEC_BIT_SRQ);
  IEC_PORT &= (uint8_t)~(IEC_BIT_ATN | IEC_BIT_CLOCK | IEC_BIT_DATA | IEC_BIT_SRQ);
#endif

  /* Prepare IEC interrupt (if any) */
  IEC_INT_SETUP();

#ifndef IEC_INT_VECT
  /* Issue an interrupt every 500us with timer 2 for ATN-Acknowledge.    */
  /* The exact timing isn't critical, it just has to be faster than 1ms. */
  /* Every 800us was too slow in rare situations.                        */
  OCR2A = 125;
  TCNT2 = 0;
  /* On the mega32 both registers are the same, so OR those bits in */
  TCCR2B = 0;
  TCCR2A |= _BV(WGM21); // CTC mode
  TCCR2B |= _BV(CS20) | _BV(CS21); // prescaler /32
#endif

  /* Read the hardware-set device address */
  DEVICE_SELECT_SETUP();
  _delay_ms(1);
  device_address = DEVICE_SELECT;

  set_error(ERROR_DOSVERSION);
}

void iec_mainloop(void) {
  int16_t cmd = 0; // make gcc happy...

  iec_data.bus_state = BUS_IDLE;

  while (1) {
    switch (iec_data.bus_state) {
    case BUS_SLEEP:
      set_atn_irq(0);
      set_data(1);
      set_clock(1);
      set_error(ERROR_OK);
      set_busy_led(0);
      set_dirty_led(1);

      /* Wait until the sleep key is used again */
      while (!key_pressed(KEY_SLEEP)) ;
      reset_key(KEY_SLEEP);

      update_leds();

      iec_data.bus_state = BUS_IDLE;
      break;

    case BUS_IDLE:  // EBFF
      /* Wait for ATN */
      set_atn_irq(1);
      while (IEC_ATN) {
        if (key_pressed(KEY_NEXT | KEY_PREV | KEY_HOME)) {
          change_disk();
        } else if (key_pressed(KEY_SLEEP)) {
          reset_key(KEY_SLEEP);
          iec_data.bus_state = BUS_SLEEP;
          break;
        }
      }

      if (iec_data.bus_state != BUS_SLEEP)
        iec_data.bus_state = BUS_FOUNDATN;
      break;

    case BUS_FOUNDATN: // E85B
      /* Pull data low to say we're here */
      set_clock(1);
      set_data(0);
      set_atn_irq(0);

      iec_data.device_state = DEVICE_IDLE;
      iec_data.bus_state    = BUS_ATNACTIVE;
      iec_data.iecflags &= (uint8_t)~(EOI_RECVD | JIFFY_ACTIVE | JIFFY_LOAD);

      /* Slight protocol violation:                        */
      /*   Wait until clock is low or 100us have passed    */
      /*   The C64 doesn't always pull down the clock line */
      /*   before ATN, this loop should keep us in sync.   */

      start_timeout(TIMEOUT_US(100));
      while (IEC_CLOCK && !has_timed_out())
        if (IEC_ATN)
          iec_data.bus_state = BUS_ATNPROCESS;

      while (!IEC_CLOCK)
        if (IEC_ATN)
          iec_data.bus_state = BUS_ATNPROCESS;

      break;

    case BUS_ATNACTIVE: // E884
      cmd = iec_getc();

      if (cmd < 0) {
        /* check_atn changed our state */
        uart_putc('C');
        break;
      }

      uart_putc('A');
      uart_puthex(cmd);
      uart_putcrlf();

      if (cmd == 0x3f) { /* Unlisten */
        if (iec_data.device_state == DEVICE_LISTEN)
          iec_data.device_state = DEVICE_IDLE;
        iec_data.bus_state = BUS_ATNFINISH;
      } else if (cmd == 0x5f) { /* Untalk */
        if (iec_data.device_state == DEVICE_TALK)
          iec_data.device_state = DEVICE_IDLE;
        iec_data.bus_state = BUS_ATNFINISH;
      } else if (cmd == 0x40+device_address) { /* Talk */
        iec_data.device_state = DEVICE_TALK;
        iec_data.bus_state = BUS_FORME;
      } else if (cmd == 0x20+device_address) { /* Listen */
        iec_data.device_state = DEVICE_LISTEN;
        iec_data.bus_state = BUS_FORME;
      } else if ((cmd & 0x60) == 0x60) {
        /* Check for OPEN/CLOSE/DATA */
        /* JiffyDOS uses a slightly modified protocol for LOAD that */
        /* is activated by using 0x61 instead of 0x60 in the TALK   */
        /* state. The original floppy code has additional checks    */
        /* that force the non-load Jiffy protocol for file types    */
        /* other than SEQ and PRG.                                  */
        /* Please note that $ is special-cased in the kernal so it  */
        /* will never trigger this.                                 */
        if (cmd == 0x61 && iec_data.device_state == DEVICE_TALK) {
          cmd = 0x60;
          iec_data.iecflags |= JIFFY_LOAD;
        }

        iec_data.secondary_address = cmd & 0x0f;
        /* 1571 handles close (0xe0-0xef) here, so we do that too. */
        if ((cmd & 0xf0) == 0xe0) {
          if (cmd == 0xef) {
            /* Close all buffers if sec. 15 is closed */
            if (free_multiple_buffers(FMB_USER_CLEAN)) {
              /* The 1571 error generator/handler always jumps to BUS_CLEANUP */
              iec_data.bus_state = BUS_CLEANUP;
              break;
            }
            set_error(ERROR_OK);
          } else {
            /* Close a single buffer */
            buffer_t *buf;
            buf = find_buffer(iec_data.secondary_address);
            if (buf != NULL) {
              if (buf->cleanup(buf)) {
                free_buffer(buf);
                iec_data.bus_state = BUS_CLEANUP;
                break;
              }
              /* Free the buffer */
              free_buffer(buf);
            }
          }
          iec_data.bus_state = BUS_FORME;
        } else {
          iec_data.bus_state = BUS_ATNFINISH;
        }
      } else {
        // Not me
        iec_data.bus_state = BUS_NOTFORME;
      }
      break;

    case BUS_FORME: // E8D2
      if (!IEC_ATN)
        iec_data.bus_state = BUS_ATNACTIVE;
      else
        iec_data.bus_state = BUS_ATNPROCESS;
      break;

    case BUS_NOTFORME: // E8FD
      set_atn_irq(0);
      set_clock(1);
      set_data(1);
      iec_data.bus_state = BUS_ATNFINISH;
      break;

    case BUS_ATNFINISH: // E902
      while (!IEC_ATN) ;
      iec_data.bus_state = BUS_ATNPROCESS;
      break;

    case BUS_ATNPROCESS: // E8D7
      set_atn_irq(1);

      if (iec_data.device_state == DEVICE_LISTEN) {
        if (iec_listen_handler(cmd))
          break;
      } else if (iec_data.device_state == DEVICE_TALK) {
        set_data(1);
        _delay_us(50);   // Implicit delay, fudged
        set_clock(0);
        _delay_us(70);   // Implicit delay, estimated

        if (iec_talk_handler(cmd))
          break;

      }
      iec_data.bus_state = BUS_CLEANUP;
      break;

    case BUS_CLEANUP:
      set_atn_irq(1);
      // 836B
      set_clock(1);
      set_data(1);

      //   0x255 -> A61C
      /* Handle commands and filenames */
      if (iec_data.iecflags & COMMAND_RECVD) {

#ifdef HAVE_HOTPLUG
        /* This seems to be a nice point to handle card changes */
        if (disk_state != DISK_OK) {
          set_busy_led(1);
          /* If the disk was changed the buffer contents are useless */
          if (disk_state == DISK_CHANGED || disk_state == DISK_REMOVED) {
            free_multiple_buffers(FMB_ALL);
            init_change();
            init_fatops(0);
          } else
            /* Disk state indicated an error, try to recover by initialising */
            init_fatops(1);
          
          update_leds();
        }
#endif

        if (iec_data.secondary_address == 0x0f) {
          /* Command channel */
          parse_doscommand();
        } else {
          /* Filename in command buffer */
          datacrc = 0xffff;
          file_open(iec_data.secondary_address);
        }
        command_length = 0;
        iec_data.iecflags &= (uint8_t)~COMMAND_RECVD;
      }

      iec_data.bus_state = BUS_IDLE;
      break;
    }
  }
}
