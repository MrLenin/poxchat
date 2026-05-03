/* X-Chat
 * Copyright (C) 2004-2008 Peter Zelezny.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <gdk/gdkkeysyms.h>

#include "../common/hexchat.h"
#include "../common/hexchatc.h"
#include "../common/servlist.h"
#include "../common/cfgfiles.h"
#include "../common/fe.h"
#include "../common/util.h"
#ifdef USE_LIBWEBSOCKETS
#include "../common/oauth.h"
#endif

#include "fe-gtk.h"
#include "gtkutil.h"
#include "gtk-helpers.h"
#include "menu.h"
#include "pixmaps.h"
#include "fkeys.h"

#ifdef WIN32
#include <windows.h>
#endif

#define SERVLIST_X_PADDING 4			/* horizontal paddig in the network editor */
#define SERVLIST_Y_PADDING 0			/* vertical padding in the network editor */

#ifdef USE_OPENSSL
# define DEFAULT_SERVER "newserver/6697"
#else
# define DEFAULT_SERVER "newserver/6667"
#endif

/* servlistgui.c globals */
static GtkWidget *serverlist_win = NULL;
static GtkWidget *networks_tree;		/* network TreeView */

static int netlist_win_width = 0;		/* don't hardcode pixels, just use as much as needed by default, save if resized */
static int netlist_win_height = 0;
static int netedit_win_width = 0;
static int netedit_win_height = 0;

static int netedit_active_tab = 0;

/* global user info */
static GtkWidget *entry_nick1;
static GtkWidget *entry_nick2;
static GtkWidget *entry_nick3;
static GtkWidget *entry_guser;

enum {
		SERVER_TREE,
		CHANNEL_TREE,
		CMD_TREE,
		N_TREES,
};

/* edit area */
static GtkWidget *edit_win;
static GtkWidget *edit_entry_nick;
static GtkWidget *edit_entry_nick2;
static GtkWidget *edit_entry_user;
static GtkWidget *edit_entry_real;
static GtkWidget *edit_entry_pass;
static GtkWidget *edit_label_nick;
static GtkWidget *edit_label_nick2;
static GtkWidget *edit_label_real;
static GtkWidget *edit_label_user;
static GtkWidget *edit_trees[N_TREES];

#ifdef USE_LIBWEBSOCKETS
/* OAuth configuration widgets */
static GtkWidget *edit_entry_oauth_authurl;
static GtkWidget *edit_label_oauth_authurl;
static GtkWidget *edit_entry_oauth_tokenurl;
static GtkWidget *edit_label_oauth_tokenurl;
static GtkWidget *edit_entry_oauth_clientid;
static GtkWidget *edit_label_oauth_clientid;
static GtkWidget *edit_entry_oauth_clientsecret;
static GtkWidget *edit_label_oauth_clientsecret;
static GtkWidget *edit_entry_oauth_scopes;
static GtkWidget *edit_label_oauth_scopes;
static GtkWidget *edit_button_oauth_authorize;
#endif

static ircnet *selected_net = NULL;
static ircserver *selected_serv = NULL;
static commandentry *selected_cmd = NULL;
static favchannel *selected_chan = NULL;
static session *servlist_sess;

/* --- GObject item types for GtkColumnView migration --- */

/* Commands tree item */
#define HC_TYPE_SERVLIST_CMD_ITEM (hc_servlist_cmd_item_get_type())
G_DECLARE_FINAL_TYPE (HcServlistCmdItem, hc_servlist_cmd_item, HC, SERVLIST_CMD_ITEM, GObject)

struct _HcServlistCmdItem {
	GObject parent;
	char *command;
	commandentry *entry;
	gboolean start_editing;
};

G_DEFINE_TYPE (HcServlistCmdItem, hc_servlist_cmd_item, G_TYPE_OBJECT)

static void
hc_servlist_cmd_item_finalize (GObject *obj)
{
	HcServlistCmdItem *item = HC_SERVLIST_CMD_ITEM (obj);
	g_free (item->command);
	G_OBJECT_CLASS (hc_servlist_cmd_item_parent_class)->finalize (obj);
}

static void hc_servlist_cmd_item_class_init (HcServlistCmdItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_servlist_cmd_item_finalize; }
static void hc_servlist_cmd_item_init (HcServlistCmdItem *item) { }

static HcServlistCmdItem *
hc_servlist_cmd_item_new (const char *command, commandentry *entry)
{
	HcServlistCmdItem *item = g_object_new (HC_TYPE_SERVLIST_CMD_ITEM, NULL);
	item->command = g_strdup (command ? command : "");
	item->entry = entry;
	return item;
}

static GListStore *cmd_store = NULL;

/* Servers tree item */
#define HC_TYPE_SERVLIST_SERVER_ITEM (hc_servlist_server_item_get_type())
G_DECLARE_FINAL_TYPE (HcServlistServerItem, hc_servlist_server_item, HC, SERVLIST_SERVER_ITEM, GObject)

struct _HcServlistServerItem {
	GObject parent;
	char *hostname;
	ircserver *serv;
	gboolean start_editing;
};

G_DEFINE_TYPE (HcServlistServerItem, hc_servlist_server_item, G_TYPE_OBJECT)

static void
hc_servlist_server_item_finalize (GObject *obj)
{
	HcServlistServerItem *item = HC_SERVLIST_SERVER_ITEM (obj);
	g_free (item->hostname);
	G_OBJECT_CLASS (hc_servlist_server_item_parent_class)->finalize (obj);
}

static void hc_servlist_server_item_class_init (HcServlistServerItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_servlist_server_item_finalize; }
static void hc_servlist_server_item_init (HcServlistServerItem *item) { }

static HcServlistServerItem *
hc_servlist_server_item_new (const char *hostname, ircserver *serv)
{
	HcServlistServerItem *item = g_object_new (HC_TYPE_SERVLIST_SERVER_ITEM, NULL);
	item->hostname = g_strdup (hostname ? hostname : "");
	item->serv = serv;
	return item;
}

static GListStore *server_store = NULL;

/* Channels tree item */
#define HC_TYPE_SERVLIST_CHAN_ITEM (hc_servlist_chan_item_get_type())
G_DECLARE_FINAL_TYPE (HcServlistChanItem, hc_servlist_chan_item, HC, SERVLIST_CHAN_ITEM, GObject)

struct _HcServlistChanItem {
	GObject parent;
	char *name;
	char *key;
	favchannel *fav;
	gboolean start_editing;
};

G_DEFINE_TYPE (HcServlistChanItem, hc_servlist_chan_item, G_TYPE_OBJECT)

static void
hc_servlist_chan_item_finalize (GObject *obj)
{
	HcServlistChanItem *item = HC_SERVLIST_CHAN_ITEM (obj);
	g_free (item->name);
	g_free (item->key);
	G_OBJECT_CLASS (hc_servlist_chan_item_parent_class)->finalize (obj);
}

static void hc_servlist_chan_item_class_init (HcServlistChanItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_servlist_chan_item_finalize; }
static void hc_servlist_chan_item_init (HcServlistChanItem *item) { }

static HcServlistChanItem *
hc_servlist_chan_item_new (const char *name, const char *key, favchannel *fav)
{
	HcServlistChanItem *item = g_object_new (HC_TYPE_SERVLIST_CHAN_ITEM, NULL);
	item->name = g_strdup (name ? name : "");
	item->key = g_strdup (key ? key : "");
	item->fav = fav;
	return item;
}

static GListStore *chan_store = NULL;

/* Networks tree item */
#define HC_TYPE_SERVLIST_NET_ITEM (hc_servlist_net_item_get_type())
G_DECLARE_FINAL_TYPE (HcServlistNetItem, hc_servlist_net_item, HC, SERVLIST_NET_ITEM, GObject)

struct _HcServlistNetItem {
	GObject parent;
	char *name;
	gboolean is_favorite;
	ircnet *net;
	gboolean start_editing;
};

G_DEFINE_TYPE (HcServlistNetItem, hc_servlist_net_item, G_TYPE_OBJECT)

static void
hc_servlist_net_item_finalize (GObject *obj)
{
	HcServlistNetItem *item = HC_SERVLIST_NET_ITEM (obj);
	g_free (item->name);
	G_OBJECT_CLASS (hc_servlist_net_item_parent_class)->finalize (obj);
}

static void hc_servlist_net_item_class_init (HcServlistNetItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_servlist_net_item_finalize; }
static void hc_servlist_net_item_init (HcServlistNetItem *item) { }

static HcServlistNetItem *
hc_servlist_net_item_new (const char *name, gboolean is_favorite, ircnet *net)
{
	HcServlistNetItem *item = g_object_new (HC_TYPE_SERVLIST_NET_ITEM, NULL);
	item->name = g_strdup (name ? name : "");
	item->is_favorite = is_favorite;
	item->net = net;
	return item;
}

static GListStore *net_store = NULL;

static void servlist_network_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data);
static GtkWidget *servlist_open_edit (GtkWidget *parent, ircnet *net);
static GSList *servlist_move_item_store (GListStore *store, GtkSelectionModel *sel_model, GSList *list, gpointer backend, int delta);


static const char *pages[]=
{
	IRC_DEFAULT_CHARSET,
	"CP1252 (Windows-1252)",
	"ISO-8859-15 (Western Europe)",
	"ISO-8859-2 (Central Europe)",
	"ISO-8859-7 (Greek)",
	"ISO-8859-8 (Hebrew)",
	"ISO-8859-9 (Turkish)",
	"ISO-2022-JP (Japanese)",
	"SJIS (Japanese)",
	"CP949 (Korean)",
	"KOI8-R (Cyrillic)",
	"CP1251 (Cyrillic)",
	"CP1256 (Arabic)",
	"CP1257 (Baltic)",
	"GB18030 (Chinese)",
	"TIS-620 (Thai)",
	NULL
};

/* This is our dictionary for authentication types. Keep these in sync with
 * login_types[]! This allows us to re-order the login type dropdown in the
 * network list without breaking config compatibility.
 *
 * Also make sure inbound_nickserv_login() won't break, i.e. if you add a new
 * type that is NickServ-based, add it there as well so that HexChat knows to
 * treat it as such.
 */
static int login_types_conf[] =
{
	LOGIN_DEFAULT,			/* default entry - we don't use this but it makes indexing consistent with login_types[] so it's nice */
	LOGIN_SASL,
#ifdef USE_OPENSSL
	LOGIN_SASLEXTERNAL,
	LOGIN_SASL_SCRAM_SHA_1,
	LOGIN_SASL_SCRAM_SHA_256,
	LOGIN_SASL_SCRAM_SHA_512,
#endif
#ifdef USE_LIBWEBSOCKETS
	LOGIN_SASL_OAUTHBEARER,
#endif
	LOGIN_PASS,
	LOGIN_MSG_NICKSERV,
	LOGIN_NICKSERV,
#ifdef USE_OPENSSL
	LOGIN_CHALLENGEAUTH,
#endif
	LOGIN_CUSTOM
};

