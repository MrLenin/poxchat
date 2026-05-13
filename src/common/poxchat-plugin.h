/* PoxChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* You can distribute this header with your plugins for easy compilation */
#ifndef POXCHAT_PLUGIN_H
#define POXCHAT_PLUGIN_H

#include <time.h>

#define POXCHAT_PRI_HIGHEST	127
#define POXCHAT_PRI_HIGH		64
#define POXCHAT_PRI_NORM		0
#define POXCHAT_PRI_LOW		(-64)
#define POXCHAT_PRI_LOWEST	(-128)

#define POXCHAT_FD_READ		1
#define POXCHAT_FD_WRITE		2
#define POXCHAT_FD_EXCEPTION	4
#define POXCHAT_FD_NOTSOCKET	8

#define POXCHAT_EAT_NONE		0	/* pass it on through! */
#define POXCHAT_EAT_POXCHAT		1	/* don't let PoxChat see this event */
#define POXCHAT_EAT_PLUGIN	2	/* don't let other plugins see this event */
#define POXCHAT_EAT_ALL		(POXCHAT_EAT_POXCHAT|POXCHAT_EAT_PLUGIN)	/* don't let anything see this event */

#define POXCHAT_TOAST_INFO	0
#define POXCHAT_TOAST_NICK	1
#define POXCHAT_TOAST_TOPIC	2
#define POXCHAT_TOAST_MODE	3
#define POXCHAT_TOAST_JOIN	4
#define POXCHAT_TOAST_ERROR	5
#define POXCHAT_TOAST_SUCCESS	6

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _poxchat_plugin poxchat_plugin;
typedef struct _poxchat_list poxchat_list;
typedef struct _poxchat_hook poxchat_hook;
#ifndef PLUGIN_C
typedef struct _poxchat_context poxchat_context;
#endif
typedef struct
{
	time_t server_time_utc; /* 0 if not used */
} poxchat_event_attrs;

