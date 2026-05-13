/* X-Chat
 * Copyright (C) 2004-2007 Peter Zelezny.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../common/poxchat.h"
#include "../common/cfgfiles.h"
#include "../common/fe.h"
#include "../common/text.h"
#include "../common/userlist.h"
#include "../common/util.h"
#include "../common/poxchatc.h"
#include "../common/outbound.h"
#include "fe-gtk.h"
#include "hex-input-edit.h"
#include "gtkutil.h"
#include "gtk-helpers.h"
#include "maingui.h"
#include "palette.h"
#include "pixmaps.h"
#include "userlistgui.h"
#include "menu.h"
#include "plugin-tray.h"
#include "notifications/notification-backend.h"

#ifdef WIN32
#include "../common/fe.h"
#endif
#include "xtext.h"

InputStyle *create_input_style (InputStyle *);
void apply_tree_css (void);
extern InputStyle *input_style;

#define LABEL_INDENT 12

static GtkWidget *setup_window = NULL;
static int last_selected_page = 0;
static int last_selected_row = 0; /* sound row */
static gboolean color_change;
static gboolean setup_applying = FALSE; /* Guard against callbacks during apply/destroy */
static struct poxchatprefs setup_prefs;
static GtkWidget *cancel_button;
static GCancellable *font_dialog_cancel = NULL;

enum
{
	ST_END,
	ST_TOGGLE,
	ST_TOGGLR,
	ST_3OGGLE,
	ST_ENTRY,
	ST_EFONT,
	ST_EFILE,
	ST_EFOLDER,
	ST_MENU,
	ST_RADIO,
	ST_NUMBER,
	ST_HSCALE,
	ST_HEADER,
	ST_LABEL,
	ST_ALERTHEAD
};

typedef struct
{
	int type;
	char *label;
	int offset;
	char *tooltip;
	char const *const *list;
	int extra;
} setting;

#ifdef WIN32
static const char *const langsmenu[] =
{
	N_("Afrikaans"),
	N_("Albanian"),
	N_("Amharic"),
	N_("Asturian"),
	N_("Azerbaijani"),
	N_("Basque"),
	N_("Belarusian"),
	N_("Bulgarian"),
	N_("Catalan"),
	N_("Chinese (Simplified)"),
	N_("Chinese (Traditional)"),
	N_("Czech"),
	N_("Danish"),
	N_("Dutch"),
	N_("English (British)"),
	N_("English"),
	N_("Estonian"),
	N_("Finnish"),
	N_("French"),
	N_("Galician"),
	N_("German"),
	N_("Greek"),
	N_("Gujarati"),
	N_("Hindi"),
	N_("Hungarian"),
	N_("Indonesian"),
	N_("Italian"),
	N_("Japanese"),
	N_("Kannada"),
	N_("Kinyarwanda"),
	N_("Korean"),
	N_("Latvian"),
	N_("Lithuanian"),
	N_("Macedonian"),
	N_("Malay"),
	N_("Malayalam"),
	N_("Norwegian (Bokmal)"),
	N_("Norwegian (Nynorsk)"),
	N_("Polish"),
	N_("Portuguese"),
	N_("Portuguese (Brazilian)"),
	N_("Punjabi"),
	N_("Russian"),
	N_("Serbian"),
	N_("Slovak"),
	N_("Slovenian"),
	N_("Spanish"),
	N_("Swedish"),
	N_("Thai"),
	N_("Turkish"),
	N_("Ukrainian"),
	N_("Vietnamese"),
	N_("Walloon"),
	NULL
};
#endif

static const setting appearance_settings[] =
{
	{ST_HEADER,	N_("General"),0,0,0},
#ifdef WIN32
	{ST_MENU,   N_("Language:"), P_OFFINTNL(hex_gui_lang), 0, langsmenu, 0},
#endif
	{ST_EFONT,  N_("Main font:"), P_OFFSETNL(hex_text_font_main), 0, 0, sizeof prefs.hex_text_font_main},

	{ST_HEADER,	N_("Text Box"),0,0,0},
	{ST_TOGGLE, N_("Colored nick names"), P_OFFINTNL(hex_text_color_nicks), N_("Give each person on IRC a different color"),0,0},
	{ST_TOGGLR, N_("Indent nick names"), P_OFFINTNL(hex_text_indent), N_("Make nick names right-justified"),0,0},
	{ST_TOGGLE, N_ ("Show marker line"), P_OFFINTNL (hex_text_show_marker), N_ ("Insert a red line after the last read text."), 0, 0},
	{ST_TOGGLR, N_("Show day separators"), P_OFFINTNL(hex_gui_day_separator), N_("Show a separator line between messages from different days."), 0, 0},
	{ST_TOGGLE, N_("Collapse long multiline messages"), P_OFFINTNL(hex_gui_collapse_multiline), N_("Auto-collapse multiline messages that exceed the threshold."), 0, 0},
	{ST_NUMBER, N_("Collapse threshold (lines)"), P_OFFINTNL(hex_gui_collapse_threshold), 0, 0, 100},
	{ST_EFILE, N_ ("Background image:"), P_OFFSETNL (hex_text_background), 0, 0, sizeof prefs.hex_text_background},

	{ST_HEADER, N_("Transparency Settings"), 0,0,0},
	{ST_HSCALE, N_("Window opacity:"), P_OFFINTNL(hex_gui_transparency),0,0,0},

	{ST_HEADER,	N_("Timestamps"),0,0,0},
	{ST_TOGGLE, N_("Enable timestamps"), P_OFFINTNL(hex_stamp_text),0,0,1},
	{ST_ENTRY,  N_("Timestamp format:"), P_OFFSETNL(hex_stamp_text_format),
#ifdef WIN32
					N_("See the strftime MSDN article for details."),0,sizeof prefs.hex_stamp_text_format},
#else
					N_("See the strftime manpage for details."),0,sizeof prefs.hex_stamp_text_format},
