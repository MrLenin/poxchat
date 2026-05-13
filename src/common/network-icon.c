/* PoxChat - IRCv3 draft/ICON Network Icon Support
 * Copyright (C) 2026
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
 * Fetches and caches network icons advertised via the draft/ICON
 * ISUPPORT token. Uses libsoup3 on Linux/macOS and libcurl on Windows.
 * The raw image bytes are passed to the frontend for pixbuf creation.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#ifdef USE_LIBSOUP
#include <libsoup/soup.h>
#endif

#ifdef USE_LIBCURL
#include <curl/curl.h>
#endif

#include "network-icon.h"
#include "poxchat.h"
#include "servlist.h"
#include "poxchatc.h"
#include "cfgfiles.h"
#include "fe.h"
#include "util.h"

/* ---- URL template substitution ---- */

char *
network_icon_resolve_url (const char *url_template, int size_px)
{
	const char *pos;
	GString *result;

	if (!url_template)
		return NULL;

	pos = strstr (url_template, "{size}");
	if (!pos)
		return g_strdup (url_template);

	result = g_string_new (NULL);
	g_string_append_len (result, url_template, pos - url_template);
	g_string_append_printf (result, "%d", size_px);
	g_string_append (result, pos + 6); /* skip "{size}" */
	return g_string_free (result, FALSE);
}

/* ---- Disk cache ----
 *
 * Cache layout: <config>/icons/network/<network_name>/<url_hash>.img
 *
 * The network name directories let us find cached icons at connect time
 * (before we know the URL). The URL hash filename lets us detect URL
 * changes when ISUPPORT arrives — just hash the new URL and compare. */

static char *
network_cache_dir (const char *network_name)
{
	char *dir = g_build_filename (get_xdir (), "icons", "network",
	                               network_name, NULL);
	g_mkdir_with_parents (dir, 0700);
	return dir;
}

/* Find the first .img file in a network's cache directory.
 * Returns the full path, or NULL if no cached icon exists. */
static char *
cache_find (const char *network_name)
{
	char *dir, *path = NULL;
	GDir *gdir;
	const char *entry;

	dir = network_cache_dir (network_name);
	gdir = g_dir_open (dir, 0, NULL);
	if (gdir)
	{
		while ((entry = g_dir_read_name (gdir)) != NULL)
		{
			if (g_str_has_suffix (entry, ".img"))
			{
				path = g_build_filename (dir, entry, NULL);
				break;
			}
		}
		g_dir_close (gdir);
	}
	g_free (dir);
	return path;
}

/* Build cache file path: <network_dir>/<url_hash>.img */
static char *
cache_path (const char *network_name, const char *resolved_url)
{
	char *dir, *hash, *name, *path;

	dir = network_cache_dir (network_name);
	hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, resolved_url, -1);
	name = g_strconcat (hash, ".img", NULL);
	path = g_build_filename (dir, name, NULL);

	g_free (name);
	g_free (hash);
	g_free (dir);
	return path;
}

/* Remove all .img files from a network's cache directory */
static void
cache_clear (const char *network_name)
{
	char *dir;
	GDir *gdir;
	const char *entry;

	dir = network_cache_dir (network_name);
	gdir = g_dir_open (dir, 0, NULL);
	if (gdir)
	{
		while ((entry = g_dir_read_name (gdir)) != NULL)
		{
			if (g_str_has_suffix (entry, ".img"))
			{
				char *path = g_build_filename (dir, entry, NULL);
				g_unlink (path);
				g_free (path);
			}
		}
		g_dir_close (gdir);
	}
	g_free (dir);
}

/* Save icon data to cache, replacing any existing file */
static void
cache_save (const char *network_name, const char *resolved_url,
            const guint8 *data, gsize len)
{
	char *path;

	/* Remove old icon(s) first */
	cache_clear (network_name);

	path = cache_path (network_name, resolved_url);
	g_file_set_contents (path, (const char *)data, len, NULL);
	g_free (path);
}

/* ---- Fetch context (shared by both backends) ---- */

typedef struct {
	struct server *serv;
	char *resolved_url;
	char *network_name;
	gboolean cancelled;
} icon_fetch_ctx;

static void
icon_fetch_ctx_free (icon_fetch_ctx *ctx)
{
	g_free (ctx->resolved_url);
	g_free (ctx->network_name);
	g_free (ctx);
}

