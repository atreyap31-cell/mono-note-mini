#ifndef PALA_SYNC_H
#define PALA_SYNC_H
#include <Arduino.h>

/* Getting notes off the device.
 *
 * This used to publish to a GitHub repo, which meant the device carried a
 * personal access token in plain text - a credential that could write to a
 * repository, sitting on a thing small enough to lose, on hardware with no
 * secure element. It also needed an account, a repo and a token pasted in
 * before a single note could leave.
 *
 * Now it posts each note to the same machine that serves the web page. There
 * is no credential, nothing leaves the network, and setup is one address.
 */

/* Stable per-device name taken from the MAC, e.g. "mnm-1dd81ab0". */
String syncDeviceId();

/* True when an address to upload to has been set. */
bool syncConfigured();

/* Uploads one note - the audio, and its transcript if there is one. Needs an
 * active Wi-Fi connection. `err` carries something specific enough to act on:
 * an HTTP status, or what the connection did instead of working.
 */
bool syncUploadNote(const String& base, String& err);

#endif
