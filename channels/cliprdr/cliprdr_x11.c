/*
   Copyright (c) 2009-2010 Jay Sorg
   Copyright (c) 2010 Vic Lee

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <iconv.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <freerdp/types_ui.h>
#include "chan_stream.h"
#include "wait_obj.h"
#include "cliprdr_main.h"

#define LOG_LEVEL 1
#define LLOG(_level, _args) \
  do { if (_level < LOG_LEVEL) { printf _args ; } } while (0)
#define LLOGLN(_level, _args) \
  do { if (_level < LOG_LEVEL) { printf _args ; printf("\n"); } } while (0)

struct clipboard_format_mapping
{
	Atom target_format;
	uint32 format_id;
};

struct clipboard_data
{
	cliprdrPlugin * plugin;
	struct wait_obj * term_event;
	int thread_status;

	/* for locking the X11 Display, formats and data */
	pthread_mutex_t * mutex;

	Display * display;
	Atom clipboard_atom;
	Window window;
	uint32 * format_ids;
	int num_formats;
	char * data;
	uint32 data_format;
	int data_length;
	XEvent * respond;

	struct clipboard_format_mapping format_mappings[10];
	int num_format_mappings;
};

static int
clipboard_select_format_mapping(struct clipboard_data * cdata, Atom target)
{
	int i;
	int j;

	if (cdata->format_ids == NULL)
	{
		return -1;
	}
	for (i = 0; i < cdata->num_format_mappings; i++)
	{
		if (cdata->format_mappings[i].target_format != target)
		{
			continue;
		}
		for (j = 0; j < cdata->num_formats; j++)
		{
			if (cdata->format_mappings[i].format_id == cdata->format_ids[j])
			{
				return i;
			}
		}
	}
	return -1;
}

static void
clipboard_provide_data(struct clipboard_data * cdata, XEvent * respond)
{
	if (respond->xselection.property != None)
	{
		pthread_mutex_lock(cdata->mutex);
		XChangeProperty(cdata->display,
			respond->xselection.requestor,
			respond->xselection.property,
			respond->xselection.target, 8, PropModeReplace,
			(unsigned char *) cdata->data, cdata->data_length);
		pthread_mutex_unlock(cdata->mutex);
	}
}

static void
thread_process_selection_request(struct clipboard_data * cdata,
	XSelectionRequestEvent * req)
{
	int delay_respond;
	char * s;
	XEvent * respond;
	int i;
	uint32 format;

	delay_respond = 0;
	respond = (XEvent *) malloc(sizeof(XEvent));
	memset(respond, 0, sizeof(XEvent));
	respond->xselection.property = None;
	respond->xselection.type = SelectionNotify;
	respond->xselection.display = req->display;
	respond->xselection.requestor = req->requestor;
	respond->xselection.selection =req->selection;
	respond->xselection.target = req->target;
	respond->xselection.time = req->time;
	i = clipboard_select_format_mapping(cdata, req->target);
	if (i >= 0)
	{
		format = cdata->format_mappings[i].format_id;
		LLOGLN(10, ("clipboard_x11 selection_request: provide format 0x%04x", format));
		if (cdata->data != 0 && format == cdata->data_format)
		{
			/* Cached clipboard data available. Send it now */
			respond->xselection.property = req->property;
			clipboard_provide_data(cdata, respond);
		}
		else if (cdata->respond)
		{
			LLOGLN(0, ("cliprdr: thread_process_selection_request: duplicated request"));
		}
		else
		{
			/* Send clipboard data request to the server.
			 * Response will be postponed after receiving the data
			 */
			if (cdata->data)
			{
				free(cdata->data);
				cdata->data = NULL;
			}
			respond->xselection.property = req->property;
			cdata->respond = respond;
			cdata->data_format = format;
			delay_respond = 1;
			s = (char *) malloc(4);
			SET_UINT32(s, 0, format);
			cliprdr_send_packet(cdata->plugin, CB_FORMAT_DATA_REQUEST,
				0, s, 4);
			free(s);
		}
	}

	if (!delay_respond)
	{
		pthread_mutex_lock(cdata->mutex);
		XSendEvent(cdata->display, req->requestor, 0, 0, respond);
		XFlush(cdata->display);
		pthread_mutex_unlock(cdata->mutex);
		free(respond);
	}
}