#endif

	{ST_HEADER,	N_("Title Bar"),0,0,0},
	{ST_TOGGLE, N_("Show channel modes"), P_OFFINTNL(hex_gui_win_modes),0,0,0},
	{ST_TOGGLR, N_("Show number of users"), P_OFFINTNL(hex_gui_win_ucount),0,0,0},
	{ST_TOGGLE, N_("Show nickname"), P_OFFINTNL(hex_gui_win_nick),0,0,0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const tabcompmenu[] = 
{
	N_("A-Z"),
	N_("Last-spoke order"),
	NULL
};

static const setting inputbox_settings[] =
{
	{ST_HEADER, N_("Input Box"),0,0,0},
	{ST_TOGGLE, N_("Use the text box font and colors"), P_OFFINTNL(hex_gui_input_style),0,0,0},
	{ST_TOGGLE, N_("Render colors and attributes"), P_OFFINTNL (hex_gui_input_attr),0,0,0},
	{ST_TOGGLE, N_("Show nick box"), P_OFFINTNL(hex_gui_input_nick),0,0,1},
	{ST_TOGGLE, N_("Show user mode icon in nick box"), P_OFFINTNL(hex_gui_input_icon),0,0,0},
	{ST_NUMBER, N_("Maximum input box height:"), P_OFFINTNL(hex_gui_input_lines), 0, (const char **)N_("lines."), 20},
	{ST_TOGGLE, N_("Spell checking"), P_OFFINTNL(hex_gui_input_spell),0,0,1},
	{ST_ENTRY,	N_("Dictionaries to use:"), P_OFFSETNL(hex_text_spell_langs),0,0,sizeof prefs.hex_text_spell_langs},
#ifdef WIN32
	{ST_LABEL,	N_("Use language codes (as in \"%LOCALAPPDATA%\\enchant\\myspell\\dicts\").\nSeparate multiple entries with commas.")},
#else
	{ST_LABEL,	N_("Use language codes. Separate multiple entries with commas.")},
#endif

	{ST_HEADER, N_("Nick Completion"),0,0,0},
	{ST_ENTRY,	N_("Nick completion suffix:"), P_OFFSETNL(hex_completion_suffix),0,0,sizeof prefs.hex_completion_suffix},
	{ST_MENU,	N_("Nick completion sorted:"), P_OFFINTNL(hex_completion_sort), 0, tabcompmenu, 0},
	{ST_NUMBER,	N_("Nick completion amount:"), P_OFFINTNL(hex_completion_amount), N_("Threshold of nicks to start listing instead of completing"), (const char **)N_("nicks."), 1000},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const lagmenutext[] = 
{
	N_("Off"),
	N_("Graphical"),
	N_("Text"),
	N_("Both"),
	NULL
};

static const char *const ulmenutext[] = 
{
	N_("A-Z, ops first"),
	N_("A-Z"),
	N_("Z-A, ops last"),
	N_("Z-A"),
	N_("Unsorted"),
	NULL
};

static const char *const cspos[] =
{
	N_("Left (upper)"),
	N_("Left (lower)"),
	N_("Right (upper)"),
	N_("Right (lower)"),
	N_("Top"),
	N_("Bottom"),
	N_("Hidden"),
	NULL
};

static const char *const ulpos[] =
{
	N_("Left (upper)"),
	N_("Left (lower)"),
	N_("Right (upper)"),
	N_("Right (lower)"),
	NULL
};

static const setting userlist_settings[] =
{
	{ST_HEADER,	N_("User List"),0,0,0},
	{ST_TOGGLE, N_("Show hostnames in user list"), P_OFFINTNL(hex_gui_ulist_show_hosts), 0, 0, 0},
	{ST_TOGGLE, N_("Use the Text box font and colors"), P_OFFINTNL(hex_gui_ulist_style),0,0,0},
	{ST_TOGGLE, N_("Show icons for user modes"), P_OFFINTNL(hex_gui_ulist_icons), N_("Use graphical icons instead of text symbols in the user list."), 0, 0},
	{ST_TOGGLE, N_("Color nicknames in userlist"), P_OFFINTNL(hex_gui_ulist_color), N_("Will color nicknames the same as in chat."), 0, 0},
	{ST_TOGGLE, N_("Show user count in channels"), P_OFFINTNL(hex_gui_ulist_count), 0, 0, 0},
	{ST_MENU,	N_("User list sorted by:"), P_OFFINTNL(hex_gui_ulist_sort), 0, ulmenutext, 0},
	{ST_MENU,	N_("Show user list at:"), P_OFFINTNL(hex_gui_ulist_pos), 0, ulpos, 1},

	{ST_HEADER,	N_("Away Tracking"),0,0,0},
	{ST_TOGGLE,	N_("Track the away status of users and mark them in a different color"), P_OFFINTNL(hex_away_track),0,0,1},
	{ST_NUMBER, N_("On channels smaller than:"), P_OFFINTNL(hex_away_size_max),0,0,10000},

	{ST_HEADER,	N_("Action Upon Double Click"),0,0,0},
	{ST_ENTRY,	N_("Execute command:"), P_OFFSETNL(hex_gui_ulist_doubleclick), 0, 0, sizeof prefs.hex_gui_ulist_doubleclick},

	{ST_HEADER,	N_("Extra Gadgets"),0,0,0},
	{ST_MENU,	N_("Lag meter:"), P_OFFINTNL(hex_gui_lagometer), 0, lagmenutext, 0},
	{ST_MENU,	N_("Throttle meter:"), P_OFFINTNL(hex_gui_throttlemeter), 0, lagmenutext, 0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const tabwin[] =
{
	N_("Windows"),
	N_("Tabs"),
	NULL
};

static const char *const focusnewtabsmenu[] =
{
	N_("Never"),
	N_("Always"),
	N_("Only requested tabs"),
	NULL
};

static const char *const noticeposmenu[] =
{
	N_("Automatic"),
	N_("In an extra tab"),
	N_("In the front tab"),
	NULL
};

static const char *const swtype[] =
{
	N_("Tabs"),	/* 0 tabs */
	"",			/* 1 reserved */
	N_("Tree"),	/* 2 tree */
	NULL
};

static const setting tabs_settings[] =
{
	/*{ST_HEADER,	N_("Channel Switcher"),0,0,0},*/
	{ST_RADIO,  N_("Switcher type:"),P_OFFINTNL(hex_gui_tab_layout), 0, swtype, 0},
	{ST_TOGGLE, N_("Open an extra tab for server messages"), P_OFFINTNL(hex_gui_tab_server), 0, 0, 0},
	{ST_TOGGLE, N_("Open a new tab when you receive a private message"), P_OFFINTNL(hex_gui_autoopen_dialog), 0, 0, 0},
	{ST_TOGGLE, N_("Sort tabs in alphabetical order"), P_OFFINTNL(hex_gui_tab_sort), 0, 0, 0},
	{ST_TOGGLE, N_("Show icons in the channel tree"), P_OFFINTNL(hex_gui_tab_icons), 0, 0, 0},
	{ST_TOGGLE, N_("Download and show network icons"), P_OFFINTNL(hex_gui_network_icons), 0, 0, 0},
	{ST_TOGGLE, N_("Show dotted lines in the channel tree"), P_OFFINTNL(hex_gui_tab_dots), 0, 0, 0},
	{ST_TOGGLE, N_("Scroll mouse-wheel to change tabs"), P_OFFINTNL (hex_gui_tab_scrollchans), 0, 0, 0},
	{ST_TOGGLE, N_("Middle click to close tab"), P_OFFINTNL(hex_gui_tab_middleclose), 0, 0, 0},
	{ST_TOGGLE, N_("Smaller text"), P_OFFINTNL(hex_gui_tab_small), 0, 0, 0},
	{ST_MENU,	N_("Focus new tabs:"), P_OFFINTNL(hex_gui_tab_newtofront), 0, focusnewtabsmenu, 0},
	{ST_MENU,	N_("Placement of notices:"), P_OFFINTNL(hex_irc_notice_pos), 0, noticeposmenu, 0},
	{ST_MENU,	N_("Show channel switcher at:"), P_OFFINTNL(hex_gui_tab_pos), 0, cspos, 1},
	{ST_NUMBER,	N_("Shorten tab labels to:"), P_OFFINTNL(hex_gui_tab_trunc), 0, (const char **)N_("letters."), 99},

	{ST_HEADER,	N_("Tabs or Windows"),0,0,0},
	{ST_MENU,	N_("Open channels in:"), P_OFFINTNL(hex_gui_tab_chans), 0, tabwin, 0},
	{ST_MENU,	N_("Open dialogs in:"), P_OFFINTNL(hex_gui_tab_dialogs), 0, tabwin, 0},
	{ST_MENU,	N_("Open utilities in:"), P_OFFINTNL(hex_gui_tab_utils), N_("Open DCC, Ignore, Notify etc, in tabs or windows?"), tabwin, 0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting color_settings[] =
{
	{ST_TOGGLE, N_("Messages"), P_OFFINTNL(hex_text_stripcolor_msg), 0, 0, 0},
	{ST_TOGGLE, N_("Scrollback"), P_OFFINTNL(hex_text_stripcolor_replay), 0, 0, 0},
	{ST_TOGGLE, N_("Topic"), P_OFFINTNL(hex_text_stripcolor_topic), 0, 0, 0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const dccaccept[] =
{
	N_("Ask for confirmation"),
	N_("Ask for download folder"),
	N_("Save without interaction"),
	NULL
};

static const setting filexfer_settings[] =
{
	{ST_HEADER, N_("Files and Directories"), 0, 0, 0},
	{ST_MENU,	N_("Auto accept file offers:"), P_OFFINTNL(hex_dcc_auto_recv), 0, dccaccept, 0},
	{ST_EFOLDER,N_("Download files to:"), P_OFFSETNL(hex_dcc_dir), 0, 0, sizeof prefs.hex_dcc_dir},
	{ST_EFOLDER,N_("Move completed files to:"), P_OFFSETNL(hex_dcc_completed_dir), 0, 0, sizeof prefs.hex_dcc_completed_dir},
	{ST_TOGGLE, N_("Save nick name in filenames"), P_OFFINTNL(hex_dcc_save_nick), 0, 0, 0},

	{ST_HEADER,	N_("Auto Open DCC Windows"),0,0,0},
	{ST_TOGGLE, N_("Send window"), P_OFFINTNL(hex_gui_autoopen_send), 0, 0, 0},
	{ST_TOGGLE, N_("Receive window"), P_OFFINTNL(hex_gui_autoopen_recv), 0, 0, 0},
	{ST_TOGGLE, N_("Chat window"), P_OFFINTNL(hex_gui_autoopen_chat), 0, 0, 0},

	{ST_HEADER, N_("Maximum File Transfer Speeds (Byte per Second)"), 0, 0, 0},
	{ST_NUMBER,	N_("One upload:"), P_OFFINTNL(hex_dcc_max_send_cps), 
					N_("Maximum speed for one transfer"), 0, 10000000},
	{ST_NUMBER,	N_("One download:"), P_OFFINTNL(hex_dcc_max_get_cps),
					N_("Maximum speed for one transfer"), 0, 10000000},
	{ST_NUMBER,	N_("All uploads combined:"), P_OFFINTNL(hex_dcc_global_max_send_cps),
					N_("Maximum speed for all files"), 0, 10000000},
	{ST_NUMBER,	N_("All downloads combined:"), P_OFFINTNL(hex_dcc_global_max_get_cps),
					N_("Maximum speed for all files"), 0, 10000000},

	{ST_END, 0, 0, 0, 0, 0}
};

static const int balloonlist[3] =
{
	P_OFFINTNL(hex_input_balloon_chans), P_OFFINTNL(hex_input_balloon_priv), P_OFFINTNL(hex_input_balloon_hilight)
};

static const int trayblinklist[3] =
{
	P_OFFINTNL(hex_input_tray_chans), P_OFFINTNL(hex_input_tray_priv), P_OFFINTNL(hex_input_tray_hilight)
};

static const int taskbarlist[3] =
{
	P_OFFINTNL(hex_input_flash_chans), P_OFFINTNL(hex_input_flash_priv), P_OFFINTNL(hex_input_flash_hilight)
};

static const int beeplist[3] =
{
	P_OFFINTNL(hex_input_beep_chans), P_OFFINTNL(hex_input_beep_priv), P_OFFINTNL(hex_input_beep_hilight)
};

static const setting alert_settings[] =
{
	{ST_HEADER,	N_("Alerts"),0,0,0},

	{ST_ALERTHEAD},


	{ST_3OGGLE, N_("Show notifications on:"), 0, 0, (void *)balloonlist, 0},
	{ST_3OGGLE, N_("Blink tray icon on:"), 0, 0, (void *)trayblinklist, 0},
	{ST_3OGGLE, N_("Blink task bar on:"), 0, 0, (void *)taskbarlist, 0},
#ifdef WIN32
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play the \"Instant Message Notification\" system sound upon the selected events"), (void *)beeplist, 0},
#else
#ifdef USE_LIBCANBERRA
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play \"message-new-instant\" from the freedesktop.org sound theme upon the selected events"), (void *)beeplist, 0},
#else
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play a GTK beep upon the selected events"), (void *)beeplist, 0},
#endif
#endif

	{ST_TOGGLE,	N_("Omit alerts when marked as being away"), P_OFFINTNL(hex_away_omit_alerts), 0, 0, 0},
	{ST_TOGGLE,	N_("Omit alerts while the window is focused"), P_OFFINTNL(hex_gui_focus_omitalerts), 0, 0, 0},

	{ST_HEADER,	N_("Tray Behavior"), 0, 0, 0},
	{ST_TOGGLE,	N_("Enable system tray icon"), P_OFFINTNL(hex_gui_tray), 0, 0, 4},
	{ST_TOGGLE,	N_("Minimize to tray"), P_OFFINTNL(hex_gui_tray_minimize), 0, 0, 0},
	{ST_TOGGLE,	N_("Close to tray"), P_OFFINTNL(hex_gui_tray_close), 0, 0, 0},
	{ST_TOGGLE,	N_("Automatically mark away/back"), P_OFFINTNL(hex_gui_tray_away), N_("Automatically change status when hiding to tray."), 0, 0},
	{ST_TOGGLE,	N_("Only show notifications when hidden or iconified"), P_OFFINTNL(hex_gui_tray_quiet), 0, 0, 0},

	{ST_HEADER,	N_("Highlighted Messages"),0,0,0},
	{ST_LABEL,	N_("Highlighted messages are ones where your nickname is mentioned, but also:"), 0, 0, 0, 1},

	{ST_ENTRY,	N_("Extra words to highlight:"), P_OFFSETNL(hex_irc_extra_hilight), 0, 0, sizeof prefs.hex_irc_extra_hilight},
	{ST_ENTRY,	N_("Nick names not to highlight:"), P_OFFSETNL(hex_irc_no_hilight), 0, 0, sizeof prefs.hex_irc_no_hilight},
	{ST_ENTRY,	N_("Nick names to always highlight:"), P_OFFSETNL(hex_irc_nick_hilight), 0, 0, sizeof prefs.hex_irc_nick_hilight},
	{ST_LABEL,	N_("Separate multiple words with commas.\nWildcards are accepted.")},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting alert_settings_nonotifications[] =
{
	{ST_HEADER,	N_("Alerts"),0,0,0},

	{ST_ALERTHEAD},
	{ST_3OGGLE, N_("Blink tray icon on:"), 0, 0, (void *)trayblinklist, 0},
#ifdef HAVE_GTK_MAC
	{ST_3OGGLE, N_("Bounce dock icon on:"), 0, 0, (void *)taskbarlist, 0},
#else
#ifndef __APPLE__
	{ST_3OGGLE, N_("Blink task bar on:"), 0, 0, (void *)taskbarlist, 0},
#endif
#endif
#ifdef WIN32
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play the \"Instant Message Notification\" system sound upon the selected events"), (void *)beeplist, 0},
#else
#ifdef USE_LIBCANBERRA
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play \"message-new-instant\" from the freedesktop.org sound theme upon the selected events"), (void *)beeplist, 0},
#else
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, N_("Play a GTK beep upon the selected events"), (void *)beeplist, 0},
#endif
#endif

	{ST_TOGGLE,	N_("Omit alerts when marked as being away"), P_OFFINTNL(hex_away_omit_alerts), 0, 0, 0},
	{ST_TOGGLE,	N_("Omit alerts while the window is focused"), P_OFFINTNL(hex_gui_focus_omitalerts), 0, 0, 0},

	{ST_HEADER,	N_("Tray Behavior"), 0, 0, 0},
	{ST_TOGGLE,	N_("Enable system tray icon"), P_OFFINTNL(hex_gui_tray), 0, 0, 4},
	{ST_TOGGLE,	N_("Minimize to tray"), P_OFFINTNL(hex_gui_tray_minimize), 0, 0, 0},
	{ST_TOGGLE,	N_("Close to tray"), P_OFFINTNL(hex_gui_tray_close), 0, 0, 0},
	{ST_TOGGLE,	N_("Automatically mark away/back"), P_OFFINTNL(hex_gui_tray_away), N_("Automatically change status when hiding to tray."), 0, 0},

	{ST_HEADER,	N_("Highlighted Messages"),0,0,0},
	{ST_LABEL,	N_("Highlighted messages are ones where your nickname is mentioned, but also:"), 0, 0, 0, 1},

	{ST_ENTRY,	N_("Extra words to highlight:"), P_OFFSETNL(hex_irc_extra_hilight), 0, 0, sizeof prefs.hex_irc_extra_hilight},
	{ST_ENTRY,	N_("Nick names not to highlight:"), P_OFFSETNL(hex_irc_no_hilight), 0, 0, sizeof prefs.hex_irc_no_hilight},
	{ST_ENTRY,	N_("Nick names to always highlight:"), P_OFFSETNL(hex_irc_nick_hilight), 0, 0, sizeof prefs.hex_irc_nick_hilight},
	{ST_LABEL,	N_("Separate multiple words with commas.\nWildcards are accepted.")},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting alert_settings_unity[] =
{
	{ST_HEADER,	N_("Alerts"),0,0,0},

	{ST_ALERTHEAD},
	{ST_3OGGLE, N_("Show notifications on:"), 0, 0, (void *)balloonlist, 0},
	{ST_3OGGLE, N_("Blink task bar on:"), 0, 0, (void *)taskbarlist, 0},
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, 0, (void *)beeplist, 0},

	{ST_TOGGLE,	N_("Omit alerts when marked as being away"), P_OFFINTNL(hex_away_omit_alerts), 0, 0, 0},
	{ST_TOGGLE,	N_("Omit alerts while the window is focused"), P_OFFINTNL(hex_gui_focus_omitalerts), 0, 0, 0},

	{ST_HEADER,	N_("Highlighted Messages"),0,0,0},
	{ST_LABEL,	N_("Highlighted messages are ones where your nickname is mentioned, but also:"), 0, 0, 0, 1},

	{ST_ENTRY,	N_("Extra words to highlight:"), P_OFFSETNL(hex_irc_extra_hilight), 0, 0, sizeof prefs.hex_irc_extra_hilight},
	{ST_ENTRY,	N_("Nick names not to highlight:"), P_OFFSETNL(hex_irc_no_hilight), 0, 0, sizeof prefs.hex_irc_no_hilight},
	{ST_ENTRY,	N_("Nick names to always highlight:"), P_OFFSETNL(hex_irc_nick_hilight), 0, 0, sizeof prefs.hex_irc_nick_hilight},
	{ST_LABEL,	N_("Separate multiple words with commas.\nWildcards are accepted.")},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting alert_settings_unityandnonotifications[] =
{
	{ST_HEADER, N_("Alerts"), 0, 0, 0},

	{ST_ALERTHEAD},
	{ST_3OGGLE, N_("Blink task bar on:"), 0, 0, (void *)taskbarlist, 0},
	{ST_3OGGLE, N_("Make a beep sound on:"), 0, 0, (void *)beeplist, 0},

	{ST_TOGGLE, N_("Omit alerts when marked as being away"), P_OFFINTNL (hex_away_omit_alerts), 0, 0, 0},
	{ST_TOGGLE, N_("Omit alerts while the window is focused"), P_OFFINTNL (hex_gui_focus_omitalerts), 0, 0, 0},

	{ST_HEADER, N_("Highlighted Messages"), 0, 0, 0},
	{ST_LABEL, N_("Highlighted messages are ones where your nickname is mentioned, but also:"), 0, 0, 0, 1},

	{ST_ENTRY, N_("Extra words to highlight:"), P_OFFSETNL (hex_irc_extra_hilight), 0, 0, sizeof prefs.hex_irc_extra_hilight},
	{ST_ENTRY, N_("Nick names not to highlight:"), P_OFFSETNL (hex_irc_no_hilight), 0, 0, sizeof prefs.hex_irc_no_hilight},
	{ST_ENTRY, N_("Nick names to always highlight:"), P_OFFSETNL (hex_irc_nick_hilight), 0, 0, sizeof prefs.hex_irc_nick_hilight},
	{ST_LABEL, N_("Separate multiple words with commas.\nWildcards are accepted.")},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting general_settings[] =
{
	{ST_HEADER,	N_("Default Messages"),0,0,0},
	{ST_ENTRY,	N_("Quit:"), P_OFFSETNL(hex_irc_quit_reason), 0, 0, sizeof prefs.hex_irc_quit_reason},
	{ST_ENTRY,	N_("Leave channel:"), P_OFFSETNL(hex_irc_part_reason), 0, 0, sizeof prefs.hex_irc_part_reason},
	{ST_ENTRY,	N_("Away:"), P_OFFSETNL(hex_away_reason), 0, 0, sizeof prefs.hex_away_reason},

	{ST_HEADER,	N_("Away"),0,0,0},
	{ST_TOGGLE,	N_("Show away once"), P_OFFINTNL(hex_away_show_once), N_("Show identical away messages only once."), 0, 0},
	{ST_TOGGLE,	N_("Automatically unmark away"), P_OFFINTNL(hex_away_auto_unmark), N_("Unmark yourself as away before sending messages."), 0, 0},

	{ST_HEADER,	N_("Miscellaneous"),0,0,0},
	{ST_TOGGLE,	N_("Display MODEs in raw form"), P_OFFINTNL(hex_irc_raw_modes), 0, 0, 0},
	{ST_TOGGLE,	N_("WHOIS on notify"), P_OFFINTNL(hex_notify_whois_online), N_("Sends a /WHOIS when a user comes online in your notify list."), 0, 0},
	{ST_TOGGLE,	N_("Hide join and part messages"), P_OFFINTNL(hex_irc_conf_mode), N_("Hide channel join/part messages by default."), 0, 0},
	{ST_TOGGLE,	N_("Hide nick change messages"), P_OFFINTNL(hex_irc_hide_nickchange), 0, 0, 0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const bantypemenu[] = 
{
	N_("*!*@*.host"),
	N_("*!*@domain"),
	N_("*!*user@*.host"),
	N_("*!*user@domain"),
	NULL
};

static const setting advanced_settings[] =
{
	{ST_HEADER,	N_("Auto Copy Behavior"),0,0,0},
	{ST_TOGGLE, N_("Automatically copy selected text"), P_OFFINTNL(hex_text_autocopy_text),
					N_("Copy selected text to clipboard when left mouse button is released. "
						"Otherwise, Ctrl+Shift+C will copy the "
						"selected text to the clipboard."), 0, 0},
	{ST_TOGGLE, N_("Automatically include timestamps"), P_OFFINTNL(hex_text_autocopy_stamp),
					N_("Automatically include timestamps in copied lines of text. Otherwise, "
						"include timestamps if the Shift key is held down while selecting."), 0, 0},
	{ST_TOGGLE, N_("Automatically include color information"), P_OFFINTNL(hex_text_autocopy_color),
					N_("Automatically include color information in copied lines of text.  "
						"Otherwise, include color information if the Ctrl key is held down "
						"while selecting."), 0, 0},

	{ST_HEADER,	N_("Miscellaneous"), 0, 0, 0},
	{ST_ENTRY,  N_("Real name:"), P_OFFSETNL(hex_irc_real_name), 0, 0, sizeof prefs.hex_irc_real_name},
	{ST_ENTRY,  N_("Alternative fonts:"), P_OFFSETNL(hex_text_font_alternative), N_("Separate multiple entries with commas without spaces before or after."), 0, sizeof prefs.hex_text_font_alternative},
	{ST_TOGGLE,	N_("Display lists in compact mode"), P_OFFINTNL(hex_gui_compact), N_("Use less spacing between user list/channel tree rows."), 0, 0},
	{ST_TOGGLE,	N_("Use server time if supported"), P_OFFINTNL(hex_irc_cap_server_time), N_("Display timestamps obtained from server if it supports the time-server extension."), 0, 0},
	{ST_TOGGLE,	N_("Automatically reconnect to servers on disconnect"), P_OFFINTNL(hex_net_auto_reconnect), 0, 0, 1},
	{ST_NUMBER,	N_("Auto reconnect delay:"), P_OFFINTNL(hex_net_reconnect_delay), 0, 0, 9999},
	{ST_NUMBER,	N_("Auto join delay:"), P_OFFINTNL(hex_irc_join_delay), 0, 0, 9999},
	{ST_MENU,	N_("Ban Type:"), P_OFFINTNL(hex_irc_ban_type), N_("Attempt to use this banmask when banning or quieting. (requires irc_who_join)"), bantypemenu, 0},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting logging_settings[] =
{
	{ST_HEADER,	N_("Logging"),0,0,0},
	{ST_TOGGLE,	N_("Display scrollback from previous session"), P_OFFINTNL(hex_text_replay), 0, 0, 0},
	{ST_NUMBER,	N_("Max selectable messages:"), P_OFFINTNL(hex_text_max_lines),0,0,100000},
	{ST_TOGGLE,	N_("Enable logging of conversations to disk"), P_OFFINTNL(hex_irc_logging), 0, 0, 0},
	{ST_ENTRY,	N_("Log filename:"), P_OFFSETNL(hex_irc_logmask), 0, 0, sizeof prefs.hex_irc_logmask},
	{ST_LABEL,	N_("%s=Server %c=Channel %n=Network.")},

	{ST_HEADER,	N_("Timestamps"),0,0,0},
	{ST_TOGGLE,	N_("Insert timestamps in logs"), P_OFFINTNL(hex_stamp_log), 0, 0, 1},
	{ST_ENTRY,	N_("Log timestamp format:"), P_OFFSETNL(hex_stamp_log_format), 0, 0, sizeof prefs.hex_stamp_log_format},
#ifdef WIN32
	{ST_LABEL,	N_("See the strftime MSDN article for details.")},
#else
	{ST_LABEL,	N_("See the strftime manpage for details.")},
#endif

	{ST_HEADER,	N_("URLs"),0,0,0},
	{ST_TOGGLE,	N_("Enable logging of URLs to disk"), P_OFFINTNL(hex_url_logging), 0, 0, 0},
	{ST_TOGGLE,	N_("Enable URL grabber"), P_OFFINTNL(hex_url_grabber), 0, 0, 1},
	{ST_NUMBER,	N_("Maximum number of URLs to grab:"), P_OFFINTNL(hex_url_grabber_limit), 0, 0, 9999},

	{ST_END, 0, 0, 0, 0, 0}
};

static const char *const proxytypes[] =
{
	N_("(Disabled)"),
	N_("Wingate"),
	N_("SOCKS4"),
	N_("SOCKS5"),
	N_("HTTP"),
	N_("Auto"),
	NULL
};

static const char *const proxyuse[] =
{
	N_("All connections"),
	N_("IRC server only"),
	N_("DCC only"),
	NULL
};

static const setting network_settings[] =
{
	{ST_HEADER,	N_("Your Address"), 0, 0, 0, 0},
	{ST_ENTRY,	N_("Bind to:"), P_OFFSETNL(hex_net_bind_host), 0, 0, sizeof prefs.hex_net_bind_host},
	{ST_LABEL,	N_("Only useful for computers with multiple addresses.")},

	{ST_HEADER, N_("File Transfers"), 0, 0, 0},
	{ST_TOGGLE, N_("Get my address from the IRC server"), P_OFFINTNL(hex_dcc_ip_from_server),
					N_("Asks the IRC server for your real address. Use this if you have a 192.168.*.* address!"), 0, 0},
	{ST_ENTRY,	N_("DCC IP address:"), P_OFFSETNL(hex_dcc_ip),
					N_("Claim you are at this address when offering files."), 0, sizeof prefs.hex_dcc_ip},
	{ST_NUMBER,	N_("First DCC listen port:"), P_OFFINTNL(hex_dcc_port_first), 0, 0, 65535},
	{ST_NUMBER,	N_("Last DCC listen port:"), P_OFFINTNL(hex_dcc_port_last), 0, 
		(const char **)N_("!Leave ports at zero for full range."), 65535},

	{ST_HEADER,	N_("Proxy Server"), 0, 0, 0, 0},
	{ST_ENTRY,	N_("Hostname:"), P_OFFSETNL(hex_net_proxy_host), 0, 0, sizeof prefs.hex_net_proxy_host},
	{ST_NUMBER,	N_("Port:"), P_OFFINTNL(hex_net_proxy_port), 0, 0, 65535},
	{ST_MENU,	N_("Type:"), P_OFFINTNL(hex_net_proxy_type), 0, proxytypes, 0},
	{ST_MENU,	N_("Use proxy for:"), P_OFFINTNL(hex_net_proxy_use), 0, proxyuse, 0},

	{ST_HEADER,	N_("Proxy Authentication"), 0, 0, 0, 0},
	{ST_TOGGLE,	N_("Use authentication (HTTP or SOCKS5 only)"), P_OFFINTNL(hex_net_proxy_auth), 0, 0, 0},
	{ST_ENTRY,	N_("Username:"), P_OFFSETNL(hex_net_proxy_user), 0, 0, sizeof prefs.hex_net_proxy_user},
	{ST_ENTRY,	N_("Password:"), P_OFFSETNL(hex_net_proxy_pass), 0, GINT_TO_POINTER(1), sizeof prefs.hex_net_proxy_pass},

	{ST_END, 0, 0, 0, 0, 0}
};

static const setting identd_settings[] =
{
	{ST_HEADER, N_("Identd Server"), 0, 0, 0, 0},
	{ST_TOGGLE, N_("Enabled"), P_OFFINTNL(hex_identd_server), N_("Server will respond with the networks username"), 0, 1},
	{ST_NUMBER,	N_("Port:"), P_OFFINTNL(hex_identd_port), N_("You must have permissions to listen on this port. "
												   "If not 113 (0 defaults to this) then you must configure port-forwarding."), 0, 65535},

	{ST_END, 0, 0, 0, 0, 0}
};

#define setup_get_str(pr,set) (((char *)pr)+set->offset)
#define setup_get_int(pr,set) *(((int *)pr)+set->offset)
#define setup_get_int3(pr,off) *(((int *)pr)+off) 

#define setup_set_int(pr,set,num) *((int *)pr+set->offset)=num
#define setup_set_str(pr,set,str) g_strlcpy(((char *)pr)+set->offset,str,set->extra)


static void
setup_3oggle_cb (GtkCheckButton *but, unsigned int *setting)
{
	/* In GTK4, toggle signals can fire during widget destruction.
	 * Ignore callbacks after we've started applying settings. */
	if (setup_applying)
		return;
	*setting = gtk_check_button_get_active (but);
}

static void
setup_headlabel (GtkWidget *tab, int row, int col, char *text)
{
	GtkWidget *label;
	char buf[128];
	char *sp;

	g_snprintf (buf, sizeof (buf), "<b><span size=\"smaller\">%s</span></b>", text);
	sp = strchr (buf + 17, ' ');
	if (sp)
		*sp = '\n';

	label = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (label), buf);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, 4);
	gtk_grid_attach (GTK_GRID (tab), label, col, row, 1, 1);
}

static void
setup_create_alert_header (GtkWidget *tab, int row, const setting *set)
{
	setup_headlabel (tab, row, 3, _("Channel Message"));
	setup_headlabel (tab, row, 4, _("Private Message"));
	setup_headlabel (tab, row, 5, _("Highlighted Message"));
}

/* makes 3 toggles side-by-side */

static void
setup_create_3oggle (GtkWidget *tab, int row, const setting *set)
{
	GtkWidget *label, *wid;
	int *offsets = (int *)set->list;

	label = gtk_label_new (_(set->label));
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	if (set->tooltip)
	{
		gtk_widget_set_tooltip_text (label, _(set->tooltip));
	}
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), label, 2, row, 1, 1);

	wid = gtk_check_button_new ();
	gtk_check_button_set_active (GTK_CHECK_BUTTON (wid),
											setup_get_int3 (&setup_prefs, offsets[0]));
	g_signal_connect (G_OBJECT (wid), "toggled",
							G_CALLBACK (setup_3oggle_cb), ((int *)&setup_prefs) + offsets[0]);
	gtk_grid_attach (GTK_GRID (tab), wid, 3, row, 1, 1);

	wid = gtk_check_button_new ();
	gtk_check_button_set_active (GTK_CHECK_BUTTON (wid),
											setup_get_int3 (&setup_prefs, offsets[1]));
	g_signal_connect (G_OBJECT (wid), "toggled",
							G_CALLBACK (setup_3oggle_cb), ((int *)&setup_prefs) + offsets[1]);
	gtk_grid_attach (GTK_GRID (tab), wid, 4, row, 1, 1);

	wid = gtk_check_button_new ();
	gtk_check_button_set_active (GTK_CHECK_BUTTON (wid),
											setup_get_int3 (&setup_prefs, offsets[2]));
	g_signal_connect (G_OBJECT (wid), "toggled",
							G_CALLBACK (setup_3oggle_cb), ((int *)&setup_prefs) + offsets[2]);
	gtk_grid_attach (GTK_GRID (tab), wid, 5, row, 1, 1);
}

static void
setup_toggle_cb (GtkCheckButton *but, const setting *set)
{
	GtkWidget *label, *disable_wid;
	int active_val;

	/* In GTK4, toggle signals can fire during widget destruction.
	 * Ignore callbacks after we've started applying settings. */
	if (setup_applying)
		return;

	active_val = gtk_check_button_get_active (but);
	setup_set_int (&setup_prefs, set, active_val);

	/* does this toggle also enable/disable another widget? */
	disable_wid = g_object_get_data (G_OBJECT (but), "nxt");
	if (disable_wid)
	{
		gtk_widget_set_sensitive (disable_wid, active_val);
		label = g_object_get_data (G_OBJECT (disable_wid), "lbl");
		gtk_widget_set_sensitive (label, active_val);
	}
}

static void
setup_toggle_sensitive_cb (GtkCheckButton *but, GtkWidget *wid)
{
	gtk_widget_set_sensitive (wid, gtk_check_button_get_active (but));
}

static void
setup_create_toggleR (GtkWidget *tab, int row, const setting *set)
{
	GtkWidget *wid;
	int val;

	wid = gtk_check_button_new_with_label (_(set->label));
	val = setup_get_int (&setup_prefs, set);
	hc_debug_log ("setup_create_toggleR: %s offset=%d val=%d", set->label, set->offset, val);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (wid), val);
	g_signal_connect (G_OBJECT (wid), "toggled",
							G_CALLBACK (setup_toggle_cb), (gpointer)set);
	if (set->tooltip)
		gtk_widget_set_tooltip_text (wid, _(set->tooltip));
	gtk_widget_set_hexpand (wid, TRUE);
	gtk_grid_attach (GTK_GRID (tab), wid, 4, row, 1, 1);
}

