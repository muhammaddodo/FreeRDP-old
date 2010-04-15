/* -*- c-basic-offset: 8 -*-
   FreeRDP: A Remote Desktop Protocol client.
   User interface services - X keyboard mapping using XKB

   Copyright (C) Marc-Andre Moreau <marcandre.moreau@gmail.com> 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef __FREERDP_KBD_H
#define	__FREERDP_KBD_H

extern unsigned char keycodeToVkcode[256];

typedef struct _virtualKey
{
	// Scan code
	int scancode;

	// Flags	
	int flags;

	// Name of virtual key
	char name[32];

} virtualKey;

extern virtualKey virtualKeyboard[256];

unsigned int
freerdp_kbd_init();

#endif // __FREERDP_KBD_H

