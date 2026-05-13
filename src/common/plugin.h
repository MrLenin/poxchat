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

#ifndef POXCHAT_COMMONPLUGIN_H
#define POXCHAT_COMMONPLUGIN_H

#ifdef PLUGIN_C
struct _poxchat_plugin
{
	/* Keep these in sync with poxchat-plugin.h */
	/* !!don't change the order, to keep binary compat!! */
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
	      const char *format, ...);
	void (*poxchat_command) (poxchat_plugin *ph,
	       const char *command);
	void (*poxchat_commandf) (poxchat_plugin *ph,
		const char *format, ...);
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
	void *(*poxchat_read_fd) (poxchat_plugin *ph);
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
	    const char *format, ...);

	/* PRIVATE FIELDS! */
	void *handle;		/* from dlopen */
	char *filename;	/* loaded from */
	char *name;
	char *desc;
	char *version;
	session *context;
	void *deinit_callback;	/* pointer to poxchat_plugin_deinit */
	unsigned int fake:1;		/* fake plugin. Added by poxchat_plugingui_add() */
	unsigned int free_strings:1;		/* free name,desc,version? */
};
#endif

GModule *module_load (char *filename);
char *plugin_load (session *sess, char *filename, char *arg);
int plugin_reload (session *sess, char *name, int by_filename);
void plugin_add (session *sess, char *filename, void *handle, void *init_func, void *deinit_func, char *arg, int fake);
int plugin_kill (char *name, int by_filename);
void plugin_kill_all (void);
void plugin_auto_load (session *sess);
int plugin_emit_command (session *sess, char *name, char *word[], char *word_eol[]);
int plugin_emit_server (session *sess, char *name, char *word[], char *word_eol[],
						time_t server_time);
int plugin_emit_print (session *sess, char *word[], time_t server_time);
int plugin_emit_dummy_print (session *sess, char *name);
int plugin_emit_keypress (session *sess, unsigned int state, unsigned int keyval, gunichar key);
GList* plugin_command_list(GList *tmp_list);
int plugin_show_help (session *sess, char *cmd);
void plugin_command_foreach (session *sess, void *userdata, void (*cb) (session *sess, void *userdata, char *name, char *usage));
session *plugin_find_context (const char *servname, const char *channel, server *current_server);

/* On macOS, G_MODULE_SUFFIX says "so" but meson uses "dylib"
 * https://github.com/mesonbuild/meson/issues/1160 */
#if defined(__APPLE__)
#  define PLUGIN_SUFFIX "dylib"
#else
#  define PLUGIN_SUFFIX G_MODULE_SUFFIX
#endif

#endif