#ifndef PLUGIN_C
struct _poxchat_plugin
{
	/* these are only used on win32 */
	poxchat_hook *(*poxchat_hook_command) (poxchat_plugin *ph,
		    const char *name,
		    int pri,
		    int (*callback) (char *word[], char *word_eol[], void *user_data),
		    const char *help_text,
		    void *userdata);
	poxchat_hook *(*poxchat_hook_server) (poxchat_plugin *ph,
		   const char *name,
		   int pri,
		   int (*callback) (char *word[], char *word_eol[], void *user_data),
		   void *userdata);
	poxchat_hook *(*poxchat_hook_print) (poxchat_plugin *ph,
		  const char *name,
		  int pri,
		  int (*callback) (char *word[], void *user_data),
		  void *userdata);
	poxchat_hook *(*poxchat_hook_timer) (poxchat_plugin *ph,
		  int timeout,
		  int (*callback) (void *user_data),
		  void *userdata);
	poxchat_hook *(*poxchat_hook_fd) (poxchat_plugin *ph,
		   int fd,
		   int flags,
		   int (*callback) (int fd, int flags, void *user_data),
		   void *userdata);
	void *(*poxchat_unhook) (poxchat_plugin *ph,
	      poxchat_hook *hook);
	void (*poxchat_print) (poxchat_plugin *ph,
	     const char *text);
	void (*poxchat_printf) (poxchat_plugin *ph,
	      const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;
	void (*poxchat_command) (poxchat_plugin *ph,
	       const char *command);
	void (*poxchat_commandf) (poxchat_plugin *ph,
		const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;
	int (*poxchat_nickcmp) (poxchat_plugin *ph,
	       const char *s1,
	       const char *s2);
	int (*poxchat_set_context) (poxchat_plugin *ph,
		   poxchat_context *ctx);
	poxchat_context *(*poxchat_find_context) (poxchat_plugin *ph,
		    const char *servname,
		    const char *channel);
	poxchat_context *(*poxchat_get_context) (poxchat_plugin *ph);
	const char *(*poxchat_get_info) (poxchat_plugin *ph,
		const char *id);
	int (*poxchat_get_prefs) (poxchat_plugin *ph,
		 const char *name,
		 const char **string,
		 int *integer);
	poxchat_list * (*poxchat_list_get) (poxchat_plugin *ph,
		const char *name);
	void (*poxchat_list_free) (poxchat_plugin *ph,
		 poxchat_list *xlist);
	const char * const * (*poxchat_list_fields) (poxchat_plugin *ph,
		   const char *name);
	int (*poxchat_list_next) (poxchat_plugin *ph,
		 poxchat_list *xlist);
	const char * (*poxchat_list_str) (poxchat_plugin *ph,
		poxchat_list *xlist,
		const char *name);
	int (*poxchat_list_int) (poxchat_plugin *ph,
		poxchat_list *xlist,
		const char *name);
	void * (*poxchat_plugingui_add) (poxchat_plugin *ph,
		     const char *filename,
		     const char *name,
		     const char *desc,
		     const char *version,
		     char *reserved);
	void (*poxchat_plugingui_remove) (poxchat_plugin *ph,
			void *handle);
	int (*poxchat_emit_print) (poxchat_plugin *ph,
			const char *event_name, ...);
	int (*poxchat_read_fd) (poxchat_plugin *ph,
			void *src,
			char *buf,
			int *len);
	time_t (*poxchat_list_time) (poxchat_plugin *ph,
		poxchat_list *xlist,
		const char *name);
	char *(*poxchat_gettext) (poxchat_plugin *ph,
		const char *msgid);
	void (*poxchat_send_modes) (poxchat_plugin *ph,
		  const char **targets,
		  int ntargets,
		  int modes_per_line,
		  char sign,
		  char mode);
	char *(*poxchat_strip) (poxchat_plugin *ph,
	     const char *str,
	     int len,
	     int flags);
	void (*poxchat_free) (poxchat_plugin *ph,
	    void *ptr);
	int (*poxchat_pluginpref_set_str) (poxchat_plugin *ph,
		const char *var,
		const char *value);
	int (*poxchat_pluginpref_get_str) (poxchat_plugin *ph,
		const char *var,
		char *dest);
	int (*poxchat_pluginpref_set_int) (poxchat_plugin *ph,
		const char *var,
		int value);
	int (*poxchat_pluginpref_get_int) (poxchat_plugin *ph,
		const char *var);
	int (*poxchat_pluginpref_delete) (poxchat_plugin *ph,
		const char *var);
	int (*poxchat_pluginpref_list) (poxchat_plugin *ph,
		char *dest);
	poxchat_hook *(*poxchat_hook_server_attrs) (poxchat_plugin *ph,
		   const char *name,
		   int pri,
		   int (*callback) (char *word[], char *word_eol[],
							poxchat_event_attrs *attrs, void *user_data),
		   void *userdata);
	poxchat_hook *(*poxchat_hook_print_attrs) (poxchat_plugin *ph,
		  const char *name,
		  int pri,
		  int (*callback) (char *word[], poxchat_event_attrs *attrs,
						   void *user_data),
		  void *userdata);
	int (*poxchat_emit_print_attrs) (poxchat_plugin *ph, poxchat_event_attrs *attrs,
									 const char *event_name, ...);
	poxchat_event_attrs *(*poxchat_event_attrs_create) (poxchat_plugin *ph);
	void (*poxchat_event_attrs_free) (poxchat_plugin *ph,
									  poxchat_event_attrs *attrs);
	void (*poxchat_toast) (poxchat_plugin *ph,
	    const char *text,
	    int type);
	void (*poxchat_toastf) (poxchat_plugin *ph,
	    int type,
	    const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 3, 4)))
#endif
	;
};
#endif


poxchat_hook *
poxchat_hook_command (poxchat_plugin *ph,
		    const char *name,
		    int pri,
		    int (*callback) (char *word[], char *word_eol[], void *user_data),
		    const char *help_text,
		    void *userdata);

poxchat_event_attrs *poxchat_event_attrs_create (poxchat_plugin *ph);

void poxchat_event_attrs_free (poxchat_plugin *ph, poxchat_event_attrs *attrs);

poxchat_hook *
poxchat_hook_server (poxchat_plugin *ph,
		   const char *name,
		   int pri,
		   int (*callback) (char *word[], char *word_eol[], void *user_data),
		   void *userdata);

poxchat_hook *
poxchat_hook_server_attrs (poxchat_plugin *ph,
		   const char *name,
		   int pri,
		   int (*callback) (char *word[], char *word_eol[],
							poxchat_event_attrs *attrs, void *user_data),
		   void *userdata);

