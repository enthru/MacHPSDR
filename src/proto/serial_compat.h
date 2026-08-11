/*  serial_compat.h

    The serial-port seam, alongside net_compat.h.

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

/*
 * On Linux/macOS this is <termios.h> and nothing else.  Windows has no termios
 * at all, so serial CAT needs the Win32 comm API — see the platform branches in
 * rigctl.c, which is the only file that actually drives a port.  This header
 * exists because the BAUD RATE CONSTANTS leak wider than that: rx->
 * rigctl_serial_baudrate is set in receiver.c and compared in
 * receiver_dialog.c's drop-down, so B4800..B38400 have to exist everywhere.
 *
 * The trick that keeps those two files unchanged: on POSIX B9600 is an opaque
 * termios speed_t token, but here it is DEFINED AS THE NUMBER, which is exactly
 * what SetCommState's DCB.BaudRate wants.  So the stored value is already the
 * right thing on both platforms and the UI's `== B9600` comparisons still work.
 */

#ifndef SERIAL_COMPAT_H
#define SERIAL_COMPAT_H

#if defined(_WIN32)

/* The values ARE the baud rates — see above. */
#ifndef B4800
#define B4800     4800
#endif
#ifndef B9600
#define B9600     9600
#endif
#ifndef B19200
#define B19200   19200
#endif
#ifndef B38400
#define B38400   38400
#endif
#ifndef B57600
#define B57600   57600
#endif
#ifndef B115200
#define B115200 115200
#endif

#else

#include <termios.h>

#endif /* _WIN32 */

#endif /* SERIAL_COMPAT_H */
