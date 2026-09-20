/*
 * ChatGPT Plugin for libpurple/Pidgin
 * Copyright (c) 2024-2026 Eion Robb
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBCHATGPT_H
#define LIBCHATGPT_H

#ifndef PURPLE_PLUGINS
#	define PURPLE_PLUGINS
#endif

#include <glib.h>

#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <glib/gi18n.h>
#include <sys/types.h>
#ifdef __GNUC__
#	include <sys/time.h>
#	include <unistd.h>
#endif

#ifndef G_GNUC_NULL_TERMINATED
#	if __GNUC__ >= 4
#		define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#	else
#		define G_GNUC_NULL_TERMINATED
#	endif /* __GNUC__ >= 4 */
#endif /* G_GNUC_NULL_TERMINATED */

// workaround for MinGW32 which doesn't support "%zu"; see also https://stackoverflow.com/a/44383330
#ifdef _WIN32
#  ifdef _WIN64
#    define PRI_SIZET PRIu64
#  else
#    define PRI_SIZET PRIu32
#  endif
#else
#  define PRI_SIZET "zu"
#endif

#include "purplecompat.h"
#include "glibcompat.h"

#ifdef _WIN32
#	include <windows.h>
#	define dlopen(a,b)  GetModuleHandleA(a)
#	define RTLD_LAZY    0x0001
#	define dlsym(a,b)   GetProcAddress(a,b)
#	define dlclose(a)   FreeLibrary(a)
#else
#	include <arpa/inet.h>
#	include <dlfcn.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#endif

#include <json-glib/json-glib.h>

#define json_object_get_int_member(JSON_OBJECT, MEMBER) \
	((JSON_OBJECT) && json_object_has_member((JSON_OBJECT), (MEMBER)) ? json_object_get_int_member((JSON_OBJECT), (MEMBER)) : 0)
#define json_object_get_string_member(JSON_OBJECT, MEMBER) \
	((JSON_OBJECT) && json_object_has_member((JSON_OBJECT), (MEMBER)) ? json_object_get_string_member((JSON_OBJECT), (MEMBER)) : NULL)
#define json_object_get_array_member(JSON_OBJECT, MEMBER) \
	((JSON_OBJECT) && json_object_has_member((JSON_OBJECT), (MEMBER)) ? json_object_get_array_member((JSON_OBJECT), (MEMBER)) : NULL)
#define json_object_get_object_member(JSON_OBJECT, MEMBER) \
	((JSON_OBJECT) && json_object_has_member((JSON_OBJECT), (MEMBER)) ? json_object_get_object_member((JSON_OBJECT), (MEMBER)) : NULL)
#define json_object_get_boolean_member(JSON_OBJECT, MEMBER) \
	((JSON_OBJECT) && json_object_has_member((JSON_OBJECT), (MEMBER)) ? json_object_get_boolean_member((JSON_OBJECT), (MEMBER)) : FALSE)
#define json_array_get_length(JSON_ARRAY) \
	((JSON_ARRAY) ? json_array_get_length(JSON_ARRAY) : 0)
#define json_node_get_array(JSON_NODE) \
	((JSON_NODE) && JSON_NODE_TYPE(JSON_NODE) == (JSON_NODE_ARRAY) ? json_node_get_array(JSON_NODE) : NULL)

#include "accountopt.h"
#include "core.h"
#include "cmds.h"
#include "connection.h"
#include "debug.h"
#include "http.h"
#include "image.h"
#include "image-store.h"
#include "plugins.h"
#include "proxy.h"
#include "request.h"
#include "roomlist.h"
#include "savedstatuses.h"
#include "sslconn.h"
#include "util.h"
#include "version.h"

#if GLIB_MAJOR_VERSION >= 2 && GLIB_MINOR_VERSION >= 12
#	define atoll(a) g_ascii_strtoll(a, NULL, 0)
#endif

#define CHATGPT_PLUGIN_ID "prpl-eionrobb-chatgpt"
#define CHATGPT_PLUGIN_VERSION "2.0"

typedef enum {
	CHATGPT_PROVIDER_OPENAI = 0,
	CHATGPT_PROVIDER_GEMINI,
	CHATGPT_PROVIDER_CLAUDE,
	CHATGPT_PROVIDER_CUSTOM
} ChatGptProvider;

typedef struct _ChatMessage {
	gchar *role;    /* "user" or "assistant" */
	gchar *content; /* message text */
} ChatMessage;

typedef struct _ChatConversation {
	gchar *who;           /* buddy name */
	GList *messages;      /* GList of ChatMessage* */
	gchar *model;         /* conversation-specific model, or NULL */
	gchar *system_prompt; /* conversation-specific system prompt, or NULL */
} ChatConversation;

typedef struct _ChatGptAccount ChatGptAccount;
struct _ChatGptAccount {
	PurpleAccount *account;
	PurpleConnection *pc;
	PurpleHttpKeepalivePool *keepalive_pool;
	PurpleHttpConnectionSet *conns;

	ChatGptProvider provider;
	gchar *api_key;
	gchar *api_url;
	gchar *default_model;
	gchar *system_prompt;

	GList *available_models;   /* GList of gchar* */
	GHashTable *conversations; /* who (gchar*) -> ChatConversation* */
};

typedef struct _ChatGptBuddy ChatGptBuddy;
struct _ChatGptBuddy {
	PurpleBuddy *buddy;
	gchar *model;
	gchar *system_prompt;
	gchar *name;
	gchar *description;
};

typedef void (*ChatGptCallbackFunc)(ChatGptAccount *cga, JsonObject *obj, gpointer user_data);
typedef void (*ChatGptCallbackErrorFunc)(ChatGptAccount *cga, const gchar *data, gssize data_len, gpointer user_data);

typedef struct _ChatGptApiConnection ChatGptApiConnection;
struct _ChatGptApiConnection {
	ChatGptAccount *cga;
	gchar *url;
	ChatGptCallbackFunc callback;
	gpointer user_data;
	PurpleHttpConnection *http_conn;
	ChatGptCallbackErrorFunc error_callback;
};

#endif /* LIBCHATGPT_H */
