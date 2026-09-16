#include "pala_usb.h"
#include "user_config.h"
#include <SD_MMC.h>
#include <sdmmc_cmd.h>

#if ARDUINO_USB_MODE == 0
#include "USB.h"
#include "USBMSC.h"

static USBMSC msc;
static bool   active = false;
static volatile uint32_t writes = 0;

/* SDMMCFS keeps its sdmmc_card_t protected, and there is no accessor. Reaching
   it through a derived type is the standard way in, and the alternative is
   dropping Arduino's SD_MMC entirely and mounting the card through ESP-IDF -
   a much larger change for one pointer. */
namespace {
struct CardPeek : public fs::SDMMCFS {
  static sdmmc_card_t* from(fs::SDMMCFS& f) { return static_cast<CardPeek&>(f)._card; }
};
}  // namespace

static sdmmc_card_t* card() { return CardPeek::from(SD_MMC); }

/* The host addresses the card in blocks, and this passes them straight
   through. `offset` is non-zero only when a host reads part of a block, which
   the SD layer cannot do - so anything unaligned is refused rather than
   silently returning the wrong bytes. */
static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  sdmmc_card_t* c = card();
  if (!c || offset != 0) return -1;
  const uint32_t secSize = c->csd.sector_size;
  if (bufsize % secSize) return -1;
  if (sdmmc_read_sectors(c, buffer, lba, bufsize / secSize) != ESP_OK) return -1;
  return (int32_t)bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  sdmmc_card_t* c = card();
  if (!c || offset != 0) return -1;
  const uint32_t secSize = c->csd.sector_size;
  if (bufsize % secSize) return -1;
  if (sdmmc_write_sectors(c, buffer, lba, bufsize / secSize) != ESP_OK) return -1;
  writes++;
  return (int32_t)bufsize;
}

/* The host asking to eject. Saying yes and then keeping the media present
   would leave it believing the drive is gone while it still answers reads. */
static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  if (load_eject && !start) msc.mediaPresent(false);
  return true;
}

bool usbDriveAvailable() { return card() != nullptr; }
bool usbDriveActive()    { return active; }
uint32_t usbDriveWrites(){ return writes; }

bool usbDriveBegin() {
  if (active) return true;
  sdmmc_card_t* c = card();
  if (!c) return false;

  /* Let go of the filesystem before the host touches the blocks underneath it.
     Leaving it mounted would mean two writers with no lock between them, and
     the damage from that surfaces later as missing notes rather than as an
     error anyone sees at the time. */
  SD_MMC.end();

  writes = 0;
  msc.vendorID("MonoNote");
  msc.productID("Notes");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);

  /* SD_MMC.end() does not free the card structure, so the geometry read here
     is still the one the host needs. */
  if (!msc.begin(c->csd.capacity, c->csd.sector_size)) {
    SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
    SD_MMC.begin("/sdcard", true);
    return false;
  }
  active = true;
  return true;
}

void usbDriveEnd() {
  if (!active) return;
  msc.mediaPresent(false);
  msc.end();
  active = false;
  /* Remount. Whatever the host did to the card, this side has to read it
     fresh - the directory it had cached is no longer trustworthy. */
  SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
  SD_MMC.begin("/sdcard", true);
}

bool usbHostPresent() {
  /* TinyUSB knows whether the port is enumerated. The old test poked the
     USB-Serial/JTAG peripheral's start-of-frame flag, which does not exist in
     this mode - that peripheral is not the one in use. */
  return (bool)USB;
}

#else  /* built without native USB */

bool     usbDriveAvailable() { return false; }
bool     usbDriveActive()    { return false; }
uint32_t usbDriveWrites()    { return 0; }
bool     usbDriveBegin()     { return false; }
void     usbDriveEnd()       {}
bool     usbHostPresent()    { return false; }

#endif
