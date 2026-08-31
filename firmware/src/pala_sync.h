#ifndef PALA_SYNC_H
#define PALA_SYNC_H
#include <Arduino.h>
#include <vector>

/* Publishing notes to the owner's own GitHub repo, from the device itself.

   This used to happen in the browser, on the page the device serves. Moving it
   into the firmware is what turns "sync" into one press rather than a press
   plus opening a laptop - but it means the device carries a GitHub token, so
   the connection is TLS-pinned rather than setInsecure(). See pala_gh_ca.h.

   Each device writes one file of its own, data/<deviceId>.json, and is the
   only writer of it. That is what makes "everyone opens the same site and sees
   only their own notes" true by access control rather than by obscurity, and
   it also means publishing can overwrite wholesale instead of merging - there
   is no second writer to lose an edit to. */

struct SyncNote {
  String base;        /* rec_20260812_101200 */
  String tag;         /* one of the five, or empty */
  String transcript;  /* may be empty if not transcribed yet */
  uint32_t secs;
};

struct SyncTodo {
  String text;
  bool done;
};

/* Stable per-device name taken from the MAC, e.g. "mnm-1dd81ab0". Printed on
   the IP screen so it can be typed into the web app. */
String syncDeviceId();

/* True when owner, repo and token are all set. */
bool syncConfigured();

/* Path this device publishes to inside the repo. */
String syncPath();

/* Publishes every note and to-do, replacing this device's file. Requires an
   active Wi-Fi connection. `err` carries something specific enough to act on -
   an HTTP status, a TLS failure, GitHub's own message. */
bool syncPublish(const std::vector<SyncNote>& notes,
                 const std::vector<SyncTodo>& todos,
                 String& err);

#endif