static const char *login_types[]=
{
	"Default",
	"SASL PLAIN (username + password)",
#ifdef USE_OPENSSL
	"SASL EXTERNAL (cert)",
	"SASL SCRAM-SHA-1",
	"SASL SCRAM-SHA-256",
	"SASL SCRAM-SHA-512",
#endif
#ifdef USE_LIBWEBSOCKETS
	"SASL OAUTHBEARER (OAuth2/OIDC)",
#endif
	"Server password (/PASS password)",
	"NickServ (/MSG NickServ + password)",
	"NickServ (/NICKSERV + password)",
#ifdef USE_OPENSSL
	"Challenge Auth (username + password)",
#endif
	"Custom... (connect commands)",
	NULL
};

/* poor man's IndexOf() - find the dropdown string index that belongs to the given config value */
static int
servlist_get_login_desc_index (int conf_value)
{
	int i;
	int length = sizeof (login_types_conf) / sizeof (login_types_conf[0]);		/* the number of elements in the conf array */

	for (i = 0; i < length; i++)
	{
		if (login_types_conf[i] == conf_value)
		{
			return i;
		}
	}

	return 0;	/* make the compiler happy */
}

static void
servlist_channels_populate (ircnet *net)
{
	int i;
	favchannel *favchan;
	GSList *list = net->favchanlist;
	GtkSelectionModel *sel_model;

	g_list_store_remove_all (chan_store);

	i = 0;
	while (list)
	{
		favchan = list->data;
		HcServlistChanItem *item = hc_servlist_chan_item_new (favchan->name, favchan->key, favchan);
		g_list_store_append (chan_store, item);
		g_object_unref (item);

		i++;
		list = list->next;
	}

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CHANNEL_TREE]));
	if (net->selected >= 0 && (guint)net->selected < g_list_model_get_n_items (G_LIST_MODEL (chan_store)))
		gtk_selection_model_select_item (sel_model, net->selected, TRUE);
	else if (g_list_model_get_n_items (G_LIST_MODEL (chan_store)) > 0)
		gtk_selection_model_select_item (sel_model, 0, TRUE);
}

static void
servlist_servers_populate (ircnet *net)
{
	int i;
	ircserver *serv;
	GSList *list = net->servlist;
	GtkSelectionModel *sel_model;

	g_list_store_remove_all (server_store);

	i = 0;
	while (list)
	{
		serv = list->data;
		HcServlistServerItem *item = hc_servlist_server_item_new (serv->hostname, serv);
		g_list_store_append (server_store, item);
		g_object_unref (item);

		i++;
		list = list->next;
	}

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[SERVER_TREE]));
	if (net->selected >= 0 && (guint)net->selected < g_list_model_get_n_items (G_LIST_MODEL (server_store)))
		gtk_selection_model_select_item (sel_model, net->selected, TRUE);
	else if (g_list_model_get_n_items (G_LIST_MODEL (server_store)) > 0)
		gtk_selection_model_select_item (sel_model, 0, TRUE);
}

static void
servlist_commands_populate (ircnet *net)
{
	int i;
	commandentry *entry;
	GSList *list = net->commandlist;
	GtkSelectionModel *sel_model;

	g_list_store_remove_all (cmd_store);

	i = 0;
	while (list)
	{
		entry = list->data;
		HcServlistCmdItem *item = hc_servlist_cmd_item_new (entry->command, entry);
		g_list_store_append (cmd_store, item);
		g_object_unref (item);

		i++;
		list = list->next;
	}

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CMD_TREE]));
	if (net->selected >= 0 && (guint)net->selected < g_list_model_get_n_items (G_LIST_MODEL (cmd_store)))
		gtk_selection_model_select_item (sel_model, net->selected, TRUE);
	else if (g_list_model_get_n_items (G_LIST_MODEL (cmd_store)) > 0)
		gtk_selection_model_select_item (sel_model, 0, TRUE);
}

static void
servlist_networks_populate_ (GSList *netlist, gboolean favorites)
{
	GtkSelectionModel *sel_model;
	int i;
	ircnet *net;

	if (!netlist)
	{
		net = servlist_net_add (_("New Network"), "", FALSE);
		servlist_server_add (net, DEFAULT_SERVER);
		netlist = network_list;
	}

	g_list_store_remove_all (net_store);

	i = 0;
	while (netlist)
	{
		net = netlist->data;
		if (!favorites || (net->flags & FLAG_FAVORITE))
		{
			HcServlistNetItem *item = hc_servlist_net_item_new (
				net->name, (net->flags & FLAG_FAVORITE) != 0, net);
			g_list_store_append (net_store, item);
			g_object_unref (item);
		}
		if (i == prefs.hex_gui_slist_select)
		{
			/* Will select after populating - need the store position */
			selected_net = net;
		}
		i++;
		netlist = netlist->next;
	}

	/* Select the remembered network */
	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
	if (selected_net)
	{
		guint n = g_list_model_get_n_items (G_LIST_MODEL (net_store));
		for (guint j = 0; j < n; j++)
		{
			HcServlistNetItem *item = g_list_model_get_item (G_LIST_MODEL (net_store), j);
			if (item && item->net == selected_net)
			{
				gtk_selection_model_select_item (sel_model, j, TRUE);
				g_object_unref (item);
				break;
			}
			if (item) g_object_unref (item);
		}
	}
	else if (g_list_model_get_n_items (G_LIST_MODEL (net_store)) > 0)
	{
		gtk_selection_model_select_item (sel_model, 0, TRUE);
	}
}

static void
servlist_networks_populate (GSList *netlist)
{
	servlist_networks_populate_ (netlist, prefs.hex_gui_slist_fav);
}

static void
servlist_server_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	guint pos;
	HcServlistServerItem *item;

	(void)position; (void)n_items; (void)user_data;

	if (!selected_net)
		return;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (sel_model), pos);
	if (item)
	{
		selected_net->selected = pos;
		selected_serv = item->serv;
		g_object_unref (item);
	}
}

static void
servlist_command_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	guint pos;
	HcServlistCmdItem *item;

	(void)position; (void)n_items; (void)user_data;

	if (!selected_net)
		return;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (sel_model), pos);
	if (item)
	{
		selected_net->selected = pos;
		selected_cmd = item->entry;
		g_object_unref (item);
	}
}

static void
servlist_channel_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	guint pos;
	HcServlistChanItem *item;

	(void)position; (void)n_items; (void)user_data;

	if (!selected_net)
		return;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (sel_model), pos);
	if (item)
	{
		selected_net->selected = pos;
		selected_chan = item->fav;
		g_object_unref (item);
	}
}

static void
servlist_addserver (void)
{
	ircserver *serv;
	HcServlistServerItem *item;
	GtkSelectionModel *sel_model;
	guint pos;

	if (!selected_net)
		return;

	serv = servlist_server_add (selected_net, DEFAULT_SERVER);
	item = hc_servlist_server_item_new (DEFAULT_SERVER, serv);
	item->start_editing = TRUE;
	g_list_store_append (server_store, item);
	g_object_unref (item);

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[SERVER_TREE]));
	pos = g_list_model_get_n_items (G_LIST_MODEL (server_store)) - 1;
	gtk_selection_model_select_item (sel_model, pos, TRUE);
}

static void
servlist_addcommand (void)
{
	commandentry *entry;
	HcServlistCmdItem *item;
	GtkSelectionModel *sel_model;
	guint pos;

	if (!selected_net)
		return;

	entry = servlist_command_add (selected_net, "ECHO hello");
	item = hc_servlist_cmd_item_new ("ECHO hello", entry);
	item->start_editing = TRUE;
	g_list_store_append (cmd_store, item);
	g_object_unref (item);

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CMD_TREE]));
	pos = g_list_model_get_n_items (G_LIST_MODEL (cmd_store)) - 1;
	gtk_selection_model_select_item (sel_model, pos, TRUE);
}

static void
servlist_addchannel (void)
{
	favchannel *fav;
	HcServlistChanItem *item;
	GtkSelectionModel *sel_model;
	guint pos;

	if (!selected_net)
		return;

	servlist_favchan_add (selected_net, "#channel");
	fav = g_slist_last (selected_net->favchanlist)->data;
	item = hc_servlist_chan_item_new ("#channel", "", fav);
	item->start_editing = TRUE;
	g_list_store_append (chan_store, item);
	g_object_unref (item);

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CHANNEL_TREE]));
	pos = g_list_model_get_n_items (G_LIST_MODEL (chan_store)) - 1;
	gtk_selection_model_select_item (sel_model, pos, TRUE);
}

static void
servlist_addnet_cb (GtkWidget *item, gpointer user_data)
{
	ircnet *net;
	HcServlistNetItem *netitem;
	GtkSelectionModel *sel_model;

	net = servlist_net_add (_("New Network"), "", TRUE);
	net->encoding = g_strdup (IRC_DEFAULT_CHARSET);
	servlist_server_add (net, DEFAULT_SERVER);

	netitem = hc_servlist_net_item_new (net->name, FALSE, net);
	netitem->start_editing = TRUE;
	g_list_store_insert (net_store, 0, netitem);
	g_object_unref (netitem);

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
	gtk_selection_model_select_item (sel_model, 0, TRUE);
}

static void
servlist_deletenetwork (ircnet *net)
{
	GtkSelectionModel *sel_model;
	guint n_items, pos;

	/* Find and remove from store */
	n_items = g_list_model_get_n_items (G_LIST_MODEL (net_store));
	for (pos = 0; pos < n_items; pos++)
	{
		HcServlistNetItem *item = g_list_model_get_item (G_LIST_MODEL (net_store), pos);
		if (item && item->net == net)
		{
			g_object_unref (item);
			g_list_store_remove (net_store, pos);
			break;
		}
		if (item) g_object_unref (item);
	}

	/* remove from backend list */
	servlist_net_remove (net);

	/* force something to be selected */
	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
	n_items = g_list_model_get_n_items (G_LIST_MODEL (net_store));
	if (n_items > 0)
		gtk_selection_model_select_item (sel_model, 0, TRUE);

	selected_net = NULL;
	/* Trigger selection callback to update selected_net */
	if (n_items > 0)
	{
		HcServlistNetItem *first = g_list_model_get_item (G_LIST_MODEL (net_store), 0);
		if (first)
		{
			selected_net = first->net;
			g_object_unref (first);
		}
	}
}

static void
servlist_deletenetdialog_cb (GObject *source, GAsyncResult *result, gpointer data)
{
	ircnet *net = data;
	int button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);
	if (button == 1) /* OK */
		servlist_deletenetwork (net);
}

static gboolean
servlist_net_keypress_cb (GtkEventControllerKey *controller, guint keyval,
                          guint keycode, GdkModifierType state, gpointer user_data)
{
	gboolean handled = FALSE;
	(void)controller; (void)keycode; (void)user_data;

	if (!selected_net || prefs.hex_gui_slist_fav)
		return FALSE;

	if (state & STATE_SHIFT)
	{
		if (keyval == GDK_KEY_Up)
		{
			handled = TRUE;
			network_list = servlist_move_item_store (net_store,
				gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree)),
				network_list, selected_net, -1);
		}
		else if (keyval == GDK_KEY_Down)
		{
			handled = TRUE;
			network_list = servlist_move_item_store (net_store,
				gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree)),
				network_list, selected_net, +1);
		}
	}

	return handled;
}