static void *
thread_func(void * arg)
{
	struct clipboard_data * cdata;
	int x_socket;
	XEvent xevent;

	LLOGLN(10, ("clipboard_x11 thread_func: in"));

	cdata = (struct clipboard_data *) arg;
	cdata->thread_status = 1;
	x_socket = ConnectionNumber(cdata->display);
	while (1)
	{
		wait_obj_select(&cdata->term_event, 1, &x_socket, 1, -1);
		if (wait_obj_is_set(cdata->term_event))
		{
			break;
		}
		while (XPending(cdata->display))
		{
			memset(&xevent, 0, sizeof(xevent));
			pthread_mutex_lock(cdata->mutex);
			XNextEvent(cdata->display, &xevent);
			pthread_mutex_unlock(cdata->mutex);
			if (xevent.type == SelectionRequest &&
				xevent.xselectionrequest.owner == cdata->window)
			{
				thread_process_selection_request(cdata,
					&(xevent.xselectionrequest));
			}
		}
	}
	cdata->thread_status = -1;
	LLOGLN(10, ("clipboard_x11 thread_func: out"));
	return 0;
}

void *
clipboard_new(cliprdrPlugin * plugin)
{
	struct clipboard_data * cdata;
	pthread_t thread;

	cdata = (struct clipboard_data *) malloc(sizeof(struct clipboard_data));
	memset(cdata, 0, sizeof(struct clipboard_data));
	cdata->plugin = plugin;
	cdata->term_event = wait_obj_new("freerdpcliprdrx11term");
	cdata->thread_status = 0;
	cdata->mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(cdata->mutex, 0);

	/* Create X11 Display */
	cdata->display = XOpenDisplay(NULL);
	if (cdata->display == NULL)
	{
		LLOGLN(0, ("clipboard_new: unable to open X display"));
	}
	else
	{
		cdata->clipboard_atom = XInternAtom(cdata->display, "CLIPBOARD", False);
		if (cdata->clipboard_atom == None)
		{
			LLOGLN(0, ("clipboard_new: unable to get CLIPBOARD atom"));
		}
		cdata->window = XCreateSimpleWindow(cdata->display, DefaultRootWindow(cdata->display),
			0, 0, 100, 100, 0, 0, 0);
		if (cdata->window == None)
		{
			LLOGLN(0, ("clipboard_new: unable to create window"));
		}

		cdata->format_mappings[0].target_format = XInternAtom(cdata->display, "UTF8_STRING", False);
		cdata->format_mappings[0].format_id = CF_UNICODETEXT;
		cdata->format_mappings[1].target_format = XInternAtom(cdata->display, "UTF8_STRING", False);
		cdata->format_mappings[1].format_id = CF_TEXT;
		cdata->num_format_mappings = 2;
	}
	pthread_create(&thread, 0, thread_func, cdata);
	pthread_detach(thread);
	return cdata;
}

int
clipboard_sync(void * device_data)
{
	struct clipboard_data * cdata;
	char * s;
	int size;

	LLOGLN(10, ("clipboard_sync"));

	cdata = (struct clipboard_data *) device_data;

	size = 72;
	s = (char *) malloc(size);
	memset(s, 0, size);
	SET_UINT32(s, 0, (uint32)CF_UNICODETEXT);
	SET_UINT32(s, 36, (uint32)CF_TEXT);
	cliprdr_send_packet(cdata->plugin, CB_FORMAT_LIST,
		0, s, size);
	free(s);
	return 0;
}

int
clipboard_format_list(void * device_data, int flag,
	char * data, int length)
{
	struct clipboard_data * cdata;
	int i;

	LLOGLN(10, ("clipboard_format_list: length=%d", length));
	if (length % 36 != 0)
	{
		LLOGLN(0, ("clipboard_format_list: length is not devided by 36"));
		return 1;
	}

	cdata = (struct clipboard_data *) device_data;

	pthread_mutex_lock(cdata->mutex);
	if (cdata->data)
	{
		free(cdata->data);
		cdata->data = NULL;
	}
	if (cdata->format_ids)
	{
		free(cdata->format_ids);
	}
	cdata->num_formats = length / 36;
	cdata->format_ids = (uint32 *) malloc(sizeof(uint32) * cdata->num_formats);
	for (i = 0; i < cdata->num_formats; i++)
	{
		cdata->format_ids[i] = GET_UINT32(data, i * 36);
#if 0
		if ((flag & CB_ASCII_NAMES) == 0)
		{
			/* Unicode name, just remove the higher byte */
			int j;
			for (j = 1; j < 16; j++)
			{
				*(data + i * 36 + 4 + j) =  *(data + i * 36 + 4 + j * 2);
			}
			*(data + i * 36 + 4 + 16) = 0;
		}
#endif
		LLOGLN(10, ("clipboard_format_list: format 0x%04x %s",
			cdata->format_ids[i], data + i * 36 + 4));
	}
	XSetSelectionOwner(cdata->display, cdata->clipboard_atom, cdata->window, CurrentTime);
	XFlush(cdata->display);
	pthread_mutex_unlock(cdata->mutex);

	return 0;
}