static GtkWidget *
setup_create_toggleL (GtkWidget *tab, int row, const setting *set)
{
	GtkWidget *wid;
	int val;

	wid = gtk_check_button_new_with_label (_(set->label));
	val = setup_get_int (&setup_prefs, set);
	hc_debug_log ("setup_create_toggleL: %s offset=%d val=%d", set->label, set->offset, val);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (wid), val);
	g_signal_connect (G_OBJECT (wid), "toggled",
							G_CALLBACK (setup_toggle_cb), (gpointer)set);
	if (set->tooltip)
		gtk_widget_set_tooltip_text (wid, _(set->tooltip));
	gtk_widget_set_margin_start (wid, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), wid, 2, row, row==6 ? 4 : 2, 1);

	return wid;
}

static GtkWidget *
setup_create_italic_label (char *text)
{
	GtkWidget *label;
	char buf[256];

	label = gtk_label_new (NULL);
	g_snprintf (buf, sizeof (buf), "<i><span size=\"smaller\">%s</span></i>", text);
	gtk_label_set_markup (GTK_LABEL (label), buf);
	gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);

	return label;
}

static void
setup_spin_cb (GtkSpinButton *spin, const setting *set)
{
	if (setup_applying)
		return;
	setup_set_int (&setup_prefs, set, gtk_spin_button_get_value_as_int (spin));
}