static gint
servlist_compare (ircnet *net1, ircnet *net2)
{
	gchar *net1_casefolded, *net2_casefolded;
	int result=0;

	net1_casefolded=g_utf8_casefold(net1->name,-1),
	net2_casefolded=g_utf8_casefold(net2->name,-1),

	result=g_utf8_collate(net1_casefolded,net2_casefolded);

	g_free(net1_casefolded);
	g_free(net2_casefolded);

	return result;

}

static void
servlist_sort (GtkWidget *button, gpointer none)
{
	network_list = g_slist_sort (network_list, (GCompareFunc)servlist_compare);
	servlist_networks_populate (network_list);
}

static void
servlist_favor (GtkWidget *button, gpointer none)
{
	GtkSelectionModel *sel_model;
	guint pos;
	HcServlistNetItem *item;

	if (!selected_net)
		return;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (net_store), pos);
	if (!item)
		return;

	if (selected_net->flags & FLAG_FAVORITE)
	{
		selected_net->flags &= ~FLAG_FAVORITE;
		item->is_favorite = FALSE;
	}
	else
	{
		selected_net->flags |= FLAG_FAVORITE;
		item->is_favorite = TRUE;
	}

	/* Force rebind to update CSS class */
	g_object_ref (item);
	g_list_store_remove (net_store, pos);
	g_list_store_insert (net_store, pos, item);
	g_object_unref (item);
	g_object_unref (item);
	gtk_selection_model_select_item (sel_model, pos, TRUE);
}

static void
servlist_update_from_entry (char **str, GtkWidget *entry)
{
	g_free (*str);

	if (hc_entry_get_text (entry)[0] == 0)
		*str = NULL;
	else
		*str = g_strdup (hc_entry_get_text (entry));
}

static void
servlist_edit_update (ircnet *net)
{
	servlist_update_from_entry (&net->nick, edit_entry_nick);
	servlist_update_from_entry (&net->nick2, edit_entry_nick2);
	servlist_update_from_entry (&net->user, edit_entry_user);
	servlist_update_from_entry (&net->real, edit_entry_real);
	servlist_update_from_entry (&net->pass, edit_entry_pass);
#ifdef USE_LIBWEBSOCKETS
	servlist_update_from_entry (&net->oauth_client_id, edit_entry_oauth_clientid);
	servlist_update_from_entry (&net->oauth_client_secret, edit_entry_oauth_clientsecret);
	servlist_update_from_entry (&net->oauth_scopes, edit_entry_oauth_scopes);
	servlist_update_from_entry (&net->oauth_authorization_url, edit_entry_oauth_authurl);
	servlist_update_from_entry (&net->oauth_token_url, edit_entry_oauth_tokenurl);
#endif
}

static void
servlist_edit_close_cb (GtkWidget *button, gpointer userdata)
{
	if (selected_net)
		servlist_edit_update (selected_net);

	hc_window_destroy_fn (GTK_WINDOW (edit_win));
	edit_win = NULL;

}

static gboolean
servlist_editwin_delete_cb (GtkWidget *win, gpointer none)
{
	(void)win; (void)none;
	servlist_edit_close_cb (NULL, NULL);
	return FALSE;
}

static void
servlist_configure_cb (GObject *obj, GParamSpec *pspec, gpointer none)
{
	GtkWindow *win = GTK_WINDOW (obj);
	(void)pspec; (void)none;
	/* remember the window size */
	netlist_win_width = gtk_widget_get_width (GTK_WIDGET (win));
	netlist_win_height = gtk_widget_get_height (GTK_WIDGET (win));
}

static void
servlist_edit_configure_cb (GObject *obj, GParamSpec *pspec, gpointer none)
{
	GtkWindow *win = GTK_WINDOW (obj);
	(void)pspec; (void)none;
	/* remember the window size */
	netedit_win_width = gtk_widget_get_width (GTK_WIDGET (win));
	netedit_win_height = gtk_widget_get_height (GTK_WIDGET (win));
}

static void
servlist_edit_cb (GtkWidget *but, gpointer none)
{
	GtkSelectionModel *sel = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
	if (hc_selection_model_get_selected_position (sel) == GTK_INVALID_LIST_POSITION)
		return;

	edit_win = servlist_open_edit (serverlist_win, selected_net);
	gtkutil_set_icon (edit_win);
	servlist_servers_populate (selected_net);
	servlist_channels_populate (selected_net);
	servlist_commands_populate (selected_net);
	g_signal_connect (G_OBJECT (edit_win), "close-request",
						 	G_CALLBACK (servlist_editwin_delete_cb), 0);
	g_signal_connect (G_OBJECT (edit_win), "close-request",
	                  G_CALLBACK (gtkutil_close_request_focus_parent),
	                  serverlist_win);
	g_signal_connect (G_OBJECT (edit_win), "notify::default-width",
							G_CALLBACK (servlist_edit_configure_cb), 0);
	g_signal_connect (G_OBJECT (edit_win), "notify::default-height",
							G_CALLBACK (servlist_edit_configure_cb), 0);
	gtk_window_present (GTK_WINDOW (edit_win));
}

static void
servlist_deletenet_cb (GtkWidget *item, ircnet *net)
{
	{
		GtkSelectionModel *sel = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
		if (hc_selection_model_get_selected_position (sel) == GTK_INVALID_LIST_POSITION)
			return;
	}

	net = selected_net;
	if (!net)
		return;
	{
		GtkAlertDialog *alert;
		const char *buttons[] = { _("_Cancel"), _("_OK"), NULL };

		alert = gtk_alert_dialog_new (_("Really remove network \"%s\" and all its servers?"), net->name);
		gtk_alert_dialog_set_buttons (alert, buttons);
		gtk_alert_dialog_set_cancel_button (alert, 0);
		gtk_alert_dialog_set_default_button (alert, 1);
		gtk_alert_dialog_set_modal (alert, TRUE);

		gtk_alert_dialog_choose (alert, GTK_WINDOW (serverlist_win),
			NULL, servlist_deletenetdialog_cb, net);
		g_object_unref (alert);
	}
}

static void
servlist_start_editing_store (GListStore *store, GtkColumnView *view, guint offset_of_flag)
{
	GtkSelectionModel *sel_model = gtk_column_view_get_model (view);
	guint position = hc_selection_model_get_selected_position (sel_model);

	if (position == GTK_INVALID_LIST_POSITION)
		return;

	GObject *item = g_list_model_get_item (G_LIST_MODEL (store), position);
	if (!item)
		return;

	gboolean *flag = (gboolean *)((char *)item + offset_of_flag);
	*flag = TRUE;
	/* remove + reinsert to force rebind */
	g_object_ref (item);
	g_list_store_remove (store, position);
	g_list_store_insert (store, position, item);
	g_object_unref (item);
	g_object_unref (item);
	gtk_selection_model_select_item (sel_model, position, TRUE);
}

static void
servlist_editbutton_cb (GtkWidget *item, GtkNotebook *notebook)
{
	switch (gtk_notebook_get_current_page (notebook))
	{
		case SERVER_TREE:
			servlist_start_editing_store (server_store,
				GTK_COLUMN_VIEW (edit_trees[SERVER_TREE]),
				G_STRUCT_OFFSET (HcServlistServerItem, start_editing));
			break;
		case CHANNEL_TREE:
			servlist_start_editing_store (chan_store,
				GTK_COLUMN_VIEW (edit_trees[CHANNEL_TREE]),
				G_STRUCT_OFFSET (HcServlistChanItem, start_editing));
			break;
		case CMD_TREE:
			servlist_start_editing_store (cmd_store,
				GTK_COLUMN_VIEW (edit_trees[CMD_TREE]),
				G_STRUCT_OFFSET (HcServlistCmdItem, start_editing));
			break;
	}
}

static void
servlist_deleteserver_cb (void)
{
	GtkSelectionModel *sel_model;
	guint position, n_items;
	HcServlistServerItem *item;

	if (!selected_net)
		return;

	/* don't remove the last server */
	if (g_slist_length (selected_net->servlist) < 2)
		return;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[SERVER_TREE]));
	position = hc_selection_model_get_selected_position (sel_model);
	if (position == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (server_store), position);
	if (!item)
		return;

	servlist_server_remove (selected_net, item->serv);
	g_object_unref (item);
	g_list_store_remove (server_store, position);

	/* select next (or previous if we removed the last) */
	n_items = g_list_model_get_n_items (G_LIST_MODEL (server_store));
	if (n_items > 0)
		gtk_selection_model_select_item (sel_model, position < n_items ? position : n_items - 1, TRUE);
}

static void
servlist_deletecommand_cb (void)
{
	GtkSelectionModel *sel_model;
	guint position, n_items;
	HcServlistCmdItem *item;

	if (!selected_net)
		return;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CMD_TREE]));
	position = hc_selection_model_get_selected_position (sel_model);
	if (position == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (cmd_store), position);
	if (!item)
		return;

	servlist_command_remove (selected_net, item->entry);
	g_object_unref (item);
	g_list_store_remove (cmd_store, position);

	n_items = g_list_model_get_n_items (G_LIST_MODEL (cmd_store));
	if (n_items > 0)
		gtk_selection_model_select_item (sel_model, position < n_items ? position : n_items - 1, TRUE);
}

static void
servlist_deletechannel_cb (void)
{
	GtkSelectionModel *sel_model;
	guint position, n_items;
	HcServlistChanItem *item;

	if (!selected_net)
		return;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CHANNEL_TREE]));
	position = hc_selection_model_get_selected_position (sel_model);
	if (position == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (chan_store), position);
	if (!item)
		return;

	servlist_favchan_remove (selected_net, item->fav);
	g_object_unref (item);
	g_list_store_remove (chan_store, position);

	n_items = g_list_model_get_n_items (G_LIST_MODEL (chan_store));
	if (n_items > 0)
		gtk_selection_model_select_item (sel_model, position < n_items ? position : n_items - 1, TRUE);
}

static void
servlist_network_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	HcServlistNetItem *item;
	guint pos;

	(void)position; (void)n_items; (void)user_data;

	selected_net = NULL;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (net_store), pos);
	if (item)
	{
		selected_net = item->net;
		prefs.hex_gui_slist_select = g_slist_index (network_list, item->net);
		g_object_unref (item);
	}
}

static int
servlist_savegui (void)
{
	char *sp;
	const char *nick1, *nick2;

	/* check for blank username, ircd will not allow this */
	if (hc_entry_get_text (entry_guser)[0] == 0)
		return 1;


	nick1 = hc_entry_get_text (entry_nick1);
	nick2 = hc_entry_get_text (entry_nick2);

	/* ensure unique nicknames */
	if (!rfc_casecmp (nick1, nick2))
		return 2;

	safe_strcpy (prefs.hex_irc_nick1, nick1, sizeof(prefs.hex_irc_nick1));
	safe_strcpy (prefs.hex_irc_nick2, nick2, sizeof(prefs.hex_irc_nick2));
	safe_strcpy (prefs.hex_irc_nick3, hc_entry_get_text (entry_nick3), sizeof(prefs.hex_irc_nick3));
	safe_strcpy (prefs.hex_irc_user_name, hc_entry_get_text (entry_guser), sizeof(prefs.hex_irc_user_name));
	sp = strchr (prefs.hex_irc_user_name, ' ');
	if (sp)
		sp[0] = 0;	/* spaces will break the login */
	servlist_save ();
	save_config (); /* For nicks stored in hexchat.conf */

	return 0;
}