poxchat_hook *
poxchat_hook_print (poxchat_plugin *ph,
		  const char *name,
		  int pri,
		  int (*callback) (char *word[], void *user_data),
		  void *userdata);

poxchat_hook *
poxchat_hook_print_attrs (poxchat_plugin *ph,
		  const char *name,
		  int pri,
		  int (*callback) (char *word[], poxchat_event_attrs *attrs,
						   void *user_data),
		  void *userdata);

poxchat_hook *
poxchat_hook_timer (poxchat_plugin *ph,
		  int timeout,
		  int (*callback) (void *user_data),
		  void *userdata);

poxchat_hook *
poxchat_hook_fd (poxchat_plugin *ph,
		int fd,
		int flags,
		int (*callback) (int fd, int flags, void *user_data),
		void *userdata);

void *
poxchat_unhook (poxchat_plugin *ph,
	      poxchat_hook *hook);

void
poxchat_print (poxchat_plugin *ph,
	     const char *text);

void
poxchat_printf (poxchat_plugin *ph,
	      const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
;

void
poxchat_command (poxchat_plugin *ph,
	       const char *command);

void
poxchat_commandf (poxchat_plugin *ph,
		const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
;

int
poxchat_nickcmp (poxchat_plugin *ph,
	       const char *s1,
	       const char *s2);

int
poxchat_set_context (poxchat_plugin *ph,
		   poxchat_context *ctx);

poxchat_context *
poxchat_find_context (poxchat_plugin *ph,
		    const char *servname,
		    const char *channel);

poxchat_context *
poxchat_get_context (poxchat_plugin *ph);

const char *
poxchat_get_info (poxchat_plugin *ph,
		const char *id);

int
poxchat_get_prefs (poxchat_plugin *ph,
		 const char *name,
		 const char **string,
		 int *integer);

poxchat_list *
poxchat_list_get (poxchat_plugin *ph,
		const char *name);

void
poxchat_list_free (poxchat_plugin *ph,
		 poxchat_list *xlist);

const char * const *
poxchat_list_fields (poxchat_plugin *ph,
		   const char *name);

int
poxchat_list_next (poxchat_plugin *ph,
		 poxchat_list *xlist);

const char *
poxchat_list_str (poxchat_plugin *ph,
		poxchat_list *xlist,
		const char *name);

int
poxchat_list_int (poxchat_plugin *ph,
		poxchat_list *xlist,
		const char *name);

time_t
poxchat_list_time (poxchat_plugin *ph,
		 poxchat_list *xlist,
		 const char *name);

void *
poxchat_plugingui_add (poxchat_plugin *ph,
		     const char *filename,
		     const char *name,
		     const char *desc,
		     const char *version,
		     char *reserved);

void
poxchat_plugingui_remove (poxchat_plugin *ph,
			void *handle);

int 
poxchat_emit_print (poxchat_plugin *ph,
		  const char *event_name, ...);

int 
poxchat_emit_print_attrs (poxchat_plugin *ph, poxchat_event_attrs *attrs,
						  const char *event_name, ...);

char *
poxchat_gettext (poxchat_plugin *ph,
	       const char *msgid);

void
poxchat_send_modes (poxchat_plugin *ph,
		  const char **targets,
		  int ntargets,
		  int modes_per_line,
		  char sign,
		  char mode);

char *
poxchat_strip (poxchat_plugin *ph,
	     const char *str,
	     int len,
	     int flags);

void
poxchat_free (poxchat_plugin *ph,
	    void *ptr);

int
poxchat_pluginpref_set_str (poxchat_plugin *ph,
		const char *var,
		const char *value);

int
poxchat_pluginpref_get_str (poxchat_plugin *ph,
		const char *var,
		char *dest);

int
poxchat_pluginpref_set_int (poxchat_plugin *ph,
		const char *var,
		int value);
int
poxchat_pluginpref_get_int (poxchat_plugin *ph,
		const char *var);

int
poxchat_pluginpref_delete (poxchat_plugin *ph,
		const char *var);

int
poxchat_pluginpref_list (poxchat_plugin *ph,
		char *dest);

void
poxchat_toast (poxchat_plugin *ph,
	    const char *text,
	    int type);

void
poxchat_toastf (poxchat_plugin *ph,
	    int type,
	    const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 3, 4)))