static GtkWidget *
setup_create_spin (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *label, *wid, *rbox;
	char *text;

	label = gtk_label_new (_(set->label));
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (table), label, 2, row, 1, 1);

	rbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_halign (rbox, GTK_ALIGN_START);
	gtk_widget_set_valign (rbox, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand (rbox, TRUE);
	gtk_grid_attach (GTK_GRID (table), rbox, 3, row, 1, 1);

	wid = gtk_spin_button_new_with_range (0, set->extra, 1);
	g_object_set_data (G_OBJECT (wid), "lbl", label);
	if (set->tooltip)
		gtk_widget_set_tooltip_text (wid, _(set->tooltip));
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (wid),
										setup_get_int (&setup_prefs, set));
	g_signal_connect (G_OBJECT (wid), "value_changed",
							G_CALLBACK (setup_spin_cb), (gpointer)set);
	gtk_box_append (GTK_BOX (rbox), wid);

	if (set->list)
	{
		text = _((char *)set->list);
		if (text[0] == '!')
			label = setup_create_italic_label (text + 1);
		else
			label = gtk_label_new (text);
		gtk_widget_set_margin_start (label, 6);
		gtk_box_append (GTK_BOX (rbox), label);
	}

	return wid;
}

static gint
setup_apply_trans (int *tag)
{
	prefs.hex_gui_transparency = setup_prefs.hex_gui_transparency;
	/* GTK4: gtk_window_set_opacity removed; use CSS opacity if needed */

	/* mg_update_xtext (current_sess->gui->xtext); */
	*tag = 0;
	return 0;
}

static void
setup_hscale_cb (GtkScale *wid, const setting *set)
{
	static int tag = 0;

	if (setup_applying)
		return;
	setup_set_int (&setup_prefs, set, (int) gtk_range_get_value (GTK_RANGE (wid)));

	if (tag == 0)
	{
		tag = g_idle_add ((GSourceFunc) setup_apply_trans, &tag);
	}
}

static void
setup_create_hscale (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *wid;

	wid = gtk_label_new (_(set->label));
	gtk_widget_set_halign (wid, GTK_ALIGN_START);
	gtk_widget_set_valign (wid, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (wid, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (table), wid, 2, row, 1, 1);

	wid = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0., 255., 1.);
	gtk_scale_set_value_pos (GTK_SCALE (wid), GTK_POS_RIGHT);
	gtk_range_set_value (GTK_RANGE (wid), setup_get_int (&setup_prefs, set));
	g_signal_connect (G_OBJECT(wid), "value_changed",
							G_CALLBACK (setup_hscale_cb), (gpointer)set);
	gtk_widget_set_hexpand (wid, TRUE);
	gtk_grid_attach (GTK_GRID (table), wid, 3, row, 3, 1);

#ifndef WIN32 /* Windows always supports this */
	/* Only used for transparency currently */
	if (current_sess && current_sess->gui && current_sess->gui->window)
	{
		if (!gdk_display_is_composited (gtk_widget_get_display (current_sess->gui->window)))
			gtk_widget_set_sensitive (wid, FALSE);
	}
#endif
}


static GtkWidget *proxy_user; 	/* username GtkEntry */
static GtkWidget *proxy_pass; 	/* password GtkEntry */

static void
setup_menu_cb (GObject *cbox, GParamSpec *pspec, const setting *set)
{
	int n;

	if (setup_applying)
		return;
	n = gtk_drop_down_get_selected (GTK_DROP_DOWN (cbox));

	/* set the prefs.<field> */
	setup_set_int (&setup_prefs, set, n + set->extra);

	if (set->list == proxytypes)
	{
		/* only HTTP and SOCKS5 can use a username/pass */
		gtk_widget_set_sensitive (proxy_user, (n == 3 || n == 4 || n == 5));
		gtk_widget_set_sensitive (proxy_pass, (n == 3 || n == 4 || n == 5));
	}
}

static void
setup_radio_cb (GtkWidget *item, const setting *set)
{
	if (setup_applying)
		return;
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (item)))
	{
		int n = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "n"));
		/* set the prefs.<field> */
		setup_set_int (&setup_prefs, set, n);
	}
}

static int
setup_create_radio (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *wid, *hbox;
	GtkWidget *first_btn = NULL;
	int i;
	const char **text = (const char **)set->list;

	wid = gtk_label_new (_(set->label));
	gtk_widget_set_halign (wid, GTK_ALIGN_START);
	gtk_widget_set_valign (wid, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (wid, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (table), wid, 2, row, 1, 1);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_grid_attach (GTK_GRID (table), hbox, 3, row, 1, 1);

	i = 0;
	while (text[i])
	{
		if (text[i][0] != 0)
		{
			wid = gtk_check_button_new_with_mnemonic (_(text[i]));
			if (first_btn)
				gtk_check_button_set_group (GTK_CHECK_BUTTON (wid), GTK_CHECK_BUTTON (first_btn));
			else
				first_btn = wid;
			/*if (set->tooltip)
				gtk_widget_set_tooltip_text (wid, _(set->tooltip));*/
			gtk_box_append (GTK_BOX (hbox), wid);
			if (i == setup_get_int (&setup_prefs, set))
				gtk_check_button_set_active (GTK_CHECK_BUTTON (wid), TRUE);
			g_object_set_data (G_OBJECT (wid), "n", GINT_TO_POINTER (i));
			g_signal_connect (G_OBJECT (wid), "toggled",
									G_CALLBACK (setup_radio_cb), (gpointer)set);
		}
		i++;
		row++;
	}

	return i;
}

static void
setup_create_menu (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *wid, *cbox, *box;
	const char **text = (const char **)set->list;
	int i;

	wid = gtk_label_new (_(set->label));
	gtk_widget_set_halign (wid, GTK_ALIGN_START);
	gtk_widget_set_valign (wid, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (wid, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (table), wid, 2, row, 1, 1);

	{
		/* Build NULL-terminated array of translated strings for GtkStringList */
		GPtrArray *items = g_ptr_array_new ();
		for (i = 0; text[i]; i++)
			g_ptr_array_add (items, (gpointer)_(text[i]));
		g_ptr_array_add (items, NULL);

		GtkStringList *model = gtk_string_list_new ((const char * const *)items->pdata);
		g_ptr_array_free (items, TRUE);

		cbox = gtk_drop_down_new (G_LIST_MODEL (model), NULL);
		gtk_drop_down_set_selected (GTK_DROP_DOWN (cbox),
										  setup_get_int (&setup_prefs, set) - set->extra);
		g_signal_connect (cbox, "notify::selected",
								G_CALLBACK (setup_menu_cb), (gpointer)set);
	}

	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append (GTK_BOX (box), cbox);
	gtk_widget_set_hexpand (box, TRUE);
	gtk_grid_attach (GTK_GRID (table), box, 3, row, 1, 1);
}

static void
setup_filereq_cb (GtkWidget *entry, char *file)
{
	if (file)
	{
		if (file[0])
			hc_entry_set_text (entry, file);
	}
}

static void
setup_browsefile_cb (GtkWidget *button, GtkWidget *entry)
{
	/* used for background image only */
	char *filter;
	int filter_type;

#ifdef WIN32
	filter = "*png;*.tiff;*.gif;*.jpeg;*.jpg";
	filter_type = FRF_EXTENSIONS;
#else
	filter = "image/*";
	filter_type = FRF_MIMETYPES;
#endif
	gtkutil_file_req (GTK_WINDOW (setup_window), _("Select an Image File"), setup_filereq_cb,
					entry, NULL, filter, filter_type|FRF_RECENTLYUSED|FRF_MODAL);
}

static void
setup_fontsel_chosen_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GtkFontDialog *fd = GTK_FONT_DIALOG (source);
	GtkWidget *entry = GTK_WIDGET (user_data);
	PangoFontDescription *desc;
	GError *error = NULL;

	g_clear_object (&font_dialog_cancel);

	desc = gtk_font_dialog_choose_font_finish (fd, result, &error);
	if (desc)
	{
		char *font_name = pango_font_description_to_string (desc);
		hc_entry_set_text (entry, font_name);
		g_free (font_name);
		pango_font_description_free (desc);
	}
	else if (error && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
	         !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED))
	{
		g_warning ("Font dialog error: %s", error->message);
	}
	g_clear_error (&error);
}