/* Common completion: validate, cache, notify frontend with raw bytes */
static void
icon_fetch_complete (icon_fetch_ctx *ctx, const guint8 *data, gsize len,
                     const char *error)
{
	if (ctx->cancelled)
	{
		icon_fetch_ctx_free (ctx);
		return;
	}

	if (error || !data || len == 0)
	{
		ctx->serv->network_icon_cancel = NULL;
		icon_fetch_ctx_free (ctx);
		return;
	}

	/* Enforce size limit */
	if (len > NETWORK_ICON_MAX_SIZE)
	{
		ctx->serv->network_icon_cancel = NULL;
		icon_fetch_ctx_free (ctx);
		return;
	}

	/* Save to disk cache */
	cache_save (ctx->network_name, ctx->resolved_url, data, len);

	ctx->serv->network_icon_cancel = NULL;

	/* Let the frontend create the pixbuf */
	fe_network_icon_ready (ctx->serv, data, len);

	icon_fetch_ctx_free (ctx);
}

/* ---- libsoup3 backend ---- */

#ifdef USE_LIBSOUP

static void
icon_soup_callback (GObject *source, GAsyncResult *result, gpointer user_data)
{
	icon_fetch_ctx *ctx = (icon_fetch_ctx *)user_data;
	SoupSession *session = SOUP_SESSION (source);
	GBytes *body;
	GError *error = NULL;
	const guint8 *data;
	gsize len;

	body = soup_session_send_and_read_finish (session, result, &error);
	g_object_unref (session);

	if (error)
	{
		icon_fetch_complete (ctx, NULL, 0, error->message);
		g_error_free (error);
		if (body)
			g_bytes_unref (body);
		return;
	}

	data = g_bytes_get_data (body, &len);
	icon_fetch_complete (ctx, data, len, NULL);
	g_bytes_unref (body);
}

static void
icon_fetch_start (icon_fetch_ctx *ctx)
{
	SoupSession *session;
	SoupMessage *msg;

	session = soup_session_new ();
	msg = soup_message_new ("GET", ctx->resolved_url);
	if (!msg)
	{
		icon_fetch_complete (ctx, NULL, 0, "Invalid icon URL");
		g_object_unref (session);
		return;
	}

	soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT, NULL,
	                                   icon_soup_callback, ctx);
	g_object_unref (msg);
}

#elif defined(USE_LIBCURL)

/* ---- libcurl backend ---- */

typedef struct {
	char *data;
	size_t size;
} curl_buf;

static size_t
icon_curl_write_cb (void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	curl_buf *buf = (curl_buf *)userp;
	char *ptr;

	/* Enforce size limit during download */
	if (buf->size + realsize > NETWORK_ICON_MAX_SIZE)
		return 0;

	ptr = g_realloc (buf->data, buf->size + realsize + 1);
	if (!ptr)
		return 0;

	buf->data = ptr;
	memcpy (&(buf->data[buf->size]), contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = '\0';
	return realsize;
}

typedef struct {
	icon_fetch_ctx *ctx;
	curl_buf response;
	CURLcode result;
	char *error_msg;
} curl_icon_ctx;

/* Called on main thread after the worker thread completes */
static gboolean
icon_curl_complete_idle (gpointer user_data)
{
	curl_icon_ctx *cctx = (curl_icon_ctx *)user_data;

	if (cctx->result != CURLE_OK)
	{
		icon_fetch_complete (cctx->ctx, NULL, 0, cctx->error_msg);
		g_free (cctx->error_msg);
		g_free (cctx->response.data);
		g_free (cctx);
		return G_SOURCE_REMOVE;
	}

	icon_fetch_complete (cctx->ctx,
	                     (const guint8 *)cctx->response.data,
	                     cctx->response.size, NULL);
	g_free (cctx->response.data);
	g_free (cctx);
	return G_SOURCE_REMOVE;
}

/* Worker thread: performs the blocking curl download */
static gpointer
icon_curl_thread (gpointer user_data)
{
	curl_icon_ctx *cctx = (curl_icon_ctx *)user_data;
	CURL *curl;

	if (cctx->ctx->cancelled)
	{
		icon_fetch_ctx_free (cctx->ctx);
		g_free (cctx);
		return NULL;
	}

	curl = curl_easy_init ();
	if (!curl)
	{
		cctx->result = CURLE_FAILED_INIT;
		cctx->error_msg = g_strdup ("Failed to init libcurl");
		g_idle_add (icon_curl_complete_idle, cctx);
		return NULL;
	}

	curl_easy_setopt (curl, CURLOPT_URL, cctx->ctx->resolved_url);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, icon_curl_write_cb);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, &cctx->response);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt (curl, CURLOPT_MAXFILESIZE, (long)NETWORK_ICON_MAX_SIZE);
