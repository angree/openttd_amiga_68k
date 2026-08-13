/* amiga_ttd_version.h - single source of truth for the AmiTTD port version.
 *
 * This is the ONLY place the Amiga port version number lives. It is read by:
 *   - src/rev.cpp.in  (appends it to _openttd_revision, so it shows in the
 *     intro-window caption under the logo);
 *   - build/make-splash.py (stamps "v" + AMIGA_TTD_VERSION into the splash).
 *
 * Bump the string here and BOTH the caption and the splash follow. No build
 * date is carried.
 */
#ifndef AMIGA_TTD_VERSION_H
#define AMIGA_TTD_VERSION_H

#define AMIGA_TTD_VERSION "1.2.2"
#define AMIGA_TTD_DATE    "20260813"

/* How many leading characters of _openttd_revision decide MULTIPLAYER
 * compatibility. The string starts "AmiTTD X.Y.Z / OpenTTD 1.0.5 ...", and 10
 * characters cover exactly "AmiTTD X.Y" - the major and minor version.
 *
 * So the THIRD digit is a patch level: it may move freely and those builds
 * still play together, because a patch release changes neither the savegame
 * format, nor the command table, nor the wire protocol. To break compatibility
 * on purpose, move the FIRST or SECOND digit. Never widen this to 12, which
 * would drag the patch level back into the comparison. */
#define AMIGA_TTD_NETWORK_COMPAT_CHARS 10

#endif /* AMIGA_TTD_VERSION_H */
