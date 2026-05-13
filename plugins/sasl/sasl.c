/* PoxChat
 * Copyright (c) 2010 <ygrek@autistici.org>
 * Copyright (c) 2012 Berke Viktor.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * SASL authentication plugin for PoxChat
 * Extremely primitive: only PLAIN, no error checking
 *
 * http://ygrek.org.ua/p/cap_sasl.html
 *
 * Docs:
 *  http://hg.atheme.org/charybdis/charybdis/file/6144f52a119b/doc/sasl.txt
 *  http://tools.ietf.org/html/rfc4422
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <glib.h>

#include "poxchat-plugin.h"

static poxchat_plugin *ph;   /* plugin handle */
static char name[] = "SASL";
static char desc[] = "SASL authentication plugin for PoxChat";
static char version[] = "1.2";
static const char sasl_help[] = "SASL Usage:\n /SASL ADD <login> <password> <network>, enable/update SASL authentication for given network\n /SASL DEL <network>, disable SASL authentication for given network\n /SASL LIST, get the list of SASL-enabled networks\n";

struct sasl_info
{
	char const* login;
	char const* password;
	char const* network;
};

typedef struct sasl_info sasl_info;

static int
add_info (char const* login, char const* password, char const* network)
{
	char buffer[512];

	sprintf (buffer, "%s:%s", login, password);
	return poxchat_pluginpref_set_str (ph, network, buffer);
}

static int
del_info (char const* network)
{
	return poxchat_pluginpref_delete (ph, network);
}

static void
print_info ()
{
	char list[512];
	char* token;

	if (poxchat_pluginpref_list (ph, list))
	{
		poxchat_printf (ph, "%s\tSASL-enabled networks:", name);
		poxchat_printf (ph, "%s\t----------------------", name);
		token = strtok (list, ",");
		while (token != NULL)
		{
			poxchat_printf (ph, "%s\t%s", name, token);
			token = strtok (NULL, ",");
		}
	}
	else
	{
		poxchat_printf (ph, "%s\tThere are no SASL-enabled networks currently", name);
	}
}

static sasl_info*
find_info (char const* network)
{
	char buffer[512];
	char* token;
	sasl_info* cur = (sasl_info*) malloc (sizeof (sasl_info));

	if (poxchat_pluginpref_get_str (ph, network, buffer))
	{
		token = strtok (buffer, ":");
		cur->login = g_strdup (token);
		token = strtok (NULL, ":");
		cur->password = g_strdup (token);
		cur->network = g_strdup (network);

		return cur;
	}

	return NULL;
}

static sasl_info*
get_info (void)
{
	const char* name;
	name = poxchat_get_info (ph, "network");

	if (name)
	{
		return find_info (name);
	}
	else
	{
		return NULL;
	}
}

static int
authend_cb (char *word[], char *word_eol[], void *userdata)
{
	if (get_info ())
	{
		/* omit cryptic server message parts */
		poxchat_printf (ph, "%s\t%s\n", name, ++word_eol[4]);
		poxchat_commandf (ph, "QUOTE CAP END");
	}

	return POXCHAT_EAT_ALL;
}

/*
static int
disconnect_cb (char *word[], void *userdata)
{
	poxchat_printf (ph, "disconnected\n");
	return POXCHAT_EAT_NONE;
}
*/

static int
server_cb (char *word[], char *word_eol[], void *userdata)
{
	size_t len;
	char* buf;
	char* enc;
	sasl_info* p;

	if (strcmp ("AUTHENTICATE", word[1]) == 0 && strcmp ("+", word[2]) == 0)
	{
		p = get_info ();

		if (!p)
		{
			return POXCHAT_EAT_NONE;
		}

		poxchat_printf (ph, "%s\tAuthenticating as %s\n", name, p->login);

		len = strlen (p->login) * 2 + 2 + strlen (p->password);
		buf = (char*) malloc (len + 1);
		strcpy (buf, p->login);
		strcpy (buf + strlen (p->login) + 1, p->login);
		strcpy (buf + strlen (p->login) * 2 + 2, p->password);
		enc = g_base64_encode ((unsigned char*) buf, len);

		/* poxchat_printf (ph, "AUTHENTICATE %s\}", enc); */
		poxchat_commandf (ph, "QUOTE AUTHENTICATE %s", enc);

		free (enc);
		free (buf);

		return POXCHAT_EAT_ALL;
	}

	return POXCHAT_EAT_NONE;
}