#endif
;

#if !defined(PLUGIN_C) && (defined(WIN32) || defined(__CYGWIN__))
#ifndef POXCHAT_PLUGIN_HANDLE
#define POXCHAT_PLUGIN_HANDLE (ph)
#endif
#define poxchat_hook_command ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_command)
#define poxchat_event_attrs_create ((POXCHAT_PLUGIN_HANDLE)->poxchat_event_attrs_create)
#define poxchat_event_attrs_free ((POXCHAT_PLUGIN_HANDLE)->poxchat_event_attrs_free)
#define poxchat_hook_server ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_server)
#define poxchat_hook_server_attrs ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_server_attrs)
#define poxchat_hook_print ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_print)
#define poxchat_hook_print_attrs ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_print_attrs)
#define poxchat_hook_timer ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_timer)
#define poxchat_hook_fd ((POXCHAT_PLUGIN_HANDLE)->poxchat_hook_fd)
#define poxchat_unhook ((POXCHAT_PLUGIN_HANDLE)->poxchat_unhook)
#define poxchat_print ((POXCHAT_PLUGIN_HANDLE)->poxchat_print)
#define poxchat_printf ((POXCHAT_PLUGIN_HANDLE)->poxchat_printf)
#define poxchat_command ((POXCHAT_PLUGIN_HANDLE)->poxchat_command)
#define poxchat_commandf ((POXCHAT_PLUGIN_HANDLE)->poxchat_commandf)
#define poxchat_nickcmp ((POXCHAT_PLUGIN_HANDLE)->poxchat_nickcmp)
#define poxchat_set_context ((POXCHAT_PLUGIN_HANDLE)->poxchat_set_context)
#define poxchat_find_context ((POXCHAT_PLUGIN_HANDLE)->poxchat_find_context)
#define poxchat_get_context ((POXCHAT_PLUGIN_HANDLE)->poxchat_get_context)
#define poxchat_get_info ((POXCHAT_PLUGIN_HANDLE)->poxchat_get_info)
#define poxchat_get_prefs ((POXCHAT_PLUGIN_HANDLE)->poxchat_get_prefs)
#define poxchat_list_get ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_get)
#define poxchat_list_free ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_free)
#define poxchat_list_fields ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_fields)
#define poxchat_list_next ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_next)
#define poxchat_list_str ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_str)
#define poxchat_list_int ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_int)
#define poxchat_plugingui_add ((POXCHAT_PLUGIN_HANDLE)->poxchat_plugingui_add)
#define poxchat_plugingui_remove ((POXCHAT_PLUGIN_HANDLE)->poxchat_plugingui_remove)
#define poxchat_emit_print ((POXCHAT_PLUGIN_HANDLE)->poxchat_emit_print)
#define poxchat_emit_print_attrs ((POXCHAT_PLUGIN_HANDLE)->poxchat_emit_print_attrs)
#define poxchat_list_time ((POXCHAT_PLUGIN_HANDLE)->poxchat_list_time)
#define poxchat_gettext ((POXCHAT_PLUGIN_HANDLE)->poxchat_gettext)
#define poxchat_send_modes ((POXCHAT_PLUGIN_HANDLE)->poxchat_send_modes)
#define poxchat_strip ((POXCHAT_PLUGIN_HANDLE)->poxchat_strip)
#define poxchat_free ((POXCHAT_PLUGIN_HANDLE)->poxchat_free)
#define poxchat_pluginpref_set_str ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_set_str)
#define poxchat_pluginpref_get_str ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_get_str)
#define poxchat_pluginpref_set_int ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_set_int)
#define poxchat_pluginpref_get_int ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_get_int)
#define poxchat_pluginpref_delete ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_delete)
#define poxchat_pluginpref_list ((POXCHAT_PLUGIN_HANDLE)->poxchat_pluginpref_list)
#define poxchat_toast ((POXCHAT_PLUGIN_HANDLE)->poxchat_toast)
#define poxchat_toastf ((POXCHAT_PLUGIN_HANDLE)->poxchat_toastf)
#endif

#ifdef __cplusplus
}
#endif
#endif