static void
servlist_addbutton_cb (GtkWidget *item, GtkNotebook *notebook)
{
		switch (gtk_notebook_get_current_page (notebook))
		{
				case SERVER_TREE:
						servlist_addserver ();
						break;
				case CHANNEL_TREE:
						servlist_addchannel ();
						break;
				case CMD_TREE:
						servlist_addcommand ();
						break;
				default:
						break;
		}
}

static void
servlist_deletebutton_cb (GtkWidget *item, GtkNotebook *notebook)
{
		switch (gtk_notebook_get_current_page (notebook))
		{
				case SERVER_TREE:
						servlist_deleteserver_cb ();
						break;
				case CHANNEL_TREE:
						servlist_deletechannel_cb ();
						break;
				case CMD_TREE:
						servlist_deletecommand_cb ();
						break;
				default:
						break;
		}
}

static GSList *
servlist_move_item_store (GListStore *store, GtkSelectionModel *sel_model,
                          GSList *list, gpointer backend, int delta)
{
	guint position, n_items, new_pos;
	int slist_pos;
	GObject *item;

	position = hc_selection_model_get_selected_position (sel_model);
	if (position == GTK_INVALID_LIST_POSITION)
		return list;

	n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
	if (delta == -1 && position > 0)
		new_pos = position - 1;
	else if (delta == 1 && position < n_items - 1)
		new_pos = position + 1;
	else
		return list;

	item = g_list_model_get_item (G_LIST_MODEL (store), position);
	g_list_store_remove (store, position);
	g_list_store_insert (store, new_pos, item);
	g_object_unref (item);

	slist_pos = g_slist_index (list, backend);
	if (slist_pos >= 0)
	{
		list = g_slist_remove (list, backend);
		list = g_slist_insert (list, backend, slist_pos + delta);
	}
	gtk_selection_model_select_item (sel_model, new_pos, TRUE);
	return list;
}

static gboolean
servlist_keypress_cb (GtkEventControllerKey *controller, guint keyval, guint keycode,
                      GdkModifierType state, GtkNotebook *notebook)
{
	gboolean handled = FALSE;
	int delta = 0;

	(void)controller; (void)keycode;

	if (!selected_net)
		return FALSE;

	if (state & STATE_SHIFT)
	{
		if (keyval == GDK_KEY_Up)
		{
			handled = TRUE;
			delta = -1;
		}
		else if (keyval == GDK_KEY_Down)
		{
			handled = TRUE;
			delta = +1;
		}
	}

	if (handled)
	{
		switch (gtk_notebook_get_current_page (notebook))
		{
			case SERVER_TREE:
				if (selected_serv)
					selected_net->servlist = servlist_move_item_store (server_store,
						gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[SERVER_TREE])),
						selected_net->servlist, selected_serv, delta);
				break;
			case CHANNEL_TREE:
				if (selected_chan)
					selected_net->favchanlist = servlist_move_item_store (chan_store,
						gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CHANNEL_TREE])),
						selected_net->favchanlist, selected_chan, delta);
				break;
			case CMD_TREE:
				if (selected_cmd)
					selected_net->commandlist = servlist_move_item_store (cmd_store,
						gtk_column_view_get_model (GTK_COLUMN_VIEW (edit_trees[CMD_TREE])),
						selected_net->commandlist, selected_cmd, delta);
				break;
		}
	}

	return handled;
}

void
servlist_autojoinedit (ircnet *net, char *channel, gboolean add)
{
	favchannel *fav;

	if (add)
	{
		servlist_favchan_add (net, channel);
		servlist_save ();
	}
	else
	{
		fav = servlist_favchan_find (net, channel, NULL);
		if (fav)
		{
			servlist_favchan_remove (net, fav);
			servlist_save ();
		}
	}
}

static void
servlist_toggle_global_user (gboolean sensitive)
{
	gtk_widget_set_sensitive (edit_entry_nick, sensitive);
	gtk_widget_set_sensitive (edit_label_nick, sensitive);

	gtk_widget_set_sensitive (edit_entry_nick2, sensitive);
	gtk_widget_set_sensitive (edit_label_nick2, sensitive);

	gtk_widget_set_sensitive (edit_entry_user, sensitive);
	gtk_widget_set_sensitive (edit_label_user, sensitive);

	gtk_widget_set_sensitive (edit_entry_real, sensitive);
	gtk_widget_set_sensitive (edit_label_real, sensitive);
}

static void
servlist_connect_cb (GtkWidget *button, gpointer userdata)
{
	int servlist_err;

	if (!selected_net)
		return;

	servlist_err = servlist_savegui ();
	if (servlist_err == 1)
	{
		fe_message (_("User name cannot be left blank."), FE_MSG_ERROR);
		return;
	}

 	if (!is_session (servlist_sess))
		servlist_sess = NULL;	/* open a new one */

	/* Prefer the tab the user was on when opening the dialog, if it's
	 * disconnected.  Only fall back to searching for another disconnected
	 * session when the current one is already connected. */
	if (servlist_sess && !servlist_sess->server->connected)
	{
		/* Current tab is idle - reuse it */
	}
	else
	{
		servlist_sess = NULL;	/* will open a new tab */
	}

	servlist_connect (servlist_sess, selected_net, TRUE);

	hc_window_destroy_fn (GTK_WINDOW (serverlist_win));
	serverlist_win = NULL;
	selected_net = NULL;
}

static void
servlist_check_cb (GtkWidget *but, gpointer num_p)
{
	int num = GPOINTER_TO_INT (num_p);

	if (!selected_net)
		return;

	if ((1 << num) == FLAG_CYCLE || (1 << num) == FLAG_USE_PROXY)
	{
		/* these ones are reversed, so it's compat with 2.0.x */
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (but)))
			selected_net->flags &= ~(1 << num);
		else
			selected_net->flags |= (1 << num);
	} else
	{
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (but)))
			selected_net->flags |= (1 << num);
		else
			selected_net->flags &= ~(1 << num);
	}

	if ((1 << num) == FLAG_USE_GLOBAL)
	{
		servlist_toggle_global_user (!gtk_check_button_get_active (GTK_CHECK_BUTTON (but)));
	}
}

static GtkWidget *
servlist_create_check (int num, int state, GtkWidget *table, int row, int col, char *labeltext)
{
	GtkWidget *but;

	but = gtk_check_button_new_with_label (labeltext);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (but), state);
	g_signal_connect (G_OBJECT (but), "toggled",
							G_CALLBACK (servlist_check_cb), GINT_TO_POINTER (num));
	gtk_widget_set_hexpand (but, TRUE);
	gtk_widget_set_margin_start (but, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (but, SERVLIST_X_PADDING);
	gtk_widget_set_margin_top (but, SERVLIST_Y_PADDING);
	gtk_widget_set_margin_bottom (but, SERVLIST_Y_PADDING);
	gtk_grid_attach (GTK_GRID (table), but, col, row, 2, 1);

	return but;
}