static void
setup_browsefolder_cb (GtkWidget *button, GtkEntry *entry)
{
	gtkutil_file_req (GTK_WINDOW (setup_window), _("Select Download Folder"), setup_filereq_cb, entry, (char*)hc_entry_get_text (GTK_WIDGET (entry)), NULL, FRF_CHOOSEFOLDER|FRF_MODAL);
}

static void
setup_browsefont_cb (GtkWidget *button, GtkWidget *entry)
{
	GtkFontDialog *fd;
	PangoFontDescription *initial_desc = NULL;
	const char *current_font;

	/* Cancel any already-open font dialog */
	if (font_dialog_cancel)
	{
		g_cancellable_cancel (font_dialog_cancel);
		g_clear_object (&font_dialog_cancel);
	}

	fd = gtk_font_dialog_new ();
	gtk_font_dialog_set_title (fd, _("Select font"));
	gtk_font_dialog_set_modal (fd, TRUE);

	current_font = hc_entry_get_text (entry);
	if (current_font && current_font[0])
		initial_desc = pango_font_description_from_string (current_font);

	font_dialog_cancel = g_cancellable_new ();

	gtk_font_dialog_choose_font (fd, GTK_WINDOW (setup_window), initial_desc,
	                             font_dialog_cancel, setup_fontsel_chosen_cb, entry);

	if (initial_desc)
		pango_font_description_free (initial_desc);

	g_object_unref (fd);
}

static void
setup_entry_cb (GtkEntry *entry, setting *set)
{
	int size;
	int pos;
	unsigned char *p;
	int len;

	/* Window destruction clears entries, firing "changed" with empty text.
	 * Ignore callbacks while we're in the apply/destroy path. */
	if (setup_applying)
		return;

	p = (unsigned char*)hc_entry_get_text (GTK_WIDGET (entry));
	len = strlen (p);

	/* need to truncate? */
	if (len >= set->extra)
	{
		len = pos = 0;
		while (1)
		{
			size = g_utf8_skip [*p];
			len += size;
			p += size;
			/* truncate to "set->extra" BYTES */
			if (len >= set->extra)
			{
				gtk_editable_delete_text (GTK_EDITABLE (entry), pos, -1);
				break;
			}
			pos++;
		}
	}
	else
	{
		setup_set_str (&setup_prefs, set, hc_entry_get_text (GTK_WIDGET (entry)));
	}
}

static void
setup_create_label (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *label = setup_create_italic_label (_(set->label));
	gtk_widget_set_hexpand (label, TRUE);
	gtk_grid_attach (GTK_GRID (table), label, set->extra ? 1 : 3, row, set->extra ? 4 : 2, 1);
}

static GtkWidget *
setup_create_entry (GtkWidget *table, int row, const setting *set)
{
	GtkWidget *label;
	GtkWidget *wid, *bwid;

	label = gtk_label_new (_(set->label));
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (table), label, 2, row, 1, 1);

	wid = gtk_entry_new ();
	g_object_set_data (G_OBJECT (wid), "lbl", label);
	if (set->list)
		gtk_entry_set_visibility (GTK_ENTRY (wid), FALSE);
	if (set->tooltip)
		gtk_widget_set_tooltip_text (wid, _(set->tooltip));
	gtk_entry_set_max_length (GTK_ENTRY (wid), set->extra - 1);
	hc_entry_set_text (wid, setup_get_str (&setup_prefs, set));
	g_signal_connect (G_OBJECT (wid), "changed",
							G_CALLBACK (setup_entry_cb), (gpointer)set);

	if (set->offset == P_OFFSETNL(hex_net_proxy_user))
		proxy_user = wid;
	if (set->offset == P_OFFSETNL(hex_net_proxy_pass))
		proxy_pass = wid;

	/* only http and Socks5 can auth */
	if ( (set->offset == P_OFFSETNL(hex_net_proxy_pass) ||
			set->offset == P_OFFSETNL(hex_net_proxy_user)) &&
	     (setup_prefs.hex_net_proxy_type != 4 && setup_prefs.hex_net_proxy_type != 3 && setup_prefs.hex_net_proxy_type != 5) )
		gtk_widget_set_sensitive (wid, FALSE);

	if (set->type == ST_ENTRY)
	{
		gtk_widget_set_hexpand (wid, TRUE);
		gtk_grid_attach (GTK_GRID (table), wid, 3, row, 3, 1);
	}
	else
	{
		gtk_widget_set_hexpand (wid, TRUE);
		gtk_grid_attach (GTK_GRID (table), wid, 3, row, 2, 1);
		bwid = gtk_button_new_with_label (_("Browse..."));
		gtk_grid_attach (GTK_GRID (table), bwid, 5, row, 1, 1);
		if (set->type == ST_EFILE)
			g_signal_connect (G_OBJECT (bwid), "clicked",
									G_CALLBACK (setup_browsefile_cb), wid);
		if (set->type == ST_EFONT)
			g_signal_connect (G_OBJECT (bwid), "clicked",
									G_CALLBACK (setup_browsefont_cb), wid);
		if (set->type == ST_EFOLDER)
			g_signal_connect (G_OBJECT (bwid), "clicked",
									G_CALLBACK (setup_browsefolder_cb), wid);
	}

	return wid;
}

static void
setup_create_header (GtkWidget *table, int row, char *labeltext)
{
	GtkWidget *label;
	char buf[128];

	if (row == 0)
		g_snprintf (buf, sizeof (buf), "<b>%s</b>", _(labeltext));
	else
		g_snprintf (buf, sizeof (buf), "\n<b>%s</b>", _(labeltext));

	label = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (label), buf);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (label, 5);
	gtk_widget_set_margin_bottom (label, 5);
	gtk_grid_attach (GTK_GRID (table), label, 0, row, 4, 1);
}

static void
setup_create_button (GtkWidget *table, int row, char *label, GCallback callback)
{
	GtkWidget *but = gtk_button_new_with_label (label);
	gtk_widget_set_margin_top (but, 5);
	gtk_widget_set_margin_bottom (but, 5);
	gtk_grid_attach (GTK_GRID (table), but, 2, row, 1, 1);
	g_signal_connect (G_OBJECT (but), "clicked", callback, NULL);
}

static GtkWidget *
setup_create_frame (void)
{
	GtkWidget *tab;

	tab = gtk_grid_new ();
	hc_widget_set_margin_all (tab, 6);
	gtk_grid_set_row_spacing (GTK_GRID (tab), 2);
	gtk_grid_set_column_spacing (GTK_GRID (tab), 3);

	return tab;
}

static void
open_data_cb (GtkWidget *button, gpointer data)
{
	fe_open_url (get_xdir ());
}

static GtkWidget *
setup_create_page (const setting *set)
{
	int i, row, do_disable;
	GtkWidget *tab;
	GtkWidget *wid = NULL, *parentwid = NULL;

	tab = setup_create_frame ();
	hc_widget_set_margin_all (tab, 6);

	i = row = do_disable = 0;
	while (set[i].type != ST_END)
	{
		switch (set[i].type)
		{
		case ST_HEADER:
			setup_create_header (tab, row, set[i].label);
			break;
		case ST_EFONT:
		case ST_ENTRY:
		case ST_EFILE:
		case ST_EFOLDER:
			wid = setup_create_entry (tab, row, &set[i]);
			break;
		case ST_TOGGLR:
			row--;
			setup_create_toggleR (tab, row, &set[i]);
			break;
		case ST_TOGGLE:
			wid = setup_create_toggleL (tab, row, &set[i]);
			if (set[i].extra)
				do_disable = set[i].extra;
			break;
		case ST_3OGGLE:
			setup_create_3oggle (tab, row, &set[i]);
			break;
		case ST_MENU:
			setup_create_menu (tab, row, &set[i]);
			break;
		case ST_RADIO:
			row += setup_create_radio (tab, row, &set[i]);
			break;
		case ST_NUMBER:
			wid = setup_create_spin (tab, row, &set[i]);
			break;
		case ST_HSCALE:
			setup_create_hscale (tab, row, &set[i]);
			break;
		case ST_LABEL:
			setup_create_label (tab, row, &set[i]);
			break;
		case ST_ALERTHEAD:
			setup_create_alert_header (tab, row, &set[i]);
		}

		if (do_disable)
		{
			if (GTK_IS_WIDGET (parentwid))
			{
				g_signal_connect (G_OBJECT (parentwid), "toggled", G_CALLBACK(setup_toggle_sensitive_cb), wid);
				gtk_widget_set_sensitive (wid, gtk_check_button_get_active (GTK_CHECK_BUTTON (parentwid)));
				do_disable--;
				if (!do_disable)
					parentwid = NULL;
			}
			else
				parentwid = wid;
		}

		i++;
		row++;
	}

	if (set == logging_settings)
	{
		setup_create_button (tab, row, _("Open Data Folder"), G_CALLBACK(open_data_cb));
	}

	return tab;
}

/* Helper function to set button background color using CSS.
 * Uses per-widget CssProvider stored via g_object_set_data to avoid leaking
 * providers on repeated color changes. The style_context API has no
 * non-deprecated replacement in GTK 4.14 — suppress until one exists. */
