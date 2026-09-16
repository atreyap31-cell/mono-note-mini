#ifndef PALA_USB_H
#define PALA_USB_H
#include <Arduino.h>

/* The card, as a drive.
 *
 * Plug the device into anything with a USB socket and the notes appear as a
 * folder. No app, no pairing, no network, no account - which is the only
 * transfer method that works on a machine you have never set up, a phone
 * belonging to someone else, or a computer with no internet at all.
 *
 * Two things this costs, both real:
 *
 * The card cannot be in two places at once. While the host has it, the device
 * must not touch it, so recording is unavailable until it is unplugged. That
 * is not a limitation that can be engineered away: a filesystem with two
 * writers and no lock between them corrupts, and the corruption shows up
 * later as missing notes rather than as an error.
 *
 * The chip has one USB port and it is either the serial/JTAG peripheral or
 * native USB, never both. Mass storage needs native USB, so the port the
 * device is flashed through is now a software CDC port that only exists while
 * the firmware is running. A firmware that crashes before USB starts cannot be
 * reached, and the way back is the BOOT button.
 */

bool usbDriveAvailable();     /* a card is mounted and could be handed over */
bool usbDriveActive();

/* Hands the card to the host. Unmounts the filesystem first, so nothing on
   this side is holding it. */
bool usbDriveBegin();

/* Takes it back and remounts. */
void usbDriveEnd();

/* True while a host is actually connected to the port. */
bool usbHostPresent();

/* Blocks written by the host since the drive was handed over - used to know
   whether anything changed and the note list needs reloading. */
uint32_t usbDriveWrites();

#endif