static GtkWidget *
servlist_create_entry (GtkWidget *table, char *labeltext, int row,
							  char *def, GtkWidget **label_ret, char *tip)
{
	GtkWidget *label, *entry;

	label = gtk_label_new_with_mnemonic (labeltext);
	if (label_ret)
		*label_ret = label;
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (label, SERVLIST_X_PADDING);
	gtk_widget_set_margin_top (label, SERVLIST_Y_PADDING);
	gtk_widget_set_margin_bottom (label, SERVLIST_Y_PADDING);
	gtk_grid_attach (GTK_GRID (table), label, 0, row, 1, 1);

	entry = gtk_entry_new ();
	gtk_widget_set_tooltip_text (entry, tip);
	hc_entry_set_text (entry, def ? def : "");
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	gtk_widget_set_hexpand (entry, TRUE);
	gtk_widget_set_margin_start (entry, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (entry, SERVLIST_X_PADDING);
	gtk_widget_set_margin_top (entry, SERVLIST_Y_PADDING);
	gtk_widget_set_margin_bottom (entry, SERVLIST_Y_PADDING);
	gtk_grid_attach (GTK_GRID (table), entry, 1, row, 1, 1);

	return entry;
}

static gboolean
servlist_delete_cb (GtkWidget *win, gpointer userdata)
{
	(void)win; (void)userdata;
	servlist_savegui ();
	serverlist_win = NULL;
	selected_net = NULL;

	if (sess_list == NULL)
		hexchat_exit ();

	return FALSE;
}

static void
servlist_close_cb (GtkWidget *button, gpointer userdata)
{
	GtkWidget *win;

	(void)button; (void)userdata;
	/* hc_window_destroy_fn uses gtk_window_destroy which does NOT fire
	 * "close-request", so servlist_delete_cb wouldn't run.  Do the
	 * cleanup and exit check here before destroying. */
	servlist_savegui ();
	win = serverlist_win;
	serverlist_win = NULL;
	selected_net = NULL;
	hc_window_destroy_fn (GTK_WINDOW (win));

	if (sess_list == NULL)
		hexchat_exit ();
}

/* convert "host:port" format to "host/port" */

static char *
servlist_sanitize_hostname (char *host)
{
	char *ret, *c, *e;

	ret = g_strdup (host);

	c = strchr  (ret, ':');
	e = strrchr (ret, ':');

	/* if only one colon exists it's probably not IPv6 */
	if (c && c == e)
		*c = '/';

	return g_strstrip(ret);
}

/* remove leading slash */
static char *
servlist_sanitize_command (char *cmd)
{
	if (cmd[0] == '/')
	{
		return (g_strdup (cmd + 1));
	}
	else
	{
		return (g_strdup (cmd));
	}
}

/* servlist_editserver_cb, servlist_editcommand_cb, servlist_editchannel_cb,
 * servlist_editkey_cb: replaced by notify::text in GtkColumnView factory bind callbacks */

static gboolean
servlist_edit_tabswitch_cb (GtkNotebook *nb, gpointer *newtab, guint newindex, gpointer user_data)
{
	/* remember the active tab */
	netedit_active_tab = newindex;

	return FALSE;
}

#ifdef USE_LIBWEBSOCKETS
/* Helper to show error/info messages parented to the edit dialog */
static void
servlist_edit_message (const char *msg)
{
	GtkAlertDialog *dialog;
	GtkWindow *parent = edit_win ? GTK_WINDOW (edit_win) : GTK_WINDOW (serverlist_win);

	dialog = gtk_alert_dialog_new ("%s", msg);
	gtk_alert_dialog_set_modal (dialog, TRUE);
	gtk_alert_dialog_show (dialog, parent);
	g_object_unref (dialog);
}

/* OAuth authorization completion callback */
static void
servlist_oauth_complete_cb (struct server *serv, oauth_token *token, const char *error)
{
	ircnet *net = (ircnet *)serv; /* user_data was passed as the network */
	(void)serv; /* serv is actually the network pointer passed as user_data */

	if (error)
	{
		servlist_edit_message (error);
		return;
	}

	if (!net || !token)
		return;

	/* Save token to secure storage (Credential Manager, keychain, etc.) */
	if (oauth_save_tokens (net->name, token))
	{
		/* Keep a copy in memory for immediate use */
		g_free (net->oauth_access_token);
		net->oauth_access_token = g_strdup (token->access_token);
		g_free (net->oauth_refresh_token);
		net->oauth_refresh_token = g_strdup (token->refresh_token);
		net->oauth_token_expires = token->expires_at;

		servlist_edit_message (_("OAuth authorization successful! Token saved securely."));
	}
	else
	{
		/* Secure storage not available, keep in memory and config file */
		g_free (net->oauth_access_token);
		net->oauth_access_token = g_strdup (token->access_token);
		g_free (net->oauth_refresh_token);
		net->oauth_refresh_token = g_strdup (token->refresh_token);
		net->oauth_token_expires = token->expires_at;

		servlist_edit_message (_("OAuth authorization successful! Token saved to config."));
	}
}

/* Handle Authorize button click */
static void
servlist_oauth_authorize_cb (GtkWidget *button, gpointer userdata)
{
	const char *client_id, *client_secret, *scopes, *auth_url, *token_url;

	if (!selected_net)
		return;

	/* Get values from entry fields */
	client_id = hc_entry_get_text (edit_entry_oauth_clientid);
	client_secret = hc_entry_get_text (edit_entry_oauth_clientsecret);
	scopes = hc_entry_get_text (edit_entry_oauth_scopes);
	auth_url = hc_entry_get_text (edit_entry_oauth_authurl);
	token_url = hc_entry_get_text (edit_entry_oauth_tokenurl);

	/* Validate required fields */
	if (!client_id || !*client_id)
	{
		servlist_edit_message (_("Client ID is required"));
		return;
	}
	if (!auth_url || !*auth_url)
	{
		servlist_edit_message (_("Authorization URL is required"));
		return;
	}
	if (!token_url || !*token_url)
	{
		servlist_edit_message (_("Token URL is required"));
		return;
	}

	/* Save values to the network before starting authorization */
	g_free (selected_net->oauth_client_id);
	selected_net->oauth_client_id = g_strdup (client_id);
	g_free (selected_net->oauth_client_secret);
	selected_net->oauth_client_secret = g_strdup (client_secret);
	g_free (selected_net->oauth_scopes);
	selected_net->oauth_scopes = g_strdup (scopes);
	g_free (selected_net->oauth_authorization_url);
	selected_net->oauth_authorization_url = g_strdup (auth_url);
	g_free (selected_net->oauth_token_url);
	selected_net->oauth_token_url = g_strdup (token_url);

	/* Start OAuth authorization flow */
	oauth_begin_authorization (selected_net, servlist_oauth_complete_cb, selected_net);
}
#endif

/* Fills up the network's authentication type so that it's guaranteed to be either NULL or a valid value. */
static void
servlist_logintypecombo_cb (GObject *obj, GParamSpec *pspec, gpointer userdata)
{
	guint sel;

	if (!selected_net)
	{
		return;
	}

	sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (obj));

	if (sel == GTK_INVALID_LIST_POSITION)
		return; /* Invalid */

	int index = (int)sel;

	/* The selection is valid. It can be 0, which is the default type, but we need to allow
	 * that so that you can revert from other types. servlist_save() will dump 0 anyway.
	 */
	selected_net->logintype = login_types_conf[index];

	if (login_types_conf[index] == LOGIN_CUSTOM)
	{
		gtk_notebook_set_current_page (GTK_NOTEBOOK (userdata), 2);		/* FIXME avoid hardcoding? */
	}
	
	/* EXTERNAL uses a cert, not a pass */
	if (login_types_conf[index] == LOGIN_SASLEXTERNAL)
	{
		gtk_widget_set_sensitive (edit_entry_pass, FALSE);
		gtk_entry_set_placeholder_text (GTK_ENTRY (edit_entry_pass), NULL);
	}
#ifdef USE_LIBWEBSOCKETS
	/* OAUTHBEARER uses tokens, not passwords */
	else if (login_types_conf[index] == LOGIN_SASL_OAUTHBEARER)
	{
		gtk_widget_set_sensitive (edit_entry_pass, FALSE);
		gtk_entry_set_placeholder_text (GTK_ENTRY (edit_entry_pass),
			_("Token managed by OAuth2 tab"));
	}
#endif
	else
	{
		gtk_widget_set_sensitive (edit_entry_pass, TRUE);
		gtk_entry_set_placeholder_text (GTK_ENTRY (edit_entry_pass), NULL);
	}

#ifdef USE_LIBWEBSOCKETS
	/* Enable/disable OAuth authorize button based on login type */
	gtk_widget_set_sensitive (edit_button_oauth_authorize,
		login_types_conf[index] == LOGIN_SASL_OAUTHBEARER);
#endif
}

static void
servlist_username_changed_cb (GtkEntry *entry, gpointer userdata)
{
	GtkWidget *connect_btn = GTK_WIDGET (userdata);

	if (hc_entry_get_text (GTK_WIDGET (entry))[0] == 0)
	{
		gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, "dialog-error");
		gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY,
										_("User name cannot be left blank."));
		gtk_widget_set_sensitive (connect_btn, FALSE);
	}
	else
	{
		gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, NULL);
		gtk_widget_set_sensitive (connect_btn, TRUE);
	}
}

static void
servlist_nick_changed_cb (GtkEntry *entry, gpointer userdata)
{
	GtkWidget *connect_btn = GTK_WIDGET (userdata);
	const gchar *nick1 = hc_entry_get_text (entry_nick1);
	const gchar *nick2 = hc_entry_get_text (entry_nick2);

	if (!nick1[0] || !nick2[0])
	{
		entry = GTK_ENTRY(!nick1[0] ? entry_nick1 : entry_nick2);
		gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, "dialog-error");
		gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY,
		                                 _("You cannot have an empty nick name."));
		gtk_widget_set_sensitive (connect_btn, FALSE);
	}
	else if (!rfc_casecmp (nick1, nick2))
	{
		gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, "dialog-error");
		gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY,
										_("You must have two unique nick names."));
		gtk_widget_set_sensitive (connect_btn, FALSE);
	}
	else
	{
		gtk_entry_set_icon_from_icon_name (GTK_ENTRY(entry_nick1), GTK_ENTRY_ICON_SECONDARY, NULL);
		gtk_entry_set_icon_from_icon_name (GTK_ENTRY(entry_nick2), GTK_ENTRY_ICON_SECONDARY, NULL);
		gtk_widget_set_sensitive (connect_btn, TRUE);
	}
}

static GtkWidget *
servlist_create_charsetcombo (void)
{
	GtkStringList *model = gtk_string_list_new (pages);
	GtkWidget *cb = gtk_drop_down_new (G_LIST_MODEL (model), NULL);
	gtk_drop_down_set_enable_search (GTK_DROP_DOWN (cb), TRUE);

	return cb;
}

static GtkWidget *
servlist_create_logintypecombo (GtkWidget *data)
{
	GtkStringList *model = gtk_string_list_new (login_types);
	GtkWidget *cb = gtk_drop_down_new (G_LIST_MODEL (model), NULL);

	gtk_drop_down_set_selected (GTK_DROP_DOWN (cb),
		servlist_get_login_desc_index (selected_net->logintype));

	gtk_widget_set_tooltip_text (cb, _("The way you identify yourself to the server. For custom login methods use connect commands."));
	g_signal_connect (cb, "notify::selected", G_CALLBACK (servlist_logintypecombo_cb), data);

	return cb;
}

static void
no_servlist (GtkWidget * igad, gpointer serv)
{
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (igad)))
		prefs.hex_gui_slist_skip = TRUE;
	else
		prefs.hex_gui_slist_skip = FALSE;
}

static void
fav_servlist (GtkWidget * igad, gpointer serv)
{
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (igad)))
		prefs.hex_gui_slist_fav = TRUE;
	else
		prefs.hex_gui_slist_fav = FALSE;

	servlist_networks_populate (network_list);
}

static GtkWidget *
bold_label (char *text)
{
	char buf[128];
	GtkWidget *label;

	g_snprintf (buf, sizeof (buf), "<b>%s</b>", text);
	label = gtk_label_new (buf);
	gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

	return label;
}

static GtkEditableLabel *servlist_editing_label = NULL;

/* --- GtkColumnView factory callbacks for Server tree --- */

static void
servlist_server_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &servlist_editing_label);
	gtk_list_item_set_child (list_item, label);
}

static void
servlist_server_text_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcServlistServerItem *item = gtk_list_item_get_item (list_item);
	const char *new_text;
	char *sanitized;

	if (!item || !selected_net)
		return;

	/* Only update backend when the user is actually editing */
	if (!gtk_editable_label_get_editing (label))
		return;

	new_text = gtk_editable_get_text (GTK_EDITABLE (label));
	sanitized = servlist_sanitize_hostname ((char *)new_text);
	g_free (item->hostname);
	item->hostname = sanitized;
	g_free (item->serv->hostname);
	item->serv->hostname = g_strdup (sanitized);
}

static void
servlist_server_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcServlistServerItem *item = gtk_list_item_get_item (list_item);

	gtk_editable_set_text (GTK_EDITABLE (label), item->hostname ? item->hostname : "");
	g_signal_connect (label, "notify::text", G_CALLBACK (servlist_server_text_changed_cb), list_item);

	if (item->start_editing)
	{
		item->start_editing = FALSE;
		g_idle_add (hc_editable_label_start_idle, label);
	}
}

static void
servlist_server_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, servlist_server_text_changed_cb, list_item);
}

/* --- GtkColumnView factory callbacks for Command tree --- */

static void
servlist_cmd_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &servlist_editing_label);
	gtk_list_item_set_child (list_item, label);
}

static void
servlist_cmd_text_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcServlistCmdItem *item = gtk_list_item_get_item (list_item);
	const char *new_text;
	char *sanitized;

	if (!item || !selected_net)
		return;

	if (!gtk_editable_label_get_editing (label))
		return;

	new_text = gtk_editable_get_text (GTK_EDITABLE (label));
	sanitized = servlist_sanitize_command ((char *)new_text);
	g_free (item->command);
	item->command = sanitized;
	g_free (item->entry->command);
	item->entry->command = g_strdup (sanitized);
}

static void
servlist_cmd_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcServlistCmdItem *item = gtk_list_item_get_item (list_item);

	gtk_editable_set_text (GTK_EDITABLE (label), item->command ? item->command : "");
	g_signal_connect (label, "notify::text", G_CALLBACK (servlist_cmd_text_changed_cb), list_item);

	if (item->start_editing)
	{
		item->start_editing = FALSE;
		g_idle_add (hc_editable_label_start_idle, label);
	}
}

static void
servlist_cmd_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, servlist_cmd_text_changed_cb, list_item);
}

/* --- GtkColumnView factory callbacks for Channel tree (name column) --- */

static void
servlist_chan_name_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &servlist_editing_label);
	gtk_list_item_set_child (list_item, label);
}

static void
servlist_chan_name_text_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcServlistChanItem *item = gtk_list_item_get_item (list_item);
	const char *new_text;

	if (!item || !selected_net)
		return;

	if (!gtk_editable_label_get_editing (label))
		return;

	new_text = gtk_editable_get_text (GTK_EDITABLE (label));
	g_free (item->name);
	item->name = g_strdup (new_text ? new_text : "");
	g_free (item->fav->name);
	item->fav->name = g_strdup (item->name);
}

