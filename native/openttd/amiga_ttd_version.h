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

#define AMIGA_TTD_VERSION "0.9.8"
#define AMIGA_TTD_DATE    "20260725"

#endif /* AMIGA_TTD_VERSION_H */