static void
setup_color_button_set_color (GtkWidget *button, GdkRGBA *col)
{
	GtkCssProvider *provider;
	char css[192];

	provider = g_object_get_data (G_OBJECT (button), "hc-css-provider");
	if (!provider)
	{
		provider = gtk_css_provider_new ();
		g_object_set_data_full (G_OBJECT (button), "hc-css-provider",
			provider, g_object_unref);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_style_context_add_provider (gtk_widget_get_style_context (button),
			GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
G_GNUC_END_IGNORE_DEPRECATIONS
	}

	g_snprintf (css, sizeof (css),
		"button { background-color: rgba(%d, %d, %d, %g); background-image: none; "
		"min-width: 0; min-height: 0; padding: 4px 6px; }",
		(int)(col->red * 255), (int)(col->green * 255), (int)(col->blue * 255), col->alpha);

	gtk_css_provider_load_from_string (provider, css);
}

/* Callback for GtkColorDialog async completion */
static void
setup_color_dialog_finish_cb (GObject *source, GAsyncResult *result, gpointer userdata)
{
	GtkColorDialog *dialog = GTK_COLOR_DIALOG (source);
	GtkWidget *button = GTK_WIDGET (userdata);
	GdkRGBA *color;
	GdkRGBA *chosen_color;
	GError *error = NULL;

	chosen_color = gtk_color_dialog_choose_rgba_finish (dialog, result, &error);

	if (chosen_color != NULL)
	{
		color = g_object_get_data (G_OBJECT (button), "color_ptr");
		if (color != NULL)
		{
			color_change = TRUE;
			*color = *chosen_color;
			setup_color_button_set_color (button, color);
		}
		gdk_rgba_free (chosen_color);
	}
	else if (error != NULL)
	{
		/* User cancelled or error occurred - ignore cancellation errors */
		if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
		    !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
		{
			g_warning ("Color dialog error: %s", error->message);
		}
		g_error_free (error);
	}

	g_object_unref (dialog);
}

static void
setup_color_cb (GtkWidget *button, gpointer userdata)
{
	GtkColorDialog *dialog;
	GdkRGBA *color;

	color = &colors[GPOINTER_TO_INT (userdata)];

	/* Store color pointer on button for use in callback */
	g_object_set_data (G_OBJECT (button), "color_ptr", color);

	dialog = gtk_color_dialog_new ();
	gtk_color_dialog_set_title (dialog, _("Select color"));
	gtk_color_dialog_set_modal (dialog, TRUE);
	gtk_color_dialog_set_with_alpha (dialog, FALSE);

	gtk_color_dialog_choose_rgba (dialog, GTK_WINDOW (setup_window), color, NULL,
	                              setup_color_dialog_finish_cb, button);
}

static void
setup_create_color_button (GtkWidget *table, int num, int row, int col)
{
	GtkWidget *but;
	char buf[64];

	if (num > 31)
		strcpy (buf, "<span size=\"x-small\"> </span>");
	else
						/* 12345678901 23456789 01  23456789 */
		sprintf (buf, "<span size=\"x-small\">%d</span>", num);
	but = gtk_button_new_with_label (" ");
	{
		GtkWidget *label = gtk_button_get_child (GTK_BUTTON (but));
		if (label && GTK_IS_LABEL (label))
			gtk_label_set_markup (GTK_LABEL (label), buf);
	}
	/* win32 build uses this to turn off themeing */
	g_object_set_data (G_OBJECT (but), "poxchat-color", (gpointer)1);
	gtk_grid_attach (GTK_GRID (table), but, col, row, 1, 1);
	g_signal_connect (G_OBJECT (but), "clicked",
							G_CALLBACK (setup_color_cb), GINT_TO_POINTER (num));
	setup_color_button_set_color (but, &colors[num]);
}

static void
setup_create_other_colorR (char *text, int num, int row, GtkWidget *tab)
{
	GtkWidget *label;

	label = gtk_label_new (text);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), label, 5, row, 4, 1);
	setup_create_color_button (tab, num, row, 9);
}

static void
setup_create_other_color (char *text, int num, int row, GtkWidget *tab)
{
	GtkWidget *label;

	label = gtk_label_new (text);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), label, 2, row, 1, 1);
	setup_create_color_button (tab, num, row, 3);
}

static GtkWidget *
setup_create_color_page (void)
{
	GtkWidget *tab, *box, *label;
	int i;

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	hc_widget_set_margin_all (box, 6);

	tab = gtk_grid_new ();
	hc_widget_set_margin_all (tab, 6);
	gtk_grid_set_row_spacing (GTK_GRID (tab), 2);
	gtk_grid_set_column_spacing (GTK_GRID (tab), 3);
	gtk_box_append (GTK_BOX (box), tab);

	setup_create_header (tab, 0, N_("Text Colors"));

	label = gtk_label_new (_("mIRC colors:"));
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), label, 2, 1, 1, 1);

	for (i = 0; i < 16; i++)
		setup_create_color_button (tab, i, 1, i+3);

	label = gtk_label_new (_("Local colors:"));
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, LABEL_INDENT);
	gtk_grid_attach (GTK_GRID (tab), label, 2, 2, 1, 1);

	for (i = 16; i < 32; i++)
		setup_create_color_button (tab, i, 2, (i+3) - 16);

	setup_create_other_color (_("Foreground:"), COL_FG, 3, tab);
	setup_create_other_colorR (_("Background:"), COL_BG, 3, tab);

	setup_create_header (tab, 5, N_("Selected Text"));

	setup_create_other_color (_("Foreground:"), COL_MARK_FG, 6, tab);
	setup_create_other_colorR (_("Background:"), COL_MARK_BG, 6, tab);

	setup_create_header (tab, 8, N_("Interface Colors"));

	setup_create_other_color (_("New data:"), COL_NEW_DATA, 9, tab);
	setup_create_other_colorR (_("Marker line:"), COL_MARKER, 9, tab);
	setup_create_other_color (_("New message:"), COL_NEW_MSG, 10, tab);
	setup_create_other_colorR (_("Away user:"), COL_AWAY, 10, tab);
	setup_create_other_color (_("Highlight:"), COL_HILIGHT, 11, tab);
	setup_create_other_colorR (_("Spell checker:"), COL_SPELL, 11, tab);

	setup_create_header (tab, 15, N_("Color Stripping"));

	/* label = gtk_label_new (_("Strip colors from:"));
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_table_attach (GTK_TABLE (tab), label, 2, 3, 16, 17,
							GTK_SHRINK | GTK_FILL, GTK_SHRINK | GTK_FILL, LABEL_INDENT, 0); */

	for (i = 0; i < 3; i++)
	{
		setup_create_toggleL (tab, i + 16, &color_settings[i]);
	}

	return box;
}

/* === GLOBALS for sound GUI === */

static GtkWidget *sndfile_entry;
static int ignore_changed = FALSE;

extern struct text_event te[]; /* text.c */
extern char *sound_files[];

/* Sound events item for GtkColumnView */
#define HC_TYPE_SOUND_ITEM (hc_sound_item_get_type())
G_DECLARE_FINAL_TYPE (HcSoundItem, hc_sound_item, HC, SOUND_ITEM, GObject)

struct _HcSoundItem {
	GObject parent;
	char *event_name;
	char *sound_file;
	int index;
};

G_DEFINE_TYPE (HcSoundItem, hc_sound_item, G_TYPE_OBJECT)

static void
hc_sound_item_finalize (GObject *obj)
{
	HcSoundItem *item = HC_SOUND_ITEM (obj);
	g_free (item->event_name);
	g_free (item->sound_file);
	G_OBJECT_CLASS (hc_sound_item_parent_class)->finalize (obj);
}

static void hc_sound_item_class_init (HcSoundItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_sound_item_finalize; }
static void hc_sound_item_init (HcSoundItem *item) { }

static HcSoundItem *
hc_sound_item_new (const char *event_name, const char *sound_file, int index)
{
	HcSoundItem *item = g_object_new (HC_TYPE_SOUND_ITEM, NULL);
	item->event_name = g_strdup (event_name ? event_name : "");
	item->sound_file = g_strdup (sound_file ? sound_file : "");
	item->index = index;
	return item;
}

/* Settings category item for GtkListView tree */
#define HC_TYPE_SETTINGS_CAT (hc_settings_cat_get_type())
G_DECLARE_FINAL_TYPE (HcSettingsCat, hc_settings_cat, HC, SETTINGS_CAT, GObject)

struct _HcSettingsCat {
	GObject parent;
	char *name;
	int page_index;		/* -1 for header categories */
	GListStore *children;	/* non-NULL for parent categories */
};

G_DEFINE_TYPE (HcSettingsCat, hc_settings_cat, G_TYPE_OBJECT)

static void
hc_settings_cat_finalize (GObject *obj)
{
	HcSettingsCat *item = HC_SETTINGS_CAT (obj);
	g_free (item->name);
	g_clear_object (&item->children);
	G_OBJECT_CLASS (hc_settings_cat_parent_class)->finalize (obj);
}

static void hc_settings_cat_class_init (HcSettingsCatClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_settings_cat_finalize; }
static void hc_settings_cat_init (HcSettingsCat *item) { }

static HcSettingsCat *
hc_settings_cat_new (const char *name, int page_index)
{
	HcSettingsCat *item = g_object_new (HC_TYPE_SETTINGS_CAT, NULL);
	item->name = g_strdup (name ? name : "");
	item->page_index = page_index;
	item->children = NULL;
	return item;
}

static GListStore *sound_store = NULL;
static GtkWidget *sound_column_view = NULL;

static void
setup_snd_populate (void)
{
	GtkSelectionModel *sel_model;
	int i;

	g_list_store_remove_all (sound_store);

	for (i = 0; i < NUM_XP; i++)
	{
		HcSoundItem *item = hc_sound_item_new (te[i].name, sound_files[i], i);
		g_list_store_append (sound_store, item);
		g_object_unref (item);
	}

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (sound_column_view));
	if (last_selected_row >= 0 && last_selected_row < NUM_XP)
		gtk_selection_model_select_item (sel_model, last_selected_row, TRUE);
	else if (g_list_model_get_n_items (G_LIST_MODEL (sound_store)) > 0)
		gtk_selection_model_select_item (sel_model, 0, TRUE);
}

static void
setup_snd_row_cb (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	guint pos;
	HcSoundItem *item;

	(void)position; (void)n_items; (void)user_data;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (sound_store), pos);
	if (!item)
		return;

	last_selected_row = item->index;

	ignore_changed = TRUE;
	if (sound_files[item->index])
		hc_entry_set_text (sndfile_entry, sound_files[item->index]);
	else
		hc_entry_set_text (sndfile_entry, "");
	ignore_changed = FALSE;

	g_object_unref (item);
}

/* Factory callbacks for sound event name column (read-only) */
static void
setup_snd_event_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_list_item_set_child (list_item, label);
}

static void
setup_snd_event_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcSoundItem *item = gtk_list_item_get_item (list_item);
	gtk_label_set_text (GTK_LABEL (label), item->event_name);
}

/* Factory callbacks for sound file column (read-only) */
static void
setup_snd_file_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_list_item_set_child (list_item, label);
}

static void
setup_snd_file_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcSoundItem *item = gtk_list_item_get_item (list_item);
	gtk_label_set_text (GTK_LABEL (label), item->sound_file);
}

static void
setup_snd_filereq_cb (GtkWidget *entry, char *file)
{
	if (file)
	{
		if (file[0])
		{
			/* Use just the filename if the given sound file is in the default <config>/sounds directory.
			 * We're comparing absolute paths so this won't work in portable mode which uses a relative path.
			 */
			if (!strcmp (g_path_get_dirname (file), g_build_filename (get_xdir (), POXCHAT_SOUND_DIR, NULL)))
			{
				hc_entry_set_text (entry, g_path_get_basename (file));
			}
			else
			{
				hc_entry_set_text (entry, file);
			}
		}
	}
}

static void
setup_snd_browse_cb (GtkWidget *button, GtkEntry *entry)
{
	char *sounds_dir = g_build_filename (get_xdir (), POXCHAT_SOUND_DIR, NULL);
	char *filter = NULL;
	int filter_type;
#ifdef WIN32 /* win32 only supports wav, others could support anything */
	filter = "*.wav";
	filter_type = FRF_EXTENSIONS;
#else
	filter = "audio/*";
	filter_type = FRF_MIMETYPES;
#endif

	gtkutil_file_req (GTK_WINDOW (setup_window), _("Select a sound file"), setup_snd_filereq_cb, entry,
						sounds_dir, filter, FRF_MODAL|FRF_FILTERISINITIAL|filter_type);
	g_free (sounds_dir);
}

static void
setup_snd_play_cb (GtkWidget *button, GtkEntry *entry)
{
	sound_play (hc_entry_get_text (GTK_WIDGET (entry)), FALSE);
}

static void
setup_snd_changed_cb (GtkEntry *ent, gpointer user_data)
{
	GtkSelectionModel *sel_model;
	guint pos;
	HcSoundItem *item;

	if (ignore_changed)
		return;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (sound_column_view));
	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	item = g_list_model_get_item (G_LIST_MODEL (sound_store), pos);
	if (!item)
		return;

	/* get the new sound file */
	g_free (sound_files[item->index]);
	sound_files[item->index] = g_strdup (hc_entry_get_text (GTK_WIDGET (ent)));

	/* update the item and force rebind */
	g_free (item->sound_file);
	item->sound_file = g_strdup (sound_files[item->index]);
	g_object_ref (item);
	g_list_store_remove (sound_store, pos);
	g_list_store_insert (sound_store, pos, item);
	g_object_unref (item);
	g_object_unref (item);
	gtk_selection_model_select_item (sel_model, pos, TRUE);

	gtk_widget_set_sensitive (cancel_button, FALSE);
}