static void
servlist_chan_name_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcServlistChanItem *item = gtk_list_item_get_item (list_item);

	gtk_editable_set_text (GTK_EDITABLE (label), item->name ? item->name : "");
	g_signal_connect (label, "notify::text", G_CALLBACK (servlist_chan_name_text_changed_cb), list_item);

	if (item->start_editing)
	{
		item->start_editing = FALSE;
		g_idle_add (hc_editable_label_start_idle, label);
	}
}

static void
servlist_chan_name_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, servlist_chan_name_text_changed_cb, list_item);
}

/* --- GtkColumnView factory callbacks for Channel tree (key column) --- */

static void
servlist_chan_key_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &servlist_editing_label);
	gtk_list_item_set_child (list_item, label);
}

static void
servlist_chan_key_text_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcServlistChanItem *item = gtk_list_item_get_item (list_item);
	const char *new_text;

	if (!item || !selected_net)
		return;

	if (!gtk_editable_label_get_editing (label))
		return;

	new_text = gtk_editable_get_text (GTK_EDITABLE (label));
	g_free (item->key);
	if (new_text && new_text[0])
		item->key = g_strdup (new_text);
	else
		item->key = NULL;
	g_free (item->fav->key);
	item->fav->key = item->key ? g_strdup (item->key) : NULL;
}

static void
servlist_chan_key_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcServlistChanItem *item = gtk_list_item_get_item (list_item);

	gtk_editable_set_text (GTK_EDITABLE (label), item->key ? item->key : "");
	g_signal_connect (label, "notify::text", G_CALLBACK (servlist_chan_key_text_changed_cb), list_item);
}

static void
servlist_chan_key_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, servlist_chan_key_text_changed_cb, list_item);
}

/* --- GtkColumnView factory callbacks for Networks tree --- */

static void
servlist_net_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &servlist_editing_label);
	gtk_widget_set_name (label, "hexchat-editable");
	gtk_list_item_set_child (list_item, label);
}

static void
servlist_net_text_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcServlistNetItem *item = gtk_list_item_get_item (list_item);
	const char *new_text;

	if (!item)
		return;

	if (!gtk_editable_label_get_editing (label))
		return;

	new_text = gtk_editable_get_text (GTK_EDITABLE (label));
	if (!new_text || new_text[0] == 0)
		return;

	g_free (item->net->name);
	item->net->name = g_strdup (new_text);
	g_free (item->name);
	item->name = g_strdup (new_text);
}

static void
servlist_net_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcServlistNetItem *item = gtk_list_item_get_item (list_item);

	gtk_editable_set_text (GTK_EDITABLE (label), item->name ? item->name : "");

	if (item->is_favorite)
		gtk_widget_add_css_class (label, "favorite");
	else
		gtk_widget_remove_css_class (label, "favorite");

	g_signal_connect (label, "notify::text", G_CALLBACK (servlist_net_text_changed_cb), list_item);

	if (item->start_editing)
	{
		item->start_editing = FALSE;
		g_idle_add (hc_editable_label_start_idle, label);
	}
}

static void
servlist_net_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, servlist_net_text_changed_cb, list_item);
}

static GtkWidget *
servlist_open_edit (GtkWidget *parent, ircnet *net)
{
	GtkWidget *editwindow;
	GtkWidget *vbox5;
	GtkWidget *table3;
	GtkWidget *label34;
	GtkWidget *label_logintype;
	GtkWidget *comboboxentry_charset;
	GtkWidget *combobox_logintypes;
	GtkWidget *hbox1;
	GtkWidget *scrolledwindow2;
	GtkWidget *scrolledwindow4;
	GtkWidget *scrolledwindow5;
	GtkWidget *treeview_servers;
	GtkWidget *treeview_channels;
	GtkWidget *treeview_commands;
	GtkWidget *vbuttonbox1;
	GtkWidget *buttonadd;
	GtkWidget *buttonremove;
	GtkWidget *buttonedit;
	GtkWidget *hseparator2;
	GtkWidget *hbuttonbox4;
	GtkWidget *button10;
	GtkWidget *check;
	GtkWidget *notebook;
	char buf[128];

	if (fe_get_application ())
		editwindow = GTK_WIDGET (gtk_application_window_new (fe_get_application ()));
	else
		editwindow = gtk_window_new ();
	g_snprintf (buf, sizeof (buf), _("Edit %s - %s"), net->name, _(DISPLAY_NAME));
	gtk_window_set_title (GTK_WINDOW (editwindow), buf);
	gtk_window_set_default_size (GTK_WINDOW (editwindow), netedit_win_width, netedit_win_height);
	gtk_window_set_transient_for (GTK_WINDOW (editwindow), GTK_WINDOW (parent));
	gtk_window_set_modal (GTK_WINDOW (editwindow), TRUE);

	vbox5 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	hc_widget_set_margin_all (vbox5, 12);
	gtk_window_set_child (GTK_WINDOW (editwindow), vbox5);


	/* Tabs and buttons */
	hbox1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand (hbox1, TRUE);
	gtk_box_append (GTK_BOX (vbox5), hbox1);

	scrolledwindow2 = gtk_scrolled_window_new ();
	scrolledwindow4 = gtk_scrolled_window_new ();
	scrolledwindow5 = gtk_scrolled_window_new ();

	notebook = gtk_notebook_new ();
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), scrolledwindow2, gtk_label_new (_("Servers")));
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), scrolledwindow4, gtk_label_new (_("Autojoin channels")));
	gtk_notebook_append_page (GTK_NOTEBOOK (notebook), scrolledwindow5, gtk_label_new (_("Connect commands")));

#ifdef USE_LIBWEBSOCKETS
	/* OAuth2 configuration tab */
	{
		GtkWidget *oauth_grid = gtk_grid_new ();
		gtk_grid_set_row_spacing (GTK_GRID (oauth_grid), 2);
		gtk_grid_set_column_spacing (GTK_GRID (oauth_grid), 8);
		hc_widget_set_margin_all (oauth_grid, 4);

		edit_entry_oauth_clientid = servlist_create_entry (oauth_grid, _("Client ID:"), 0,
			net->oauth_client_id, &edit_label_oauth_clientid,
			_("OAuth2 Client ID from your provider"));

		edit_entry_oauth_clientsecret = servlist_create_entry (oauth_grid, _("Client Secret:"), 1,
			net->oauth_client_secret, &edit_label_oauth_clientsecret,
			_("OAuth2 Client Secret (optional for public clients)"));
		gtk_entry_set_visibility (GTK_ENTRY (edit_entry_oauth_clientsecret), FALSE);

		edit_entry_oauth_scopes = servlist_create_entry (oauth_grid, _("Scopes:"), 2,
			net->oauth_scopes ? net->oauth_scopes : "openid",
			&edit_label_oauth_scopes,
			_("OAuth2 scopes to request (e.g., openid)"));

		edit_entry_oauth_authurl = servlist_create_entry (oauth_grid, _("Authorization URL:"), 3,
			net->oauth_authorization_url, &edit_label_oauth_authurl,
			_("OAuth2 authorization endpoint URL"));

		edit_entry_oauth_tokenurl = servlist_create_entry (oauth_grid, _("Token URL:"), 4,
			net->oauth_token_url, &edit_label_oauth_tokenurl,
			_("OAuth2 token endpoint URL"));

		/* Authorize button */
		edit_button_oauth_authorize = gtk_button_new_with_mnemonic (_("_Authorize..."));
		gtk_widget_set_margin_start (edit_button_oauth_authorize, 4);
		gtk_widget_set_margin_top (edit_button_oauth_authorize, 8);
		gtk_widget_set_margin_bottom (edit_button_oauth_authorize, 4);
		gtk_widget_set_tooltip_text (edit_button_oauth_authorize,
			_("Open browser to authorize with OAuth2 provider. "
			  "Requires SASL OAUTHBEARER login method."));
		gtk_grid_attach (GTK_GRID (oauth_grid), edit_button_oauth_authorize, 1, 5, 1, 1);
		g_signal_connect (G_OBJECT (edit_button_oauth_authorize), "clicked",
			G_CALLBACK (servlist_oauth_authorize_cb), NULL);

		/* Disable authorize button unless OAUTHBEARER is selected */
		if (!selected_net || selected_net->logintype != LOGIN_SASL_OAUTHBEARER)
			gtk_widget_set_sensitive (edit_button_oauth_authorize, FALSE);

		gtk_notebook_append_page (GTK_NOTEBOOK (notebook), oauth_grid, gtk_label_new (_("OAuth2")));
	}
#endif

	gtk_notebook_set_tab_pos (GTK_NOTEBOOK (notebook), GTK_POS_BOTTOM);
	gtk_widget_set_hexpand (notebook, TRUE);
	gtk_widget_set_margin_start (notebook, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (notebook, SERVLIST_X_PADDING);
	gtk_box_append (GTK_BOX (hbox1), notebook);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow2), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow4), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow5), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_tooltip_text (scrolledwindow5, _("%n=Nick name\n%p=Password\n%r=Real name\n%u=User name"));


	/* Server Tree (GtkColumnView) */
	{
		GtkColumnViewColumn *col;
		GtkSelectionModel *sel_model;

		server_store = g_list_store_new (HC_TYPE_SERVLIST_SERVER_ITEM);
		edit_trees[SERVER_TREE] = treeview_servers = hc_column_view_new_simple (G_LIST_MODEL (server_store), GTK_SELECTION_SINGLE);
		gtk_column_view_set_header_factory (GTK_COLUMN_VIEW (treeview_servers), NULL);
		hc_column_view_hide_headers (GTK_COLUMN_VIEW (treeview_servers));

		col = hc_column_view_add_column (GTK_COLUMN_VIEW (treeview_servers), NULL,
									G_CALLBACK (servlist_server_setup_cb),
									G_CALLBACK (servlist_server_bind_cb),
									G_CALLBACK (servlist_server_unbind_cb),
									NULL);
		gtk_column_view_column_set_expand (col, TRUE);

		hc_add_key_controller (treeview_servers, G_CALLBACK (servlist_keypress_cb), NULL, notebook);
		sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (treeview_servers));
		g_signal_connect (sel_model, "selection-changed", G_CALLBACK (servlist_server_row_cb), NULL);
gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolledwindow2), treeview_servers);
		gtk_widget_set_size_request (treeview_servers, -1, 80);
	}

	/* Channel Tree (GtkColumnView, two columns) */
	{
		GtkColumnViewColumn *col;
		GtkSelectionModel *sel_model;

		chan_store = g_list_store_new (HC_TYPE_SERVLIST_CHAN_ITEM);
		edit_trees[CHANNEL_TREE] = treeview_channels = hc_column_view_new_simple (G_LIST_MODEL (chan_store), GTK_SELECTION_SINGLE);

		/* Name column */
		col = hc_column_view_add_column (GTK_COLUMN_VIEW (treeview_channels), _("Channel"),
									G_CALLBACK (servlist_chan_name_setup_cb),
									G_CALLBACK (servlist_chan_name_bind_cb),
									G_CALLBACK (servlist_chan_name_unbind_cb),
									NULL);
		gtk_column_view_column_set_expand (col, TRUE);

		/* Key column */
		col = hc_column_view_add_column (GTK_COLUMN_VIEW (treeview_channels), _("Key (Password)"),
									G_CALLBACK (servlist_chan_key_setup_cb),
									G_CALLBACK (servlist_chan_key_bind_cb),
									G_CALLBACK (servlist_chan_key_unbind_cb),
									NULL);
		gtk_column_view_column_set_expand (col, TRUE);

		hc_add_key_controller (treeview_channels, G_CALLBACK (servlist_keypress_cb), NULL, notebook);
		sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (treeview_channels));
		g_signal_connect (sel_model, "selection-changed", G_CALLBACK (servlist_channel_row_cb), NULL);
gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolledwindow4), treeview_channels);
	}

	/* Command Tree (GtkColumnView) */
	{
		GtkColumnViewColumn *col;
		GtkSelectionModel *sel_model;

		cmd_store = g_list_store_new (HC_TYPE_SERVLIST_CMD_ITEM);
		edit_trees[CMD_TREE] = treeview_commands = hc_column_view_new_simple (G_LIST_MODEL (cmd_store), GTK_SELECTION_SINGLE);
		gtk_column_view_set_header_factory (GTK_COLUMN_VIEW (treeview_commands), NULL);
		hc_column_view_hide_headers (GTK_COLUMN_VIEW (treeview_commands));

		col = hc_column_view_add_column (GTK_COLUMN_VIEW (treeview_commands), NULL,
									G_CALLBACK (servlist_cmd_setup_cb),
									G_CALLBACK (servlist_cmd_bind_cb),
									G_CALLBACK (servlist_cmd_unbind_cb),
									NULL);
		gtk_column_view_column_set_expand (col, TRUE);

		hc_add_key_controller (treeview_commands, G_CALLBACK (servlist_keypress_cb), NULL, notebook);
		sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (treeview_commands));
		g_signal_connect (sel_model, "selection-changed", G_CALLBACK (servlist_command_row_cb), NULL);
gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolledwindow5), treeview_commands);
	}


	/* Button Box */
	vbuttonbox1 = hc_button_box_new_impl (GTK_ORIENTATION_VERTICAL);
	gtk_box_set_spacing (GTK_BOX (vbuttonbox1), 3);
	hc_button_box_set_layout_impl (vbuttonbox1, HC_BUTTONBOX_START);
	gtk_widget_set_margin_start (vbuttonbox1, 3);
	gtk_widget_set_margin_end (vbuttonbox1, 3);
	gtk_box_append (GTK_BOX (hbox1), vbuttonbox1);

	buttonadd = gtk_button_new_with_mnemonic (_("_Add"));
	g_signal_connect (G_OBJECT (buttonadd), "clicked",
							G_CALLBACK (servlist_addbutton_cb), notebook);
	gtk_box_append (GTK_BOX (vbuttonbox1), buttonadd);

	buttonremove = gtk_button_new_with_mnemonic (_("_Remove"));
	g_signal_connect (G_OBJECT (buttonremove), "clicked",
							G_CALLBACK (servlist_deletebutton_cb), notebook);
	gtk_box_append (GTK_BOX (vbuttonbox1), buttonremove);

	buttonedit = gtk_button_new_with_mnemonic (_("_Edit"));
	g_signal_connect (G_OBJECT (buttonedit), "clicked",
							G_CALLBACK (servlist_editbutton_cb), notebook);
	gtk_box_append (GTK_BOX (vbuttonbox1), buttonedit);


	/* Checkboxes and entries */
	table3 = gtk_grid_new ();
	gtk_box_append (GTK_BOX (vbox5), table3);
	gtk_grid_set_row_spacing (GTK_GRID (table3), 2);
	gtk_grid_set_column_spacing (GTK_GRID (table3), 8);

	check = servlist_create_check (0, !(net->flags & FLAG_CYCLE), table3, 0, 0, _("Connect to selected server only"));
	gtk_widget_set_tooltip_text (check, _("Don't cycle through all the servers when the connection fails."));
	servlist_create_check (3, net->flags & FLAG_AUTO_CONNECT, table3, 1, 0, _("Connect to this network automatically"));
	servlist_create_check (4, !(net->flags & FLAG_USE_PROXY), table3, 2, 0, _("Bypass proxy server"));
	check = servlist_create_check (2, net->flags & FLAG_USE_SSL, table3, 3, 0, _("Use SSL for all the servers on this network"));
#ifndef USE_OPENSSL
	gtk_widget_set_sensitive (check, FALSE);
#endif
	check = servlist_create_check (5, net->flags & FLAG_ALLOW_INVALID, table3, 4, 0, _("Accept invalid SSL certificates"));
#ifndef USE_OPENSSL
	gtk_widget_set_sensitive (check, FALSE);
#endif
	check = servlist_create_check (7, net->flags & FLAG_PERSISTENT, table3, 5, 0, _("Persistent server (bouncer keeps channel state)"));
	gtk_widget_set_tooltip_text (check, _("Don't auto-rejoin channels on reconnect — the server (bouncer) restores them itself."));
	servlist_create_check (1, net->flags & FLAG_USE_GLOBAL, table3, 6, 0, _("Use global user information"));

	edit_entry_nick = servlist_create_entry (table3, _("_Nick name:"), 7, net->nick, &edit_label_nick, 0);
	edit_entry_nick2 = servlist_create_entry (table3, _("Second choice:"), 8, net->nick2, &edit_label_nick2, 0);
	edit_entry_real = servlist_create_entry (table3, _("Rea_l name:"), 9, net->real, &edit_label_real, 0);
	edit_entry_user = servlist_create_entry (table3, _("_User name:"), 10, net->user, &edit_label_user, 0);

	label_logintype = gtk_label_new (_("Login method:"));
	gtk_widget_set_halign (label_logintype, GTK_ALIGN_START);
	gtk_widget_set_valign (label_logintype, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label_logintype, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (label_logintype, SERVLIST_X_PADDING);
	gtk_widget_set_margin_top (label_logintype, SERVLIST_Y_PADDING);
	gtk_widget_set_margin_bottom (label_logintype, SERVLIST_Y_PADDING);
	gtk_grid_attach (GTK_GRID (table3), label_logintype, 0, 11, 1, 1);
	combobox_logintypes = servlist_create_logintypecombo (notebook);
	gtk_widget_set_margin_start (combobox_logintypes, 4);
	gtk_widget_set_margin_end (combobox_logintypes, 4);
	gtk_widget_set_margin_top (combobox_logintypes, 2);
	gtk_widget_set_margin_bottom (combobox_logintypes, 2);
	gtk_grid_attach (GTK_GRID (table3), combobox_logintypes, 1, 11, 1, 1);

	edit_entry_pass = servlist_create_entry (table3, _("Password:"), 12, net->pass, 0, _("Password used for login. If in doubt, leave blank."));
	gtk_entry_set_visibility (GTK_ENTRY (edit_entry_pass), FALSE);
	if (selected_net && selected_net->logintype == LOGIN_SASLEXTERNAL)
		gtk_widget_set_sensitive (edit_entry_pass, FALSE);
#ifdef USE_LIBWEBSOCKETS
	if (selected_net && selected_net->logintype == LOGIN_SASL_OAUTHBEARER)
	{
		gtk_widget_set_sensitive (edit_entry_pass, FALSE);
		gtk_entry_set_placeholder_text (GTK_ENTRY (edit_entry_pass),
			_("Token managed by OAuth2 tab"));
	}
#endif

	label34 = gtk_label_new (_("Character set:"));
	gtk_widget_set_halign (label34, GTK_ALIGN_START);
	gtk_widget_set_valign (label34, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label34, SERVLIST_X_PADDING);
	gtk_widget_set_margin_end (label34, SERVLIST_X_PADDING);
	gtk_widget_set_margin_top (label34, SERVLIST_Y_PADDING);
	gtk_widget_set_margin_bottom (label34, SERVLIST_Y_PADDING);
	gtk_grid_attach (GTK_GRID (table3), label34, 0, 13, 1, 1);
	comboboxentry_charset = servlist_create_charsetcombo ();
	gtk_widget_set_margin_start (comboboxentry_charset, 4);
	gtk_widget_set_margin_end (comboboxentry_charset, 4);
	gtk_widget_set_margin_top (comboboxentry_charset, 2);
	gtk_widget_set_margin_bottom (comboboxentry_charset, 2);
	gtk_grid_attach (GTK_GRID (table3), comboboxentry_charset, 1, 13, 1, 1);


	/* Rule and Close button */
	hseparator2 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top (hseparator2, 12);
	gtk_widget_set_margin_bottom (hseparator2, 12);
	gtk_box_append (GTK_BOX (vbox5), hseparator2);

	hbuttonbox4 = hc_button_box_new_impl (GTK_ORIENTATION_HORIZONTAL);
	gtk_box_append (GTK_BOX (vbox5), hbuttonbox4);

	button10 = gtk_button_new_with_mnemonic (_("_Close"));
	g_signal_connect (G_OBJECT (button10), "clicked",
							G_CALLBACK (servlist_edit_close_cb), 0);
	gtk_box_append (GTK_BOX (hbuttonbox4), button10);

	if (net->flags & FLAG_USE_GLOBAL)
	{
		servlist_toggle_global_user (FALSE);
	}

	gtk_widget_grab_focus (button10);

	/* We can't set the active tab without child elements being shown, so this must be *after* gtk_widget_show()s! */
	gtk_notebook_set_current_page (GTK_NOTEBOOK (notebook), netedit_active_tab);

	/* We need to connect this *after* setting the active tab so that the value doesn't get overriden. */
	g_signal_connect (G_OBJECT (notebook), "switch-page", G_CALLBACK (servlist_edit_tabswitch_cb), notebook);

	return editwindow;
}