static int
cap_cb (char *word[], char *word_eol[], void *userdata)
{
	if (get_info ())
	{
		/* FIXME test sasl cap */
		/* this is visible in the rawlog in case someone needs it, otherwise it's just noise */
		/* poxchat_printf (ph, "%s\t%s\n", name, word_eol[1]); */
		poxchat_commandf (ph, "QUOTE AUTHENTICATE PLAIN");
	}

	return POXCHAT_EAT_ALL;
}

static int
sasl_cmd_cb (char *word[], char *word_eol[], void *userdata)
{
	const char* login;
	const char* password;
	const char* network;
	const char* mode = word[2];

	if (!g_ascii_strcasecmp ("ADD", mode))
	{
		login = word[3];
		password = word[4];
		network = word_eol[5];

		if (!network || !*network)	/* only check for the last word, if it's there, the previous ones will be there, too */
		{
			poxchat_printf (ph, "%s", sasl_help);
			return POXCHAT_EAT_ALL;
		}

		if (add_info (login, password, network))
		{
			poxchat_printf (ph, "%s\tEnabled SASL authentication for the \"%s\" network\n", name, network);
		}
		else
		{
			poxchat_printf (ph, "%s\tFailed to enable SASL authentication for the \"%s\" network\n", name, network);
		}

		return POXCHAT_EAT_ALL;
	}
	else if (!g_ascii_strcasecmp ("DEL", mode))
	{
		network = word_eol[3];

		if (!network || !*network)
		{
			poxchat_printf (ph, "%s", sasl_help);
			return POXCHAT_EAT_ALL;
		}

		if (del_info (network))
		{
			poxchat_printf (ph, "%s\tDisabled SASL authentication for the \"%s\" network\n", name, network);
		}
		else
		{
			poxchat_printf (ph, "%s\tFailed to disable SASL authentication for the \"%s\" network\n", name, network);
		}

		return POXCHAT_EAT_ALL;
	}
	else if (!g_ascii_strcasecmp ("LIST", mode))
	{
		print_info ();
		return POXCHAT_EAT_ALL;
	}
	else
	{
		poxchat_printf (ph, "%s", sasl_help);
		return POXCHAT_EAT_ALL;
	}
}

static int
connect_cb (char *word[], void *userdata)
{
	if (get_info ())
	{
		poxchat_printf (ph, "%s\tSASL enabled\n", name);
		poxchat_commandf (ph, "QUOTE CAP REQ :sasl");
	}

	return POXCHAT_EAT_NONE;
}

int
poxchat_plugin_init (poxchat_plugin *plugin_handle, char **plugin_name, char **plugin_desc, char **plugin_version, char *arg)
{
	/* we need to save this for use with any poxchat_* functions */
	ph = plugin_handle;

	/* tell PoxChat our info */
	*plugin_name = name;
	*plugin_desc = desc;
	*plugin_version = version;

	poxchat_hook_command (ph, "SASL", POXCHAT_PRI_NORM, sasl_cmd_cb, sasl_help, 0);
	poxchat_hook_print (ph, "Connected", POXCHAT_PRI_NORM, connect_cb, NULL);
	/* poxchat_hook_print (ph, "Disconnected", POXCHAT_PRI_NORM, disconnect_cb, NULL); */
	poxchat_hook_server (ph, "CAP", POXCHAT_PRI_NORM, cap_cb, NULL);
	poxchat_hook_server (ph, "RAW LINE", POXCHAT_PRI_NORM, server_cb, NULL);
	poxchat_hook_server (ph, "903", POXCHAT_PRI_NORM, authend_cb, NULL);
	poxchat_hook_server (ph, "904", POXCHAT_PRI_NORM, authend_cb, NULL);
	poxchat_hook_server (ph, "905", POXCHAT_PRI_NORM, authend_cb, NULL);
	poxchat_hook_server (ph, "906", POXCHAT_PRI_NORM, authend_cb, NULL);
	poxchat_hook_server (ph, "907", POXCHAT_PRI_NORM, authend_cb, NULL);

	poxchat_toastf (ph, POXCHAT_TOAST_INFO, "%s plugin loaded", name);

	return 1;
}

int
poxchat_plugin_deinit (void)
{
	poxchat_toastf (ph, POXCHAT_TOAST_INFO, "%s plugin unloaded", name);
	return 1;
}