static GtkWidget *
setup_create_sound_page (void)
{
	GtkWidget *vbox1;
	GtkWidget *vbox2;
	GtkWidget *scrolledwindow1;
	GtkWidget *table1;
	GtkWidget *sound_label;
	GtkWidget *sound_browse;
	GtkWidget *sound_play;
	GtkListItemFactory *factory;
	GtkColumnViewColumn *col;
	GtkSelectionModel *sel_model;

	vbox1 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	hc_widget_set_margin_all (vbox1, 6);

	vbox2 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append (GTK_BOX (vbox1), vbox2);

	scrolledwindow1 = gtk_scrolled_window_new ();
	gtk_widget_set_vexpand (scrolledwindow1, TRUE);
	gtk_box_append (GTK_BOX (vbox2), scrolledwindow1);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow1),
											  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	/* Sound Events (GtkColumnView) */
	sound_store = g_list_store_new (HC_TYPE_SOUND_ITEM);
	sound_column_view = hc_column_view_new_simple (G_LIST_MODEL (sound_store), GTK_SELECTION_SINGLE);

	/* Event name column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (sound_column_view), _("Event"),
	                                  G_CALLBACK (setup_snd_event_setup_cb),
	                                  G_CALLBACK (setup_snd_event_bind_cb), NULL, NULL);
	gtk_column_view_column_set_expand (col, TRUE);

	/* Sound file column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (sound_column_view), _("Sound file"),
	                                  G_CALLBACK (setup_snd_file_setup_cb),
	                                  G_CALLBACK (setup_snd_file_bind_cb), NULL, NULL);
	gtk_column_view_column_set_expand (col, TRUE);

	setup_snd_populate ();

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (sound_column_view));
	g_signal_connect (sel_model, "selection-changed", G_CALLBACK (setup_snd_row_cb), NULL);

	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolledwindow1), sound_column_view);

	table1 = gtk_grid_new ();
	gtk_widget_set_margin_top (table1, 8);
	gtk_box_append (GTK_BOX (vbox2), table1);
	gtk_grid_set_row_spacing (GTK_GRID (table1), 2);
	gtk_grid_set_column_spacing (GTK_GRID (table1), 4);

	sound_label = gtk_label_new_with_mnemonic (_("Sound file:"));
	gtk_widget_set_halign (sound_label, GTK_ALIGN_START);
	gtk_widget_set_valign (sound_label, GTK_ALIGN_CENTER);
	gtk_grid_attach (GTK_GRID (table1), sound_label, 0, 0, 1, 1);

	sndfile_entry = gtk_entry_new ();
	g_signal_connect (G_OBJECT (sndfile_entry), "changed",
							G_CALLBACK (setup_snd_changed_cb), NULL);
	gtk_widget_set_hexpand (sndfile_entry, TRUE);
	gtk_grid_attach (GTK_GRID (table1), sndfile_entry, 0, 1, 1, 1);

	sound_browse = gtk_button_new_with_mnemonic (_("_Browse..."));
	g_signal_connect (G_OBJECT (sound_browse), "clicked",
							G_CALLBACK (setup_snd_browse_cb), sndfile_entry);
	gtk_grid_attach (GTK_GRID (table1), sound_browse, 1, 1, 1, 1);

	sound_play = gtk_button_new_from_icon_name ("media-playback-start");
	g_signal_connect (G_OBJECT (sound_play), "clicked",
							G_CALLBACK (setup_snd_play_cb), sndfile_entry);
	gtk_grid_attach (GTK_GRID (table1), sound_play, 2, 1, 1, 1);

	/* Trigger initial selection update */
	setup_snd_row_cb (sel_model, 0, 0, NULL);

	return vbox1;
}

static void
setup_add_page (const char *title, GtkWidget *book, GtkWidget *tab)
{
	GtkWidget *label, *vvbox, *viewport;
	GtkScrolledWindow *sw;
	char buf[128];

	vvbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

	/* label */
	label = gtk_label_new (NULL);
	g_snprintf (buf, sizeof (buf), "<b><big>%s</big></b>", _(title));
	gtk_label_set_markup (GTK_LABEL (label), buf);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start (label, 2);
	gtk_widget_set_margin_end (label, 2);
	gtk_widget_set_margin_top (label, 1);
	gtk_widget_set_margin_bottom (label, 1);
	gtk_widget_set_margin_bottom (label, 2);
	gtk_box_append (GTK_BOX (vvbox), label);

	/* Settings grid should expand to fill available space */
	gtk_widget_set_hexpand (tab, TRUE);
	gtk_widget_set_vexpand (tab, TRUE);
	gtk_box_append (GTK_BOX (vvbox), tab);

	sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new ());
	gtk_scrolled_window_set_policy (sw, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	viewport = gtk_viewport_new (NULL, NULL);
	gtk_viewport_set_child (GTK_VIEWPORT (viewport), vvbox);
	gtk_scrolled_window_set_child (sw, viewport);

	hc_page_container_append (book, GTK_WIDGET(sw));
}

static const char *const cata[] =
{
	N_("Interface"),
		N_("Appearance"),
		N_("Input box"),
		N_("User list"),
		N_("Channel switcher"),
		N_("Colors"),
		NULL,
	N_("Chatting"),
		N_("General"),
		N_("Alerts"),
		N_("Sounds"),
		N_("Logging"),
		N_("Advanced"),
		NULL,
	N_("Network"),
		N_("Network setup"),
		N_("File transfers"),
		N_("Identd"),
		NULL,
	NULL
};

static GtkWidget *
setup_create_pages (GtkWidget *box)
{
	GtkWidget *book;
	GtkWindow *win = GTK_WINDOW(gtk_widget_get_root (box));

	book = hc_page_container_new ();

	setup_add_page (cata[1], book, setup_create_page (appearance_settings));
	setup_add_page (cata[2], book, setup_create_page (inputbox_settings));
	setup_add_page (cata[3], book, setup_create_page (userlist_settings));
	setup_add_page (cata[4], book, setup_create_page (tabs_settings));
	setup_add_page (cata[5], book, setup_create_color_page ());

	setup_add_page (cata[8], book, setup_create_page (general_settings));

	if (!gtkutil_tray_icon_supported (win) && !notification_backend_supported ())
	{
		setup_add_page (cata[9], book, setup_create_page (alert_settings_unityandnonotifications));
	}
	else if (!gtkutil_tray_icon_supported (win))
	{
		setup_add_page (cata[9], book, setup_create_page (alert_settings_unity));
	}
	else if (!notification_backend_supported ())
	{
		setup_add_page (cata[9], book, setup_create_page (alert_settings_nonotifications));
	}
	else
	{
		setup_add_page (cata[9], book, setup_create_page (alert_settings));
	}

	setup_add_page (cata[10], book, setup_create_sound_page ());
	setup_add_page (cata[11], book, setup_create_page (logging_settings));
	setup_add_page (cata[12], book, setup_create_page (advanced_settings));

	setup_add_page (cata[15], book, setup_create_page (network_settings));
	setup_add_page (cata[16], book, setup_create_page (filexfer_settings));
	setup_add_page (cata[17], book, setup_create_page (identd_settings));

	/* Page container should expand to fill available space */
	gtk_widget_set_hexpand (book, TRUE);
	gtk_widget_set_vexpand (book, TRUE);
	gtk_box_append (GTK_BOX (box), book);

	return book;
}

/* GtkTreeListModel child model callback */
static GListModel *
setup_tree_create_child_model (gpointer item, gpointer user_data)
{
	HcSettingsCat *cat = HC_SETTINGS_CAT (item);
	if (cat->children)
		return G_LIST_MODEL (g_object_ref (cat->children));
	return NULL;
}

/* Factory setup: GtkTreeExpander with label */
static void
setup_tree_factory_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *expander, *label;

	expander = gtk_tree_expander_new ();
	gtk_tree_expander_set_indent_for_depth (GTK_TREE_EXPANDER (expander), TRUE);
	gtk_tree_expander_set_hide_expander (GTK_TREE_EXPANDER (expander), TRUE);
	gtk_widget_set_hexpand (expander, TRUE);

	label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_widget_set_hexpand (label, TRUE);

	gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), label);
	gtk_list_item_set_child (list_item, expander);

	g_object_set_data (G_OBJECT (list_item), "expander", expander);
	g_object_set_data (G_OBJECT (list_item), "label", label);
}

/* Factory bind: set text, bold for headers */
static void
setup_tree_factory_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *expander, *label;
	GtkTreeListRow *row;
	HcSettingsCat *cat;

	expander = g_object_get_data (G_OBJECT (list_item), "expander");
	label = g_object_get_data (G_OBJECT (list_item), "label");
	if (!expander || !label)
		return;

	row = gtk_list_item_get_item (list_item);
	if (!row)
		return;

	gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

	cat = gtk_tree_list_row_get_item (row);
	if (!cat)
		return;

	gtk_label_set_text (GTK_LABEL (label), cat->name);

	/* Bold headers, normal children */
	if (cat->page_index == -1)
	{
		PangoAttrList *attrs = pango_attr_list_new ();
		pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
		gtk_label_set_attributes (GTK_LABEL (label), attrs);
		pango_attr_list_unref (attrs);
		gtk_list_item_set_selectable (list_item, FALSE);
	}
	else
	{
		gtk_label_set_attributes (GTK_LABEL (label), NULL);
		gtk_list_item_set_selectable (list_item, TRUE);
	}

	g_object_unref (cat);
}

/* Factory unbind */
static void
setup_tree_factory_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *expander = g_object_get_data (G_OBJECT (list_item), "expander");
	if (expander)
		gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);
}

/* Selection changed: switch page */
static void
setup_tree_sel_cb (GtkSelectionModel *sel_model, guint position, guint n_items, GtkWidget *book)
{
	guint pos;
	GtkTreeListRow *row;
	HcSettingsCat *cat;

	(void)position; (void)n_items;

	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	row = g_list_model_get_item (G_LIST_MODEL (sel_model), pos);
	if (!row)
		return;

	cat = gtk_tree_list_row_get_item (row);
	g_object_unref (row);
	if (!cat)
		return;

	if (cat->page_index != -1)
	{
		hc_page_container_set_current_page (book, cat->page_index);
		last_selected_page = cat->page_index;
	}

	g_object_unref (cat);
}

static void
setup_create_tree (GtkWidget *box, GtkWidget *book)
{
	GtkWidget *view, *frame;
	GtkListItemFactory *factory;
	GtkTreeListModel *tree_model;
	GtkSingleSelection *sel_model;
	GListStore *root_store;
	int i, page;
	guint sel_pos = 0;

	/* Build the category model */
	root_store = g_list_store_new (HC_TYPE_SETTINGS_CAT);

	i = 0;
	page = 0;
	do
	{
		HcSettingsCat *parent = hc_settings_cat_new (_(cata[i]), -1);
		parent->children = g_list_store_new (HC_TYPE_SETTINGS_CAT);
		i++;

		do
		{
			HcSettingsCat *child = hc_settings_cat_new (_(cata[i]), page);
			g_list_store_append (parent->children, child);
			g_object_unref (child);
			page++;
			i++;
		} while (cata[i]);

		g_list_store_append (root_store, parent);
		g_object_unref (parent);
		i++;

	} while (cata[i]);

	/* Create tree list model (autoexpand) */
	tree_model = gtk_tree_list_model_new (
		G_LIST_MODEL (root_store),
		FALSE,	/* passthrough */
		TRUE,	/* autoexpand */
		setup_tree_create_child_model,
		NULL, NULL);

	/* Find the position of last_selected_page in the flattened model */
	{
		guint n = g_list_model_get_n_items (G_LIST_MODEL (tree_model));
		for (guint j = 0; j < n; j++)
		{
			GtkTreeListRow *row = g_list_model_get_item (G_LIST_MODEL (tree_model), j);
			if (row)
			{
				HcSettingsCat *cat = gtk_tree_list_row_get_item (row);
				if (cat && cat->page_index == last_selected_page)
				{
					sel_pos = j;
					g_object_unref (cat);
					g_object_unref (row);
					break;
				}
				if (cat) g_object_unref (cat);
				g_object_unref (row);
			}
		}
	}

	sel_model = gtk_single_selection_new (G_LIST_MODEL (tree_model));

	/* Factory */
	factory = gtk_signal_list_item_factory_new ();
	g_signal_connect (factory, "setup", G_CALLBACK (setup_tree_factory_setup_cb), NULL);
	g_signal_connect (factory, "bind", G_CALLBACK (setup_tree_factory_bind_cb), NULL);
	g_signal_connect (factory, "unbind", G_CALLBACK (setup_tree_factory_unbind_cb), NULL);

	/* Create list view */
	view = gtk_list_view_new (GTK_SELECTION_MODEL (sel_model), factory);
	gtk_widget_set_name (view, "poxchat-list");

	g_signal_connect (sel_model, "selection-changed",
	                  G_CALLBACK (setup_tree_sel_cb), book);

	frame = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (frame),
	                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (frame), view);
	gtk_widget_set_vexpand (frame, TRUE);
	gtk_box_append (GTK_BOX (box), frame);
	hc_box_reorder_child (GTK_BOX (box), frame, 0);

	/* Select remembered page */
	gtk_single_selection_set_selected (sel_model, sel_pos);
}