static GtkWidget *
servlist_open_networks (void)
{
	GtkWidget *servlist;
	GtkWidget *vbox1;
	GtkWidget *label2;
	GtkWidget *table1;
	GtkWidget *label3;
	GtkWidget *label4;
	GtkWidget *label5;
	GtkWidget *label6;
	GtkWidget *entry1;
	GtkWidget *entry2;
	GtkWidget *entry3;
	GtkWidget *entry4;
	/* GtkWidget *entry5; */
	GtkWidget *vbox2;
	GtkWidget *label1;
	GtkWidget *table4;
	GtkWidget *scrolledwindow3;
	GtkWidget *treeview_networks;
	GtkWidget *checkbutton_skip;
	GtkWidget *checkbutton_fav;
	GtkWidget *hbox;
	GtkWidget *vbuttonbox2;
	GtkWidget *button_add;
	GtkWidget *button_remove;
	GtkWidget *button_edit;
	extern GtkWidget *parent_window;
	GtkWidget *button_sort;
	GtkWidget *hseparator1;
	GtkWidget *hbuttonbox1;
	GtkWidget *button_connect;
	GtkWidget *button_close;
	char buf[128];

	if (fe_get_application ())
		servlist = GTK_WIDGET (gtk_application_window_new (fe_get_application ()));
	else
		servlist = gtk_window_new ();
	g_snprintf(buf, sizeof(buf), _("Network List - %s"), _(DISPLAY_NAME));
	gtk_window_set_title (GTK_WINDOW (servlist), buf);
	gtk_window_set_default_size (GTK_WINDOW (servlist), netlist_win_width, netlist_win_height);
	if (current_sess)
	{
		gtk_window_set_transient_for (GTK_WINDOW (servlist), GTK_WINDOW (current_sess->gui->window));
		g_signal_connect (servlist, "close-request",
		                  G_CALLBACK (gtkutil_close_request_focus_parent),
		                  current_sess->gui->window);
	}
	else if (parent_window)
	{
		gtk_window_set_transient_for (GTK_WINDOW (servlist), GTK_WINDOW (parent_window));
		g_signal_connect (servlist, "close-request",
		                  G_CALLBACK (gtkutil_close_request_focus_parent),
		                  parent_window);
	}

	vbox1 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	hc_widget_set_margin_all (vbox1, 12);
	gtk_window_set_child (GTK_WINDOW (servlist), vbox1);

	label2 = bold_label (_("User Information"));
	gtk_box_append (GTK_BOX (vbox1), label2);

	table1 = gtk_grid_new ();
	gtk_box_append (GTK_BOX (vbox1), table1);
	gtk_grid_set_row_spacing (GTK_GRID (table1), 2);
	gtk_grid_set_column_spacing (GTK_GRID (table1), 4);

	label3 = gtk_label_new_with_mnemonic (_("_Nick name:"));
	gtk_widget_set_halign (label3, GTK_ALIGN_START);
	gtk_widget_set_valign (label3, GTK_ALIGN_CENTER);
	gtk_grid_attach (GTK_GRID (table1), label3, 0, 0, 1, 1);

	label4 = gtk_label_new (_("Second choice:"));
	gtk_widget_set_halign (label4, GTK_ALIGN_START);
	gtk_widget_set_valign (label4, GTK_ALIGN_CENTER);
	gtk_grid_attach (GTK_GRID (table1), label4, 0, 1, 1, 1);

	label5 = gtk_label_new (_("Third choice:"));
	gtk_widget_set_halign (label5, GTK_ALIGN_START);
	gtk_widget_set_valign (label5, GTK_ALIGN_CENTER);
	gtk_grid_attach (GTK_GRID (table1), label5, 0, 2, 1, 1);

	label6 = gtk_label_new_with_mnemonic (_("_User name:"));
	gtk_widget_set_halign (label6, GTK_ALIGN_START);
	gtk_widget_set_valign (label6, GTK_ALIGN_CENTER);
	gtk_grid_attach (GTK_GRID (table1), label6, 0, 3, 1, 1);


	entry_nick1 = entry1 = gtk_entry_new ();
	hc_entry_set_text (entry1, prefs.hex_irc_nick1);
	gtk_widget_set_hexpand (entry1, TRUE);
	gtk_grid_attach (GTK_GRID (table1), entry1, 1, 0, 1, 1);

	entry_nick2 = entry2 = gtk_entry_new ();
	hc_entry_set_text (entry2, prefs.hex_irc_nick2);
	gtk_widget_set_hexpand (entry2, TRUE);
	gtk_grid_attach (GTK_GRID (table1), entry2, 1, 1, 1, 1);

	entry_nick3 = entry3 = gtk_entry_new ();
	hc_entry_set_text (entry3, prefs.hex_irc_nick3);
	gtk_widget_set_hexpand (entry3, TRUE);
	gtk_grid_attach (GTK_GRID (table1), entry3, 1, 2, 1, 1);

	entry_guser = entry4 = gtk_entry_new ();
	hc_entry_set_text (entry4, prefs.hex_irc_user_name);
	gtk_widget_set_hexpand (entry4, TRUE);
	gtk_grid_attach (GTK_GRID (table1), entry4, 1, 3, 1, 1);


	vbox2 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand (vbox2, TRUE);
	gtk_box_append (GTK_BOX (vbox1), vbox2);

	label1 = bold_label (_("Networks"));
	gtk_box_append (GTK_BOX (vbox2), label1);

	table4 = gtk_grid_new ();
	gtk_widget_set_vexpand (table4, TRUE);
	gtk_box_append (GTK_BOX (vbox2), table4);
	hc_widget_set_margin_all (table4, 8);
	gtk_grid_set_row_spacing (GTK_GRID (table4), 2);
	gtk_grid_set_column_spacing (GTK_GRID (table4), 3);

	scrolledwindow3 = gtk_scrolled_window_new ();
	gtk_widget_set_hexpand (scrolledwindow3, TRUE);
	gtk_widget_set_vexpand (scrolledwindow3, TRUE);
	gtk_grid_attach (GTK_GRID (table4), scrolledwindow3, 0, 0, 1, 1);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow3),
											  GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	/* Networks Tree (GtkColumnView) */
	{
		GtkColumnViewColumn *col;

		net_store = g_list_store_new (HC_TYPE_SERVLIST_NET_ITEM);
		networks_tree = treeview_networks = hc_column_view_new_simple (G_LIST_MODEL (net_store), GTK_SELECTION_SINGLE);
		gtk_column_view_set_header_factory (GTK_COLUMN_VIEW (treeview_networks), NULL);
		hc_column_view_hide_headers (GTK_COLUMN_VIEW (treeview_networks));

		col = hc_column_view_add_column (GTK_COLUMN_VIEW (treeview_networks), NULL,
									G_CALLBACK (servlist_net_setup_cb),
									G_CALLBACK (servlist_net_bind_cb),
									G_CALLBACK (servlist_net_unbind_cb),
									NULL);
		gtk_column_view_column_set_expand (col, TRUE);

		gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolledwindow3), treeview_networks);
	}

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, FALSE);
	gtk_grid_attach (GTK_GRID (table4), hbox, 0, 1, 2, 1);

	checkbutton_skip =
		gtk_check_button_new_with_mnemonic (_("Skip network list on startup"));
	gtk_check_button_set_active (GTK_CHECK_BUTTON (checkbutton_skip),
											prefs.hex_gui_slist_skip);
	gtk_box_append (GTK_BOX (hbox), checkbutton_skip);
	g_signal_connect (G_OBJECT (checkbutton_skip), "toggled",
							G_CALLBACK (no_servlist), 0);

	checkbutton_fav =
		gtk_check_button_new_with_mnemonic (_("Show favorites only"));
	gtk_check_button_set_active (GTK_CHECK_BUTTON (checkbutton_fav),
											prefs.hex_gui_slist_fav);
	gtk_box_append (GTK_BOX (hbox), checkbutton_fav);
	g_signal_connect (G_OBJECT (checkbutton_fav), "toggled",
							G_CALLBACK (fav_servlist), 0);

	vbuttonbox2 = hc_button_box_new_impl (GTK_ORIENTATION_VERTICAL);
	gtk_box_set_spacing (GTK_BOX (vbuttonbox2), 3);
	hc_button_box_set_layout_impl (vbuttonbox2, HC_BUTTONBOX_START);
	gtk_grid_attach (GTK_GRID (table4), vbuttonbox2, 1, 0, 1, 1);

	button_add = gtk_button_new_with_mnemonic (_("_Add"));
	g_signal_connect (G_OBJECT (button_add), "clicked",
							G_CALLBACK (servlist_addnet_cb), NULL);
	gtk_box_append (GTK_BOX (vbuttonbox2), button_add);

	button_remove = gtk_button_new_with_mnemonic (_("_Remove"));
	g_signal_connect (G_OBJECT (button_remove), "clicked",
							G_CALLBACK (servlist_deletenet_cb), 0);
	gtk_box_append (GTK_BOX (vbuttonbox2), button_remove);

	button_edit = gtk_button_new_with_mnemonic (_("_Edit..."));
	g_signal_connect (G_OBJECT (button_edit), "clicked",
							G_CALLBACK (servlist_edit_cb), 0);
	gtk_box_append (GTK_BOX (vbuttonbox2), button_edit);

	button_sort = gtk_button_new_with_mnemonic (_("_Sort"));
	gtk_widget_set_tooltip_text (button_sort, _("Sorts the network list in alphabetical order. "
				"Use Shift+Up and Shift+Down keys to move a row."));
	g_signal_connect (G_OBJECT (button_sort), "clicked",
							G_CALLBACK (servlist_sort), 0);
	gtk_box_append (GTK_BOX (vbuttonbox2), button_sort);

	button_sort = gtk_button_new_with_mnemonic (_("_Favor"));
	gtk_widget_set_tooltip_text (button_sort, _("Mark or unmark this network as a favorite."));
	g_signal_connect (G_OBJECT (button_sort), "clicked",
							G_CALLBACK (servlist_favor), 0);
	gtk_box_append (GTK_BOX (vbuttonbox2), button_sort);

	hseparator1 = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top (hseparator1, 12);
	gtk_widget_set_margin_bottom (hseparator1, 12);
	gtk_box_append (GTK_BOX (vbox1), hseparator1);

	hbuttonbox1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append (GTK_BOX (vbox1), hbuttonbox1);

	button_close = gtk_button_new_with_mnemonic (_("_Close"));
	g_signal_connect (G_OBJECT (button_close), "clicked",
							G_CALLBACK (servlist_close_cb), 0);
	gtk_box_append (GTK_BOX (hbuttonbox1), button_close);

	/* GTK4: Add spacer to push Connect button to the right */
	{
		GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_hexpand (spacer, TRUE);
		gtk_box_append (GTK_BOX (hbuttonbox1), spacer);
	}

	button_connect = gtkutil_button (hbuttonbox1, "network-transmit-receive", NULL,
												servlist_connect_cb, NULL, _("C_onnect"));

	g_signal_connect (G_OBJECT (entry_guser), "changed", 
					G_CALLBACK(servlist_username_changed_cb), button_connect);
	g_signal_connect (G_OBJECT (entry_nick1), "changed",
					G_CALLBACK(servlist_nick_changed_cb), button_connect);
	g_signal_connect (G_OBJECT (entry_nick2), "changed",
					G_CALLBACK(servlist_nick_changed_cb), button_connect);

	/* Run validity checks now */
	servlist_nick_changed_cb (GTK_ENTRY(entry_nick2), button_connect);
	servlist_username_changed_cb (GTK_ENTRY(entry_guser), button_connect);

	gtk_label_set_mnemonic_widget (GTK_LABEL (label3), entry1);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label6), entry4);

	gtk_widget_grab_focus (networks_tree);
	return servlist;
}

void
fe_serverlist_open (session *sess)
{
	if (serverlist_win)
	{
		gtk_window_present (GTK_WINDOW (serverlist_win));
		return;
	}

	servlist_sess = sess;

	serverlist_win = servlist_open_networks ();
	gtkutil_set_icon (serverlist_win);

	servlist_networks_populate (network_list);

	g_signal_connect (G_OBJECT (serverlist_win), "close-request",
						 	G_CALLBACK (servlist_delete_cb), 0);
	g_signal_connect (G_OBJECT (serverlist_win), "notify::default-width",
							G_CALLBACK (servlist_configure_cb), 0);
	g_signal_connect (G_OBJECT (serverlist_win), "notify::default-height",
							G_CALLBACK (servlist_configure_cb), 0);
	{
		GtkSelectionModel *sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (networks_tree));
		g_signal_connect (sel_model, "selection-changed", G_CALLBACK (servlist_network_row_cb), NULL);
	}
	hc_add_key_controller (networks_tree, G_CALLBACK (servlist_net_keypress_cb), NULL, NULL);

	gtk_window_present (GTK_WINDOW (serverlist_win));
}