int
clipboard_request_data(void * device_data, int format)
{
	struct clipboard_data * cdata;

	LLOGLN(10, ("clipboard_request_data: format=%d", format));
	cdata = (struct clipboard_data *) device_data;
	return 0;
}

static void
clipboard_handle_text(struct clipboard_data * cdata,
	char * data, int length)
{
	cdata->data = (char *) malloc(length);
	memcpy(cdata->data, data, length);
	cdata->data_length = length;
}

static void
clipboard_handle_unicodetext(struct clipboard_data * cdata,
	char * data, int length)
{
	iconv_t cd;
	size_t avail;
	size_t in_size;
	char * out;

	cd = iconv_open("UTF-8", "UTF-16LE");
	if (cd == (iconv_t) - 1)
	{
		LLOGLN(0, ("clipboard_handle_unicodetext: iconv_open failed."));
		return;
	}
	cdata->data_length = length * 2;
	cdata->data = malloc(cdata->data_length);
	memset(cdata->data, 0, cdata->data_length);
	in_size = (size_t)length;
	avail = cdata->data_length;
	out = cdata->data;
	iconv(cd, &data, &in_size, &out, &avail);
	iconv_close(cd);
}

int
clipboard_handle_data(void * device_data, int flag,
	char * data, int length)
{
	struct clipboard_data * cdata;

	LLOGLN(10, ("clipboard_handle_data: length=%d", length));
	cdata = (struct clipboard_data *) device_data;
	if (cdata->respond == NULL)
	{
		LLOGLN(10, ("clipboard_handle_data: unexpected data"));
		return 1;
	}
	if ((flag & CB_RESPONSE_FAIL) != 0)
	{
		cdata->respond->xselection.property = None;
		LLOGLN(0, ("clipboard_handle_data: server responded fail."));
	}
	else
	{
/*		int i;
		for (i = 0; i < length; i++)
		{
			printf("%x%x ", data[i]>>4, data[i]&15);
		}
		printf("\n");*/
		if (cdata->data)
		{
			free(cdata->data);
			cdata->data = NULL;
		}
		switch (cdata->data_format)
		{
		case CF_TEXT:
			clipboard_handle_text(cdata, data, length);
			break;
		case CF_UNICODETEXT:
			clipboard_handle_unicodetext(cdata, data, length);
			break;
		default:
			cdata->respond->xselection.property = None;
			break;
		}
		clipboard_provide_data(cdata, cdata->respond);
	}

	pthread_mutex_lock(cdata->mutex);
	XSendEvent(cdata->display, cdata->respond->xselection.requestor,
		0, 0, cdata->respond);
	XFlush(cdata->display);
	free(cdata->respond);
	cdata->respond = NULL;
	pthread_mutex_unlock(cdata->mutex);

	return 0;
}

int
clipboard_handle_caps(void * device_data, int flag,
	char * data, int length)
{
	struct clipboard_data * cdata;
	char * s;
	int size;

	cdata = (struct clipboard_data *) device_data;

	size = 16;
	s = (char *) malloc(size);
	memset(s, 0, size);
	SET_UINT16(s, 0, 1);
	SET_UINT16(s, 2, 0);
	SET_UINT16(s, 4, 1); /* CB_CAPSTYPE_GENERAL */
	SET_UINT16(s, 6, 12); /* length */
	SET_UINT32(s, 8, 2); /* version */
	SET_UINT32(s, 12, 0); /* generalFlags */
	cliprdr_send_packet(cdata->plugin, CB_CLIP_CAPS,
		0, s, size);
	free(s);
	return 0;
}

void
clipboard_free(void * device_data)
{
	struct clipboard_data * cdata;
	int index;

	cdata = (struct clipboard_data *) device_data;
	wait_obj_set(cdata->term_event);
	index = 0;
	while ((cdata->thread_status > 0) && (index < 100))
	{
		index++;
		usleep(250 * 1000);
	}
	wait_obj_free(cdata->term_event);

	pthread_mutex_destroy(cdata->mutex);
	free(cdata->mutex);

	if (cdata->window != None)
	{
		XDestroyWindow(cdata->display, cdata->window);
	}
	if (cdata->display != NULL)
	{
		XCloseDisplay(cdata->display);
	}
	if (cdata->format_ids)
	{
		free(cdata->format_ids);
	}
	if (cdata->data)
	{
		free(cdata->data);
	}
	if (cdata->respond)
	{
		free(cdata->respond);
	}

	free(device_data);
}