static void
setup_apply_to_sess (session_gui *gui)
{
	mg_update_xtext (gui->xtext);

	/* Font and colors for input boxes, userlist, and tree are applied via
	 * CSS providers (create_input_style / apply_tree_css) - no per-widget
	 * override needed in GTK4. */

	if (prefs.hex_gui_ulist_buttons)
		gtk_widget_set_visible (gui->button_box, TRUE);
	else
		gtk_widget_set_visible (gui->button_box, FALSE);

	/* update active languages */
	hex_input_edit_deactivate_language (HEX_INPUT_EDIT (gui->input_box), NULL);
	hex_input_edit_activate_default_languages (HEX_INPUT_EDIT (gui->input_box));
	hex_input_edit_set_max_lines (HEX_INPUT_EDIT (gui->input_box), prefs.hex_gui_input_lines);
	hex_input_edit_set_checked (HEX_INPUT_EDIT (gui->input_box), prefs.hex_gui_input_spell);
	hex_input_edit_set_emoji_cache (HEX_INPUT_EDIT (gui->input_box),
	                                GTK_XTEXT (gui->xtext)->emoji_cache);
	hex_input_edit_set_palette (HEX_INPUT_EDIT (gui->input_box),
	                            GTK_XTEXT (gui->xtext)->palette);
	if (input_style && input_style->font_desc)
	{
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->input_box), input_style->font_desc);
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->topic_entry), input_style->font_desc);
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->key_entry), input_style->font_desc);
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->limit_entry), input_style->font_desc);
	}
}

static void
unslash (char *dir)
{
	if (dir[0])
	{
		int len = strlen (dir) - 1;
#ifdef WIN32
		if (dir[len] == '/' || dir[len] == '\\')
#else
		if (dir[len] == '/')
#endif
			dir[len] = 0;
	}
}

void
setup_apply_real (int new_pix, int do_ulist, int do_layout, int do_meters, int do_identd)
{
	GSList *list;
	session *sess;
	int done_main = FALSE;

	/* remove trailing slashes */
	unslash (prefs.hex_dcc_dir);
	unslash (prefs.hex_dcc_completed_dir);

	g_mkdir (prefs.hex_dcc_dir, 0700);
	g_mkdir (prefs.hex_dcc_completed_dir, 0700);

	if (new_pix)
	{
		if (channelwin_pix)
			g_object_unref (channelwin_pix);
		channelwin_pix = pixmap_load_from_file (prefs.hex_text_background);
	}

	input_style = create_input_style (input_style);
	apply_tree_css ();

	/* Apply userlist pref changes (column visibility, sort, count) early
	 * so they're visible before the slower per-session xtext updates. */
	if (current_tab)
		userlist_apply_prefs (current_tab);

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->gui->is_tab)
		{
			/* only apply to main tabwindow once */
			if (!done_main)
			{
				done_main = TRUE;
				setup_apply_to_sess (sess->gui);
				if (do_meters)
					mg_update_meters (sess->gui);
			}
		} else
		{
			setup_apply_to_sess (sess->gui);
			if (do_meters)
				mg_update_meters (sess->gui);
		}

		log_open_or_close (sess);

		if (do_ulist)
			userlist_rehash (sess);

		list = list->next;
	}

	mg_apply_setup ();
	menu_sync_toggle_states ();
	tray_apply_setup ();
	poxchat_reinit_timers ();

	if (do_layout)
		menu_change_layout ();

	if (do_identd)
		handle_command (current_sess, "IDENTD reload", FALSE);
}

static void
setup_apply (struct poxchatprefs *pr)
{
	PangoFontDescription *old_desc;
	PangoFontDescription *new_desc;
	char buffer[4 * FONTNAMELEN + 1];
	int new_pix = FALSE;
	int noapply = FALSE;
	int do_ulist = FALSE;
	int do_layout = FALSE;
	int do_meters = FALSE;
	int do_identd = FALSE;

	if (strcmp (pr->hex_text_background, prefs.hex_text_background) != 0)
		new_pix = TRUE;

#define DIFF(a) (pr->a != prefs.a)

#ifdef WIN32
	if (DIFF (hex_gui_lang))
		noapply = TRUE;
#endif
	if (DIFF (hex_gui_input_icon))
		noapply = TRUE;
	if (DIFF (hex_gui_input_nick))
		noapply = TRUE;
	if (DIFF (hex_gui_lagometer) || DIFF (hex_gui_throttlemeter))
		do_meters = TRUE;
	if (DIFF (hex_gui_tab_icons))
		noapply = TRUE;
	if (DIFF (hex_gui_tab_server))
		noapply = TRUE;
	if (DIFF (hex_gui_tab_small))
		noapply = TRUE;
	if (DIFF (hex_gui_tab_sort))
		noapply = TRUE;
	if (DIFF (hex_gui_tab_trunc))
		noapply = TRUE;
	if (DIFF (hex_gui_input_style) && prefs.hex_gui_input_style == TRUE)
		noapply = TRUE; /* Requires restart to *disable* */

	/* Userlist prefs applied live via userlist_apply_prefs() */
	if (DIFF (hex_gui_ulist_icons))
		do_ulist = TRUE;	/* rehash to update nick text (prefix char) */

	if (DIFF (hex_gui_tab_dots))
		do_layout = TRUE;
	if (DIFF (hex_gui_tab_layout))
		do_layout = TRUE;
	if (DIFF (hex_gui_tab_pos))
		do_layout = TRUE;
	if (DIFF (hex_gui_ulist_pos))
		do_layout = TRUE;

	if (DIFF (hex_identd_server) || DIFF (hex_identd_port))
		do_identd = TRUE;

	if (color_change || (DIFF (hex_gui_ulist_color)) || (DIFF (hex_away_size_max)) || (DIFF (hex_away_track)))
		do_ulist = TRUE;

	if ((pr->hex_gui_tab_pos == 5 || pr->hex_gui_tab_pos == 6) &&
		 pr->hex_gui_tab_layout == 2 && pr->hex_gui_tab_pos != prefs.hex_gui_tab_pos)
		fe_message (_("You cannot place the tree on the top or bottom!\n"
						"Please change to the <b>Tabs</b> layout in the <b>View</b>"
						" menu first."),
						FE_MSG_WARN | FE_MSG_MARKUP);

	/* format cannot be blank, there is already a setting for this */
	if (pr->hex_stamp_text_format[0] == 0)
	{
		pr->hex_stamp_text = 0;
		strcpy (pr->hex_stamp_text_format, prefs.hex_stamp_text_format);
	}

	memcpy (&prefs, pr, sizeof (prefs));

	/* merge hex_font_main and hex_font_alternative into hex_font_normal */
	old_desc = pango_font_description_from_string (prefs.hex_text_font_main);
	sprintf (buffer, "%s,%s", pango_font_description_get_family (old_desc), prefs.hex_text_font_alternative);
	new_desc = pango_font_description_from_string (buffer);
	pango_font_description_set_weight (new_desc, pango_font_description_get_weight (old_desc));
	pango_font_description_set_style (new_desc, pango_font_description_get_style (old_desc));
	pango_font_description_set_size (new_desc, pango_font_description_get_size (old_desc));
	sprintf (prefs.hex_text_font, "%s", pango_font_description_to_string (new_desc));
	pango_font_description_free (old_desc);
	pango_font_description_free (new_desc);

	if (prefs.hex_irc_real_name[0] == 0)
	{
		fe_message (_("The Real name option cannot be left blank. Falling back to \"realname\"."), FE_MSG_WARN);
		strcpy (prefs.hex_irc_real_name, "realname");
	}
	
	setup_apply_real (new_pix, do_ulist, do_layout, do_meters, do_identd);

	if (noapply)
		fe_message (_("Some settings were changed that require a"
						" restart to take full effect."), FE_MSG_WARN);

#ifndef WIN32
	if (prefs.hex_dcc_auto_recv == 2) /* Auto */
	{
		if (!strcmp ((char *)g_get_home_dir (), prefs.hex_dcc_dir))
		{
			fe_message (_("*WARNING*\n"
							 "Auto accepting DCC to your home directory\n"
							 "can be dangerous and is exploitable. Eg:\n"
							 "Someone could send you a .bash_profile"), FE_MSG_WARN);
		}
	}
#endif
}

static void
setup_ok_cb (GtkWidget *but, GtkWidget *win)
{
	/* Set flag to prevent toggle callbacks from corrupting settings during window destruction */
	setup_applying = TRUE;
	hc_window_destroy_fn (GTK_WINDOW (win));

	/* Apply must happen BEFORE memcpy, because setup_apply uses DIFF(a) =
	 * (pr->a != prefs.a) to detect what changed.  If we copy first, all
	 * diffs would be FALSE and nothing would be applied.
	 * setup_apply() does memcpy(prefs, pr) internally and then merges
	 * hex_text_font from hex_text_font_main + alternatives, so we must
	 * NOT overwrite prefs again afterwards — that would clobber the merge. */
	setup_apply (&setup_prefs);
	save_config ();
	palette_save ();

	setup_applying = FALSE;
}

static GtkWidget *
setup_window_open (void)
{
	GtkWidget *wid, *win, *vbox, *hbox, *hbbox;
	char buf[128];

	g_snprintf(buf, sizeof(buf), _("Preferences - %s"), _(DISPLAY_NAME));
	win = gtkutil_window_new (buf, "prefs", 0, 600, 2);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
	hc_widget_set_margin_all (vbox, 12);
	gtk_window_set_child (GTK_WINDOW (win), vbox);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
	/* Main content area should expand to fill available space */
	gtk_widget_set_vexpand (hbox, TRUE);
	gtk_box_append (GTK_BOX (vbox), hbox);

	setup_create_tree (hbox, setup_create_pages (hbox));

	/* prepare the button box */
	hbbox = hc_button_box_new_impl (GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top (hbbox, 12);
	gtk_box_append (GTK_BOX (vbox), hbbox);

	cancel_button = wid = gtk_button_new_with_mnemonic (_("_Cancel"));
	g_signal_connect (G_OBJECT (wid), "clicked",
							G_CALLBACK (gtkutil_destroy), win);
	gtk_box_append (GTK_BOX (hbbox), wid);

	wid = gtk_button_new_with_mnemonic (_("_OK"));
	g_signal_connect (G_OBJECT (wid), "clicked",
							G_CALLBACK (setup_ok_cb), win);
	gtk_box_append (GTK_BOX (hbbox), wid);

	return win;
}

static void
setup_close_cb (GtkWidget *win, GtkWidget **swin)
{
	*swin = NULL;

	if (font_dialog_cancel)
	{
		g_cancellable_cancel (font_dialog_cancel);
		g_clear_object (&font_dialog_cancel);
	}
}

void
setup_open (void)
{
	if (setup_window)
	{
		gtk_window_present (GTK_WINDOW (setup_window));
		return;
	}

	memcpy (&setup_prefs, &prefs, sizeof (prefs));
	hc_debug_log ("setup_open: prefs.hex_gui_ulist_color=%d setup_prefs.hex_gui_ulist_color=%d",
	              prefs.hex_gui_ulist_color, setup_prefs.hex_gui_ulist_color);

	color_change = FALSE;
	setup_window = setup_window_open ();

	g_signal_connect (G_OBJECT (setup_window), "destroy",
							G_CALLBACK (setup_close_cb), &setup_window);
}