#ifdef WIN32
	/* Use Windows native CA store for SSL verification */
	curl_easy_setopt (curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

	cctx->result = curl_easy_perform (curl);
	curl_easy_cleanup (curl);

	if (cctx->result != CURLE_OK)
		cctx->error_msg = g_strdup_printf ("curl error: %s",
		                                    curl_easy_strerror (cctx->result));

	/* Dispatch completion back to the main thread */
	g_idle_add (icon_curl_complete_idle, cctx);
	return NULL;
}

static void
icon_fetch_start (icon_fetch_ctx *ctx)
{
	curl_icon_ctx *cctx = g_new0 (curl_icon_ctx, 1);
	cctx->ctx = ctx;
	g_thread_unref (g_thread_new ("network-icon", icon_curl_thread, cctx));
}

#else /* no HTTP library */

static void
icon_fetch_start (icon_fetch_ctx *ctx)
{
	icon_fetch_complete (ctx, NULL, 0,
	                     "No HTTP library available (need libsoup3 or libcurl)");
}

#endif /* USE_LIBSOUP / USE_LIBCURL */

/* ---- Public API ---- */

/* Get the network name for cache purposes. Returns NULL if unavailable. */
static const char *
get_network_name (struct server *serv)
{
	ircnet *net = (ircnet *)serv->network;
	if (net && net->name)
		return net->name;
	return NULL;
}

void
network_icon_load_cached (struct server *serv)
{
	const char *net_name;
	char *path;
	guint8 *data;
	gsize len;

	if (!prefs.hex_gui_network_icons)
		return;

	net_name = get_network_name (serv);
	if (!net_name)
		return;

	path = cache_find (net_name);
	if (!path)
		return;

	if (g_file_get_contents (path, (char **)&data, &len, NULL))
	{
		fe_network_icon_ready (serv, data, len);
		g_free (data);
	}
	g_free (path);
}

void
network_icon_fetch (struct server *serv)
{
	icon_fetch_ctx *ctx;
	const char *net_name;
	char *resolved_url, *cached_path;

	if (!prefs.hex_gui_network_icons)
		return;

	if (!serv->network_icon_url || serv->network_icon_url[0] == '\0')
		return;

	net_name = get_network_name (serv);
	if (!net_name)
		return;

	/* Cancel any existing fetch */
	network_icon_cancel (serv);

	resolved_url = network_icon_resolve_url (serv->network_icon_url,
	                                          NETWORK_ICON_SIZE);
	if (!resolved_url)
		return;

	/* Check if cached file already matches this URL hash */
	cached_path = cache_path (net_name, resolved_url);
	if (g_file_test (cached_path, G_FILE_TEST_EXISTS))
	{
		/* Hash matches — icon already loaded from cache or is current */
		g_free (cached_path);
		g_free (resolved_url);
		return;
	}
	g_free (cached_path);

	/* URL changed — need to fetch the new icon */
	ctx = g_new0 (icon_fetch_ctx, 1);
	ctx->serv = serv;
	ctx->resolved_url = resolved_url;
	ctx->network_name = g_strdup (net_name);
	ctx->cancelled = FALSE;
	serv->network_icon_cancel = ctx;

	icon_fetch_start (ctx);
}

void
network_icon_clear_cache (struct server *serv)
{
	const char *net_name = get_network_name (serv);
	if (net_name)
		cache_clear (net_name);
}

void
network_icon_cancel (struct server *serv)
{
	icon_fetch_ctx *ctx;

	if (!serv->network_icon_cancel)
		return;

	ctx = (icon_fetch_ctx *)serv->network_icon_cancel;
	ctx->cancelled = TRUE;
	serv->network_icon_cancel = NULL;
}
