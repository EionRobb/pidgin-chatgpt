/*
 * ChatGPT / Multi-Provider Plugin for libpurple/Pidgin
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

#include "libchatgpt.h"
#include "cmds.h"
#include "glib-object.h"
#include "glib.h"
#include "purplecompat.h"
#include <http.h>
#include "markdown.h"

/******************************************************************************/
/* Defaults by Provider                                                       */
/******************************************************************************/

static const gchar *
chatgpt_provider_default_url(ChatGptProvider provider)
{
	switch (provider) {
	case CHATGPT_PROVIDER_OPENAI:
		return "https://api.openai.com";
	case CHATGPT_PROVIDER_GEMINI:
		return "https://generativelanguage.googleapis.com";
	case CHATGPT_PROVIDER_CLAUDE:
		return "https://api.anthropic.com";
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		return "http://localhost:11434/v1";
	}
}

static const gchar *
chatgpt_provider_default_model(ChatGptProvider provider)
{
	switch (provider) {
	case CHATGPT_PROVIDER_OPENAI:
		return "gpt-4o-mini";
	case CHATGPT_PROVIDER_GEMINI:
		return "gemini-2.5-flash";
	case CHATGPT_PROVIDER_CLAUDE:
		return "claude-3-5-sonnet-20241022";
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		return "llama3";
	}
}

static const gchar *
chatgpt_provider_default_buddy_name(ChatGptProvider provider)
{
	switch (provider) {
	case CHATGPT_PROVIDER_OPENAI:
		return "ChatGPT";
	case CHATGPT_PROVIDER_GEMINI:
		return "Gemini";
	case CHATGPT_PROVIDER_CLAUDE:
		return "Claude";
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		return "AI Assistant";
	}
}

static const gchar *
chatgpt_provider_name(ChatGptProvider provider)
{
	switch (provider) {
	case CHATGPT_PROVIDER_OPENAI:
		return "OpenAI";
	case CHATGPT_PROVIDER_GEMINI:
		return "Google Gemini";
	case CHATGPT_PROVIDER_CLAUDE:
		return "Anthropic Claude";
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		return "Custom / OpenAI-Compatible";
	}
}

/******************************************************************************/
/* JSON functions                                                             */
/******************************************************************************/

static gchar *
json_object_to_string(const JsonObject *jsonobj, gsize *length)
{
	JsonGenerator *generator;
	JsonNode *root;
	gchar *string;

	root = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(root, (JsonObject *)jsonobj);

	generator = json_generator_new();
	json_generator_set_root(generator, root);

	string = json_generator_to_data(generator, length);

	g_object_unref(generator);
	json_node_free(root);

	return string;
}

static JsonNode *
json_decode(const gchar *data, gssize len)
{
	JsonParser *parser = json_parser_new();
	JsonNode *root = NULL;

	if (!data || !json_parser_load_from_data(parser, data, len, NULL)) {
		purple_debug_error("chatgpt", "Error parsing JSON: %s\n", data ? data : "(null)");
	} else {
		root = json_parser_get_root(parser);
		if (root != NULL) {
			root = json_node_copy(root);
		}
	}
	g_object_unref(parser);

	return root;
}

static JsonObject *
json_string_to_object(const gchar *data, gssize len)
{
	JsonNode *root = json_decode(data, len);
	JsonObject *ret;

	g_return_val_if_fail(root, NULL);

	if (!JSON_NODE_HOLDS_OBJECT(root)) {
		json_node_free(root);
		return NULL;
	}

	ret = json_node_dup_object(root);
	json_node_free(root);

	return ret;
}

static gchar *
chatgpt_extract_json_error(JsonObject *obj, const gchar *fallback)
{
	if (obj != NULL) {
		JsonObject *err_obj = json_object_get_object_member(obj, "error");
		if (err_obj != NULL) {
			const gchar *msg = json_object_get_string_member(err_obj, "message");
			if (msg != NULL && *msg) {
				return g_strdup(msg);
			}
		}
		const gchar *top_msg = json_object_get_string_member(obj, "message");
		if (top_msg != NULL && *top_msg) {
			return g_strdup(top_msg);
		}
	}
	return fallback ? g_strdup(fallback) : g_strdup(_("Unknown error"));
}

/******************************************************************************/
/* Conversation History Helpers                                               */
/******************************************************************************/

static void
chat_message_free(ChatMessage *msg)
{
	if (msg) {
		g_free(msg->role);
		g_free(msg->content);
		g_free(msg);
	}
}

static void
chat_conversation_free(ChatConversation *conv)
{
	if (conv) {
		g_free(conv->who);
		g_list_free_full(conv->messages, (GDestroyNotify)chat_message_free);
		g_free(conv->model);
		g_free(conv->system_prompt);
		g_free(conv);
	}
}

static ChatConversation *
chat_conversation_get_or_create(ChatGptAccount *cga, const gchar *who)
{
	ChatConversation *conv = g_hash_table_lookup(cga->conversations, who);
	if (conv == NULL) {
		conv = g_new0(ChatConversation, 1);
		conv->who = g_strdup(who);
		g_hash_table_insert(cga->conversations, conv->who, conv);
	}
	return conv;
}

static void
chat_conversation_add_message(ChatConversation *conv, const gchar *role, const gchar *content)
{
	ChatMessage *msg = g_new0(ChatMessage, 1);
	msg->role = g_strdup(role);
	msg->content = g_strdup(content);
	conv->messages = g_list_append(conv->messages, msg);

	/* Cap history at 40 messages to avoid token limit overflow */
	while (g_list_length(conv->messages) > 40) {
		ChatMessage *old = conv->messages->data;
		conv->messages = g_list_delete_link(conv->messages, conv->messages);
		chat_message_free(old);
	}
}

static void
chat_conversation_remove_last_user_message(ChatConversation *conv)
{
	GList *last = g_list_last(conv->messages);
	if (last != NULL) {
		ChatMessage *msg = last->data;
		if (purple_strequal(msg->role, "user")) {
			conv->messages = g_list_delete_link(conv->messages, last);
			chat_message_free(msg);
		}
	}
}

/******************************************************************************/
/* HTTP Request Functions                                                     */
/******************************************************************************/

static void
chatgpt_http_request_cb(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	ChatGptApiConnection *conn = user_data;
	const gchar *data;
	gsize len = 0;
	JsonObject *obj = NULL;
	gboolean success = purple_http_response_is_successful(response);

	data = purple_http_response_get_data(response, &len);
	if (data != NULL && len > 0) {
		obj = json_string_to_object(data, len);
	}

	if (!success || obj == NULL) {
		if (conn->error_callback != NULL) {
			conn->error_callback(conn->cga, data, len, conn->user_data);
		} else {
			gchar *err = chatgpt_extract_json_error(obj, purple_http_response_get_error(response));
			purple_debug_error("chatgpt", "HTTP error (%d): %s\n", purple_http_response_get_code(response), err);
			g_free(err);
		}
	} else {
		if (conn->callback != NULL) {
			conn->callback(conn->cga, obj, conn->user_data);
		}
	}

	if (obj != NULL) {
		json_object_unref(obj);
	}
	g_free(conn->url);
	g_free(conn);
}

static ChatGptApiConnection *
chatgpt_http_request_full(ChatGptAccount *cga, const gchar *method, const gchar *url,
                          const JsonObject *obj, ChatGptCallbackFunc callback,
                          gpointer user_data, ChatGptCallbackErrorFunc error_callback)
{
	PurpleConnection *pc = cga->pc;
	ChatGptApiConnection *conn;
	PurpleHttpRequest *request;

	request = purple_http_request_new(url);
	purple_http_request_set_keepalive_pool(request, cga->keepalive_pool);
	purple_http_request_set_max_redirects(request, 3);
	purple_http_request_set_timeout(request, 120);

	if (method != NULL) {
		purple_http_request_set_method(request, method);
	}

	/* Set authentication and custom headers based on provider */
	switch (cga->provider) {
	case CHATGPT_PROVIDER_OPENAI:
		if (cga->api_key && *cga->api_key) {
			purple_http_request_header_set_printf(request, "Authorization", "Bearer %s", cga->api_key);
		}
		break;
	case CHATGPT_PROVIDER_GEMINI:
		if (cga->api_key && *cga->api_key) {
			purple_http_request_header_set(request, "x-goog-api-key", cga->api_key);
		}
		break;
	case CHATGPT_PROVIDER_CLAUDE:
		if (cga->api_key && *cga->api_key) {
			purple_http_request_header_set(request, "x-api-key", cga->api_key);
		}
		purple_http_request_header_set(request, "anthropic-version", "2023-06-01");
		break;
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		if (cga->api_key && *cga->api_key) {
			purple_http_request_header_set_printf(request, "Authorization", "Bearer %s", cga->api_key);
		}
		break;
	}

	if (obj != NULL) {
		gsize len;
		gchar *body = json_object_to_string(obj, &len);

		if (method == NULL) {
			purple_http_request_set_method(request, "POST");
		}
		purple_http_request_set_contents(request, body, len);
		purple_http_request_header_set(request, "Content-Type", "application/json");
		g_free(body);
	}

	conn = g_new0(ChatGptApiConnection, 1);
	conn->cga = cga;
	conn->url = g_strdup(url);
	conn->user_data = user_data;
	conn->callback = callback;
	conn->error_callback = error_callback;

	conn->http_conn = purple_http_request(pc, request, chatgpt_http_request_cb, conn);
	if (conn->http_conn != NULL) {
		purple_http_connection_set_add(cga->conns, conn->http_conn);
	}
	purple_http_request_unref(request);

	return conn;
}

/******************************************************************************/
/* Models Fetching and Handling                                               */
/******************************************************************************/

static gint
chatgpt_model_sort_cb(gconstpointer a, gconstpointer b)
{
	return g_ascii_strcasecmp((const gchar *)a, (const gchar *)b);
}

static void
chatgpt_save_cached_models(ChatGptAccount *cga)
{
	GString *joined = g_string_new("");
	GList *l;

	for (l = cga->available_models; l != NULL; l = l->next) {
		if (joined->len > 0) {
			g_string_append_c(joined, ',');
		}
		g_string_append(joined, (const gchar *)l->data);
	}

	purple_account_set_string(cga->account, "cached_models", joined->str);
	g_string_free(joined, TRUE);
}

static void
chatgpt_load_cached_models(ChatGptAccount *cga)
{
	const gchar *cached = purple_account_get_string(cga->account, "cached_models", NULL);
	gchar **split;
	int i;

	if (cached == NULL || *cached == '\0') {
		return;
	}

	g_list_free_full(cga->available_models, g_free);
	cga->available_models = NULL;

	split = g_strsplit(cached, ",", -1);
	for (i = 0; split[i] != NULL; i++) {
		if (*split[i] != '\0') {
			cga->available_models = g_list_append(cga->available_models, g_strdup(split[i]));
		}
	}
	g_strfreev(split);
}

static void
chatgpt_fetch_models_cb(ChatGptAccount *cga, JsonObject *obj, gpointer user_data)
{
	gboolean notify_user = GPOINTER_TO_INT(user_data);
	GList *models = NULL;

	if (cga->provider == CHATGPT_PROVIDER_GEMINI) {
		JsonArray *models_arr = json_object_get_array_member(obj, "models");
		guint i, len = json_array_get_length(models_arr);

		for (i = 0; i < len; i++) {
			JsonObject *m_obj = json_array_get_object_element(models_arr, i);
			const gchar *name = json_object_get_string_member(m_obj, "name");
			JsonArray *methods = json_object_get_array_member(m_obj, "supportedGenerationMethods");
			gboolean can_generate = FALSE;

			if (methods != NULL) {
				guint j, m_len = json_array_get_length(methods);
				for (j = 0; j < m_len; j++) {
					const gchar *method = json_array_get_string_element(methods, j);
					if (purple_strequal(method, "generateContent")) {
						can_generate = TRUE;
						break;
					}
				}
			}

			if (can_generate && name != NULL) {
				const gchar *clean_name = g_str_has_prefix(name, "models/") ? name + 7 : name;
				models = g_list_append(models, g_strdup(clean_name));
			}
		}
	} else {
		/* OpenAI, Claude, and Custom / OpenAI-compatible */
		JsonArray *data_arr = json_object_get_array_member(obj, "data");
		guint i, len = json_array_get_length(data_arr);

		for (i = 0; i < len; i++) {
			JsonObject *m_obj = json_array_get_object_element(data_arr, i);
			const gchar *id = json_object_get_string_member(m_obj, "id");

			if (id == NULL || *id == '\0') {
				continue;
			}

			/* For OpenAI, filter out obvious non-chat models */
			if (cga->provider == CHATGPT_PROVIDER_OPENAI) {
				if (g_str_has_prefix(id, "text-embedding") ||
				    g_str_has_prefix(id, "whisper") ||
				    g_str_has_prefix(id, "tts") ||
				    g_str_has_prefix(id, "dall-e") ||
				    g_str_has_prefix(id, "babbage") ||
				    g_str_has_prefix(id, "davinci") ||
				    g_str_has_prefix(id, "canary")) {
					continue;
				}
			}

			models = g_list_append(models, g_strdup(id));
		}
	}

	if (models != NULL) {
		models = g_list_sort(models, chatgpt_model_sort_cb);
		g_list_free_full(cga->available_models, g_free);
		cga->available_models = models;
		chatgpt_save_cached_models(cga);

		purple_debug_info("chatgpt", "Successfully fetched %u models\n", g_list_length(models));

		if (notify_user) {
			gchar *msg = g_strdup_printf(_("Successfully downloaded %u available models for %s."),
			                            g_list_length(models), chatgpt_provider_name(cga->provider));
			purple_notify_info(cga->pc, _("Models Updated"), _("Available Models Refreshed"), msg);
			g_free(msg);
		}
	}
}

static void
chatgpt_fetch_models_error_cb(ChatGptAccount *cga, const gchar *data, gssize data_len, gpointer user_data)
{
	gboolean notify_user = GPOINTER_TO_INT(user_data);
	JsonObject *obj = json_string_to_object(data, data_len);
	gchar *err = chatgpt_extract_json_error(obj, _("Failed to download models from API endpoint"));

	purple_debug_error("chatgpt", "Error downloading models: %s\n", err);
	if (notify_user) {
		purple_notify_error(cga->pc, _("Download Failed"), _("Could not fetch models list"), err, NULL);
	}

	g_free(err);
	if (obj) {
		json_object_unref(obj);
	}
}

static void
chatgpt_fetch_models(ChatGptAccount *cga, gboolean notify_user)
{
	gchar *url;
	const gchar *base = cga->api_url && *cga->api_url ? cga->api_url : chatgpt_provider_default_url(cga->provider);

	switch (cga->provider) {
	case CHATGPT_PROVIDER_GEMINI:
		url = g_strdup_printf("%s/v1beta/models", base);
		break;
	case CHATGPT_PROVIDER_CLAUDE:
		url = g_strdup_printf("%s/v1/models", base);
		break;
	case CHATGPT_PROVIDER_OPENAI:
	case CHATGPT_PROVIDER_CUSTOM:
	default:
		if (g_str_has_suffix(base, "/v1") || g_str_has_suffix(base, "/v1/")) {
			gchar *trimmed = g_str_has_suffix(base, "/") ? g_strndup(base, strlen(base) - 1) : g_strdup(base);
			url = g_strdup_printf("%s/models", trimmed);
			g_free(trimmed);
		} else {
			url = g_strdup_printf("%s/v1/models", base);
		}
		break;
	}

	purple_debug_info("chatgpt", "Fetching models from: %s\n", url);
	chatgpt_http_request_full(cga, "GET", url, NULL, chatgpt_fetch_models_cb,
	                          GINT_TO_POINTER(notify_user), chatgpt_fetch_models_error_cb);
	g_free(url);
}

/******************************************************************************/
/* Model Selection Dialog                                                     */
/******************************************************************************/

static void
chatgpt_action_select_model_cb(gpointer user_data, PurpleRequestFields *fields)
{
	ChatGptAccount *cga = user_data;
	PurpleRequestField *list_field;
	PurpleRequestField *custom_field;
	const gchar *custom_val = NULL;
	const gchar *chosen = NULL;
	GList *selected = NULL;

	list_field = purple_request_fields_get_field(fields, "model_list");
	custom_field = purple_request_fields_get_field(fields, "model_custom");

	if (custom_field != NULL) {
		custom_val = purple_request_field_string_get_value(custom_field);
		if (custom_val != NULL) {
			custom_val = g_strstrip((gchar *)custom_val);
		}
	}

	if (list_field != NULL) {
		selected = purple_request_field_list_get_selected(list_field);
	}

	if (selected != NULL) {
		chosen = (const gchar *)selected->data;
	}

	/* If custom input differs from previous default and is non-empty, use custom */
	if (custom_val != NULL && *custom_val != '\0') {
		if (cga->default_model == NULL || !purple_strequal(custom_val, cga->default_model)) {
			chosen = custom_val;
		}
	}

	if (chosen != NULL && *chosen != '\0') {
		g_free(cga->default_model);
		cga->default_model = g_strdup(chosen);
		purple_account_set_string(cga->account, "model", chosen);

		gchar *msg = g_strdup_printf(_("Active model for %s has been updated to: %s"),
		                            chatgpt_provider_name(cga->provider), chosen);
		purple_notify_info(cga->pc, _("Model Updated"), _("Model selection saved"), msg);
		g_free(msg);
	}
}

static void
chatgpt_action_select_model(PurplePluginAction *action)
{
	PurpleConnection *pc = purple_protocol_action_get_connection(action);
	ChatGptAccount *cga = purple_connection_get_protocol_data(pc);
	PurpleRequestFields *fields;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *list_field;
	PurpleRequestField *custom_field;
	GList *l;
	const gchar *current_model;

	if (cga == NULL) {
		return;
	}

	if (cga->available_models == NULL) {
		chatgpt_load_cached_models(cga);
	}

	if (cga->available_models == NULL) {
		purple_notify_info(pc, _("Fetching Models"), _("Downloading models..."),
		                   _("Downloading models from the provider. Please try again in a few moments."));
		chatgpt_fetch_models(cga, TRUE);
		return;
	}

	current_model = cga->default_model ? cga->default_model : chatgpt_provider_default_model(cga->provider);

	fields = purple_request_fields_new();
	group = purple_request_field_group_new(NULL);
	purple_request_fields_add_group(fields, group);

	list_field = purple_request_field_list_new("model_list", _("Available Models"));
	purple_request_field_list_set_multi_select(list_field, FALSE);

	for (l = cga->available_models; l != NULL; l = l->next) {
		const gchar *model_name = l->data;
		purple_request_field_list_add_icon(list_field, model_name, NULL, (gpointer)model_name);
		if (purple_strequal(model_name, current_model)) {
			purple_request_field_list_add_selected(list_field, model_name);
		}
	}
	purple_request_field_group_add_field(group, list_field);

	custom_field = purple_request_field_string_new("model_custom", _("Custom Model ID"), current_model, FALSE);
	purple_request_field_group_add_field(group, custom_field);

	purple_request_fields(pc, _("Select Model"), _("Choose an AI Model"),
	                      _("Pick an available model or enter a custom model identifier:"),
	                      fields,
	                      _("OK"), G_CALLBACK(chatgpt_action_select_model_cb),
	                      _("Cancel"), NULL,
	                      cga->account, NULL, NULL,
	                      cga);
}

static void
chatgpt_action_refresh_models(PurplePluginAction *action)
{
	PurpleConnection *pc = purple_protocol_action_get_connection(action);
	ChatGptAccount *cga = purple_connection_get_protocol_data(pc);

	if (cga != NULL) {
		chatgpt_fetch_models(cga, TRUE);
	}
}

static void
chatgpt_action_clear_history(PurplePluginAction *action)
{
	PurpleConnection *pc = purple_protocol_action_get_connection(action);
	ChatGptAccount *cga = purple_connection_get_protocol_data(pc);

	if (cga != NULL && cga->conversations != NULL) {
		g_hash_table_remove_all(cga->conversations);
		purple_notify_info(pc, _("History Cleared"), _("Conversation history cleared"),
		                   _("All conversation contexts have been cleared."));
	}
}

/******************************************************************************/
/* Chat Generation and Messaging                                              */
/******************************************************************************/

typedef struct {
	ChatGptAccount *cga;
	gchar *who;
} ChatGptSendMsgData;

static void
chatgpt_send_message_error_cb(ChatGptAccount *cga, const gchar *data, gssize data_len, gpointer user_data)
{
	ChatGptSendMsgData *smd = user_data;
	JsonObject *obj = json_string_to_object(data, data_len);
	gchar *err = chatgpt_extract_json_error(obj, _("Failed to communicate with AI provider"));
	ChatConversation *conv;

	serv_got_typing_stopped(cga->pc, smd->who);

	/* Remove failed prompt so context isn't corrupted */
	conv = g_hash_table_lookup(cga->conversations, smd->who);
	if (conv != NULL) {
		chat_conversation_remove_last_user_message(conv);
	}

	purple_serv_got_im(cga->pc, smd->who, err, PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_SYSTEM, time(NULL));

	g_free(err);
	if (obj) {
		json_object_unref(obj);
	}
	g_free(smd->who);
	g_free(smd);
}

static void
chatgpt_send_message_cb(ChatGptAccount *cga, JsonObject *obj, gpointer user_data)
{
	ChatGptSendMsgData *smd = user_data;
	ChatConversation *conv;
	gchar *reply_text = NULL;

	serv_got_typing_stopped(cga->pc, smd->who);

	conv = chat_conversation_get_or_create(cga, smd->who);

	switch (cga->provider) {
	case CHATGPT_PROVIDER_GEMINI: {
		JsonArray *candidates = json_object_get_array_member(obj, "candidates");
		if (candidates != NULL && json_array_get_length(candidates) > 0) {
			JsonObject *cand = json_array_get_object_element(candidates, 0);
			JsonObject *content = json_object_get_object_member(cand, "content");
			JsonArray *parts = json_object_get_array_member(content, "parts");
			if (parts != NULL && json_array_get_length(parts) > 0) {
				JsonObject *part = json_array_get_object_element(parts, 0);
				const gchar *t = json_object_get_string_member(part, "text");
				if (t != NULL) {
					reply_text = g_strdup(t);
				}
			}
		}
		break;
	}
	case CHATGPT_PROVIDER_CLAUDE: {
		JsonArray *content = json_object_get_array_member(obj, "content");
		if (content != NULL) {
			guint i, len = json_array_get_length(content);
			GString *combined = g_string_new("");
			for (i = 0; i < len; i++) {
				JsonObject *block = json_array_get_object_element(content, i);
				const gchar *type = json_object_get_string_member(block, "type");
				if (purple_strequal(type, "text")) {
					const gchar *t = json_object_get_string_member(block, "text");
					if (t != NULL) {
						g_string_append(combined, t);
					}
				}
			}
			if (combined->len > 0) {
				reply_text = g_string_free(combined, FALSE);
			} else {
				g_string_free(combined, TRUE);
			}
		}
		break;
	}
	case CHATGPT_PROVIDER_OPENAI:
	case CHATGPT_PROVIDER_CUSTOM:
	default: {
		JsonArray *choices = json_object_get_array_member(obj, "choices");
		if (choices != NULL && json_array_get_length(choices) > 0) {
			JsonObject *choice = json_array_get_object_element(choices, 0);
			JsonObject *msg = json_object_get_object_member(choice, "message");
			const gchar *t = json_object_get_string_member(msg, "content");
			if (t != NULL) {
				reply_text = g_strdup(t);
			}
		}
		break;
	}
	}

	if (reply_text != NULL) {
		gchar *html;

		chat_conversation_add_message(conv, "assistant", reply_text);
		html = markdown_convert_markdown(reply_text, TRUE, FALSE);
		purple_serv_got_im(cga->pc, smd->who, html, PURPLE_MESSAGE_RECV, time(NULL));

		g_free(html);
		g_free(reply_text);
	} else {
		gchar *err = chatgpt_extract_json_error(obj, _("No content received in response"));
		chat_conversation_remove_last_user_message(conv);
		purple_serv_got_im(cga->pc, smd->who, err, PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(err);
	}

	g_free(smd->who);
	g_free(smd);
}

static void
chatgpt_send_chat_request(ChatGptAccount *cga, const gchar *who)
{
	ChatConversation *conv = chat_conversation_get_or_create(cga, who);
	const gchar *model = conv->model ? conv->model :
	                     (cga->default_model ? cga->default_model : chatgpt_provider_default_model(cga->provider));
	const gchar *sys = conv->system_prompt ? conv->system_prompt :
	                   (cga->system_prompt ? cga->system_prompt : "You are a helpful AI assistant.");
	const gchar *base = cga->api_url && *cga->api_url ? cga->api_url : chatgpt_provider_default_url(cga->provider);
	JsonObject *req_obj = json_object_new();
	gchar *url;
	GList *l;
	ChatGptSendMsgData *smd;

	serv_got_typing(cga->pc, who, 0, PURPLE_TYPING);

	switch (cga->provider) {
	case CHATGPT_PROVIDER_GEMINI: {
		JsonArray *contents;

		url = g_strdup_printf("%s/v1beta/models/%s:generateContent", base, model);

		if (sys && *sys) {
			JsonObject *sys_obj = json_object_new();
			JsonArray *parts = json_array_new();
			JsonObject *p = json_object_new();
			json_object_set_string_member(p, "text", sys);
			json_array_add_object_element(parts, p);
			json_object_set_array_member(sys_obj, "parts", parts);
			json_object_set_object_member(req_obj, "systemInstruction", sys_obj);
		}

		contents = json_array_new();
		for (l = conv->messages; l != NULL; l = l->next) {
			ChatMessage *cm = l->data;
			JsonObject *c = json_object_new();
			JsonArray *parts = json_array_new();
			JsonObject *p = json_object_new();

			json_object_set_string_member(c, "role", purple_strequal(cm->role, "assistant") ? "model" : "user");
			json_object_set_string_member(p, "text", cm->content);
			json_array_add_object_element(parts, p);
			json_object_set_array_member(c, "parts", parts);
			json_array_add_object_element(contents, c);
		}
		json_object_set_array_member(req_obj, "contents", contents);
		break;
	}
	case CHATGPT_PROVIDER_CLAUDE: {
		JsonArray *msgs;

		url = g_strdup_printf("%s/v1/messages", base);
		json_object_set_string_member(req_obj, "model", model);
		json_object_set_int_member(req_obj, "max_tokens", 4096);

		if (sys && *sys) {
			json_object_set_string_member(req_obj, "system", sys);
		}

		msgs = json_array_new();
		for (l = conv->messages; l != NULL; l = l->next) {
			ChatMessage *cm = l->data;
			JsonObject *m = json_object_new();

			json_object_set_string_member(m, "role", cm->role);
			json_object_set_string_member(m, "content", cm->content);
			json_array_add_object_element(msgs, m);
		}
		json_object_set_array_member(req_obj, "messages", msgs);
		break;
	}
	case CHATGPT_PROVIDER_OPENAI:
	case CHATGPT_PROVIDER_CUSTOM:
	default: {
		JsonArray *msgs;

		if (g_str_has_suffix(base, "/v1") || g_str_has_suffix(base, "/v1/")) {
			gchar *trimmed = g_str_has_suffix(base, "/") ? g_strndup(base, strlen(base) - 1) : g_strdup(base);
			url = g_strdup_printf("%s/chat/completions", trimmed);
			g_free(trimmed);
		} else {
			url = g_strdup_printf("%s/v1/chat/completions", base);
		}

		json_object_set_string_member(req_obj, "model", model);

		msgs = json_array_new();
		if (sys && *sys) {
			JsonObject *m = json_object_new();
			json_object_set_string_member(m, "role", "system");
			json_object_set_string_member(m, "content", sys);
			json_array_add_object_element(msgs, m);
		}

		for (l = conv->messages; l != NULL; l = l->next) {
			ChatMessage *cm = l->data;
			JsonObject *m = json_object_new();
			json_object_set_string_member(m, "role", cm->role);
			json_object_set_string_member(m, "content", cm->content);
			json_array_add_object_element(msgs, m);
		}
		json_object_set_array_member(req_obj, "messages", msgs);
		break;
	}
	}

	smd = g_new0(ChatGptSendMsgData, 1);
	smd->cga = cga;
	smd->who = g_strdup(who);

	chatgpt_http_request_full(cga, "POST", url, req_obj, chatgpt_send_message_cb, smd, chatgpt_send_message_error_cb);

	json_object_unref(req_obj);
	g_free(url);
}

/******************************************************************************/
/* PRPL functions                                                             */
/******************************************************************************/

static const char *
chatgpt_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
	return "chatgpt";
}

void
chatgpt_tooltip_text(PurpleBuddy *buddy, PurpleNotifyUserInfo *user_info, gboolean full)
{
	PurpleAccount *account = purple_buddy_get_account(buddy);
	PurpleConnection *pc = purple_account_get_connection(account);
	ChatGptAccount *cga;
	ChatConversation *conv = NULL;
	const gchar *who;
	const gchar *model;
	const gchar *sys;
	guint msg_count = 0;

	if (pc == NULL) {
		return;
	}
	cga = purple_connection_get_protocol_data(pc);
	if (cga == NULL) {
		return;
	}

	who = purple_buddy_get_name(buddy);
	conv = g_hash_table_lookup(cga->conversations, who);

	model = (conv && conv->model) ? conv->model :
	        (cga->default_model ? cga->default_model : chatgpt_provider_default_model(cga->provider));
	sys = (conv && conv->system_prompt) ? conv->system_prompt :
	      (cga->system_prompt ? cga->system_prompt : "");
	if (conv != NULL) {
		msg_count = g_list_length(conv->messages);
	}

	purple_notify_user_info_add_pair(user_info, _("Provider"), chatgpt_provider_name(cga->provider));
	purple_notify_user_info_add_pair(user_info, _("Model"), model);
	if (sys && *sys) {
		purple_notify_user_info_add_pair(user_info, _("Instructions"), sys);
	}
	if (msg_count > 0) {
		gchar *cnt_str = g_strdup_printf("%u messages", msg_count);
		purple_notify_user_info_add_pair(user_info, _("Context History"), cnt_str);
		g_free(cnt_str);
	}
}

GList *
chatgpt_status_types(PurpleAccount *account)
{
	GList *types = NULL;
	PurpleStatusType *status;

	status = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, NULL, NULL, TRUE, TRUE, FALSE);
	types = g_list_append(types, status);

	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "available", NULL, TRUE, TRUE, FALSE);
	types = g_list_append(types, status);

	return types;
}

gint
chatgpt_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags)
{
	ChatGptAccount *cga = purple_connection_get_protocol_data(gc);
	ChatConversation *conv;

	if (cga == NULL || message == NULL || *message == '\0') {
		return 1;
	}

	conv = chat_conversation_get_or_create(cga, who);

	/* Check for slash commands */
	if (purple_strequal(message, "/clear") || purple_strequal(message, "/reset") || purple_strequal(message, "/new")) {
		g_list_free_full(conv->messages, (GDestroyNotify)chat_message_free);
		conv->messages = NULL;
		purple_serv_got_im(gc, who, _("Conversation context cleared."), PURPLE_MESSAGE_SYSTEM, time(NULL));
		return 1;
	}

	if (purple_strequal(message, "/models")) {
		GString *str = g_string_new(_("Available models:\n"));
		GList *l;
		if (cga->available_models == NULL) {
			chatgpt_load_cached_models(cga);
		}
		if (cga->available_models == NULL) {
			g_string_append(str, _("(Models not downloaded yet. Run /models after login or use Account Actions.)"));
			chatgpt_fetch_models(cga, FALSE);
		} else {
			for (l = cga->available_models; l != NULL; l = l->next) {
				g_string_append_printf(str, "• %s\n", (const gchar *)l->data);
			}
		}
		purple_serv_got_im(gc, who, str->str, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_string_free(str, TRUE);
		return 1;
	}

	if (g_str_has_prefix(message, "/model")) {
		const gchar *arg = message + 6;
		while (*arg == ' ') {
			arg++;
		}
		if (*arg != '\0') {
			g_free(conv->model);
			conv->model = g_strdup(arg);
			gchar *msg = g_strdup_printf(_("Model set to '%s' for this chat."), arg);
			purple_serv_got_im(gc, who, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
			g_free(msg);
		} else {
			const gchar *curr = conv->model ? conv->model :
			                    (cga->default_model ? cga->default_model : chatgpt_provider_default_model(cga->provider));
			gchar *msg = g_strdup_printf(_("Current model: %s. Use '/model <name>' to switch, or '/models' to list."), curr);
			purple_serv_got_im(gc, who, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
			g_free(msg);
		}
		return 1;
	}

	if (g_str_has_prefix(message, "/system")) {
		const gchar *arg = message + 7;
		while (*arg == ' ') {
			arg++;
		}
		if (*arg != '\0') {
			g_free(conv->system_prompt);
			conv->system_prompt = g_strdup(arg);
			purple_serv_got_im(gc, who, _("System prompt updated for this chat."), PURPLE_MESSAGE_SYSTEM, time(NULL));
		} else {
			const gchar *curr = conv->system_prompt ? conv->system_prompt :
			                    (cga->system_prompt ? cga->system_prompt : "(none)");
			gchar *msg = g_strdup_printf(_("System prompt: %s"), curr);
			purple_serv_got_im(gc, who, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
			g_free(msg);
		}
		return 1;
	}

	if (purple_strequal(message, "/history")) {
		guint cnt = g_list_length(conv->messages);
		gchar *msg = g_strdup_printf(_("Current conversation history contains %u messages."), cnt);
		purple_serv_got_im(gc, who, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(msg);
		return 1;
	}

	if (purple_strequal(message, "/help")) {
		const gchar *help_text = _("Available commands:\n"
		                           "/models - List available models from provider\n"
		                           "/model [name] - View or switch model for this chat\n"
		                           "/clear or /reset - Clear conversation context\n"
		                           "/system [instructions] - View or set system instructions\n"
		                           "/history - View number of messages in context");
		purple_serv_got_im(gc, who, help_text, PURPLE_MESSAGE_SYSTEM, time(NULL));
		return 1;
	}

	/* Normal message: append to conversation history and submit to provider */
	chat_conversation_add_message(conv, "user", message);
	chatgpt_send_chat_request(cga, who);

	return 1;
}

void
chatgpt_fake_group_buddy(PurpleConnection *pc, const char *who, const char *old_group, const char *new_group)
{
	/* Do nothing */
}

void
chatgpt_fake_group_rename(PurpleConnection *pc, const char *old_name, PurpleGroup *group, GList *moved_buddies)
{
	/* Do nothing */
}

static void
chatgpt_login(PurpleAccount *account)
{
	PurpleConnection *pc = purple_account_get_connection(account);
	ChatGptAccount *cga = g_new0(ChatGptAccount, 1);
	PurpleConnectionFlags flags;
	const gchar *provider_str;
	const gchar *api_key;
	const gchar *api_url;
	const gchar *model;
	const gchar *default_buddy;
	GSList *buddies;

	purple_connection_set_protocol_data(pc, cga);

	flags = purple_connection_get_flags(pc);
	flags |= PURPLE_CONNECTION_FLAG_HTML | PURPLE_CONNECTION_FLAG_NO_BGCOLOR | PURPLE_CONNECTION_FLAG_NO_FONTSIZE;
	purple_connection_set_flags(pc, flags);

	cga->account = account;
	cga->pc = pc;
	cga->keepalive_pool = purple_http_keepalive_pool_new();
	cga->conns = purple_http_connection_set_new();
	cga->conversations = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chat_conversation_free);

	/* Determine Provider */
	provider_str = purple_account_get_string(account, "provider", "openai");
	if (purple_strequal(provider_str, "gemini")) {
		cga->provider = CHATGPT_PROVIDER_GEMINI;
	} else if (purple_strequal(provider_str, "claude")) {
		cga->provider = CHATGPT_PROVIDER_CLAUDE;
	} else if (purple_strequal(provider_str, "custom")) {
		cga->provider = CHATGPT_PROVIDER_CUSTOM;
	} else {
		cga->provider = CHATGPT_PROVIDER_OPENAI;
	}

	/* Determine API Key: check api_key option, then password, then legacy openai_token */
	api_key = purple_account_get_string(account, "api_key", NULL);
	if (api_key == NULL || *api_key == '\0') {
		api_key = purple_account_get_password(account);
	}
	if (api_key == NULL || *api_key == '\0') {
		api_key = purple_account_get_string(account, "openai_token", NULL);
	}
	cga->api_key = g_strdup(api_key);

	/* Determine API URL override */
	api_url = purple_account_get_string(account, "api_url", NULL);
	if (api_url != NULL && *api_url != '\0') {
		cga->api_url = g_strdup(api_url);
	} else {
		cga->api_url = g_strdup(chatgpt_provider_default_url(cga->provider));
	}

	/* Determine Default Model */
	model = purple_account_get_string(account, "model", NULL);
	if (model == NULL || *model == '\0') {
		model = purple_account_get_string(account, "default_model", NULL);
	}
	if (model != NULL && *model != '\0') {
		cga->default_model = g_strdup(model);
	} else {
		cga->default_model = g_strdup(chatgpt_provider_default_model(cga->provider));
	}

	/* Determine System Prompt */
	cga->system_prompt = g_strdup(purple_account_get_string(account, "system_prompt", "You are a helpful AI assistant."));

	purple_connection_update_progress(pc, "", 1, 1);
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	purple_connection_set_state(pc, PURPLE_CONNECTION_CONNECTED);
#endif

	/* Ensure default buddy exists on buddy list */
	default_buddy = chatgpt_provider_default_buddy_name(cga->provider);
	buddies = purple_blist_find_buddies(account, NULL);
	if (buddies == NULL) {
		purple_blist_add_buddy(purple_buddy_new(account, default_buddy, default_buddy), NULL, NULL, NULL);
		buddies = purple_blist_find_buddies(account, NULL);
	}

	/* Set all buddies online */
	while (buddies != NULL) {
		PurpleBuddy *b = buddies->data;
		purple_prpl_got_user_status(account, purple_buddy_get_name(b), "available", NULL);
		buddies = g_slist_delete_link(buddies, buddies);
	}

	/* Check for API Key presence (warning if absent on non-custom provider) */
	if ((cga->api_key == NULL || *cga->api_key == '\0') && cga->provider != CHATGPT_PROVIDER_CUSTOM) {
		purple_notify_warning(pc, _("API Key Missing"), _("No API Key configured"),
		                      _("Please enter your API Key in the account password or account settings."), NULL);
	} else {
		/* Load cached models and download latest available models */
		chatgpt_load_cached_models(cga);
		chatgpt_fetch_models(cga, FALSE);
	}
}

static void
chatgpt_close(PurpleConnection *pc)
{
	ChatGptAccount *cga;

	g_return_if_fail(pc != NULL);
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	purple_connection_set_state(pc, PURPLE_CONNECTION_DISCONNECTING);
#endif

	cga = purple_connection_get_protocol_data(pc);
	g_return_if_fail(cga != NULL);

	purple_http_conn_cancel_all(pc);
	if (cga->conns != NULL) {
		purple_http_connection_set_destroy(cga->conns);
		cga->conns = NULL;
	}
	if (cga->keepalive_pool != NULL) {
		purple_http_keepalive_pool_unref(cga->keepalive_pool);
		cga->keepalive_pool = NULL;
	}

	if (cga->conversations != NULL) {
		g_hash_table_destroy(cga->conversations);
		cga->conversations = NULL;
	}
	if (cga->available_models != NULL) {
		g_list_free_full(cga->available_models, g_free);
		cga->available_models = NULL;
	}

	g_free(cga->api_key);
	g_free(cga->api_url);
	g_free(cga->default_model);
	g_free(cga->system_prompt);
	g_free(cga);
}

gboolean
chatgpt_offline_message(const PurpleBuddy *buddy)
{
	return TRUE;
}

static PurpleCmdRet
chatgpt_cmd_model(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data)
{
	PurpleConnection *pc = purple_conversation_get_connection(conv);
	ChatGptAccount *cga = pc ? purple_connection_get_protocol_data(pc) : NULL;
	const gchar *who = purple_conversation_get_name(conv);
	ChatConversation *chat_conv;

	if (cga == NULL || who == NULL) {
		return PURPLE_CMD_RET_FAILED;
	}

	chat_conv = chat_conversation_get_or_create(cga, who);

	if (args && args[0] && *args[0]) {
		g_free(chat_conv->model);
		chat_conv->model = g_strdup(args[0]);
		purple_conversation_write(conv, "", _("Model changed for this conversation."), PURPLE_MESSAGE_SYSTEM, time(NULL));
	} else {
		const gchar *curr = chat_conv->model ? chat_conv->model :
		                    (cga->default_model ? cga->default_model : chatgpt_provider_default_model(cga->provider));
		gchar *msg = g_strdup_printf(_("Current model: %s"), curr);
		purple_conversation_write(conv, "", msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(msg);
	}

	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
chatgpt_cmd_clear(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data)
{
	PurpleConnection *pc = purple_conversation_get_connection(conv);
	ChatGptAccount *cga = pc ? purple_connection_get_protocol_data(pc) : NULL;
	const gchar *who = purple_conversation_get_name(conv);
	ChatConversation *chat_conv;

	if (cga == NULL || who == NULL) {
		return PURPLE_CMD_RET_FAILED;
	}

	chat_conv = chat_conversation_get_or_create(cga, who);
	g_list_free_full(chat_conv->messages, (GDestroyNotify)chat_message_free);
	chat_conv->messages = NULL;

	purple_conversation_write(conv, "", _("Conversation history cleared."), PURPLE_MESSAGE_SYSTEM, time(NULL));
	return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet
chatgpt_cmd_models(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data)
{
	PurpleConnection *pc = purple_conversation_get_connection(conv);
	ChatGptAccount *cga = pc ? purple_connection_get_protocol_data(pc) : NULL;
	GString *str;
	GList *l;

	if (cga == NULL) {
		return PURPLE_CMD_RET_FAILED;
	}

	if (cga->available_models == NULL) {
		chatgpt_load_cached_models(cga);
	}

	str = g_string_new(_("Available models:\n"));
	if (cga->available_models == NULL) {
		g_string_append(str, _("(Models not downloaded yet. Run /models after login or use Account Actions.)"));
		chatgpt_fetch_models(cga, FALSE);
	} else {
		for (l = cga->available_models; l != NULL; l = l->next) {
			g_string_append_printf(str, "• %s\n", (const gchar *)l->data);
		}
	}

	purple_conversation_write(conv, "", str->str, PURPLE_MESSAGE_SYSTEM, time(NULL));
	g_string_free(str, TRUE);

	return PURPLE_CMD_RET_OK;
}

/******************************************************************************/
/* Plugin registration and callbacks                                          */
/******************************************************************************/

#if !PURPLE_VERSION_CHECK(3, 0, 0)
void _purple_socket_init(void);
void _purple_socket_uninit(void);
#endif

static gboolean
plugin_load(PurplePlugin *plugin
#if PURPLE_VERSION_CHECK(3, 0, 0)
, GError **error
#endif
)
{
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	_purple_socket_init();
	purple_http_init();
#endif

	purple_cmd_register("model", "w", PURPLE_CMD_P_PLUGIN, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PROTOCOL_ONLY,
	                    CHATGPT_PLUGIN_ID, chatgpt_cmd_model,
	                    _("model [model]: View or change the model for this conversation"), NULL);

	purple_cmd_register("models", "", PURPLE_CMD_P_PLUGIN, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PROTOCOL_ONLY,
	                    CHATGPT_PLUGIN_ID, chatgpt_cmd_models,
	                    _("models: List available models downloaded from provider"), NULL);

	purple_cmd_register("clear", "", PURPLE_CMD_P_PLUGIN, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PROTOCOL_ONLY,
	                    CHATGPT_PLUGIN_ID, chatgpt_cmd_clear,
	                    _("clear: Clear conversation context history"), NULL);

	purple_cmd_register("reset", "", PURPLE_CMD_P_PLUGIN, PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PROTOCOL_ONLY,
	                    CHATGPT_PLUGIN_ID, chatgpt_cmd_clear,
	                    _("reset: Clear conversation context history"), NULL);

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin
#if PURPLE_VERSION_CHECK(3, 0, 0)
, GError **error
#endif
)
{
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	_purple_socket_uninit();
	purple_http_uninit();
#endif
	purple_signals_disconnect_by_handle(plugin);
	return TRUE;
}

static GList *
chatgpt_actions(
#if !PURPLE_VERSION_CHECK(3, 0, 0)
PurplePlugin *plugin, gpointer context
#else
PurpleConnection *pc
#endif
)
{
	GList *m = NULL;
	PurpleProtocolAction *act;

	act = purple_protocol_action_new(_("Select Model..."), chatgpt_action_select_model);
	m = g_list_append(m, act);

	act = purple_protocol_action_new(_("Refresh Available Models"), chatgpt_action_refresh_models);
	m = g_list_append(m, act);

	act = purple_protocol_action_new(_("Clear Conversation History"), chatgpt_action_clear_history);
	m = g_list_append(m, act);

	return m;
}

#if !PURPLE_VERSION_CHECK(2, 8, 0)
#	define OPT_PROTO_INVITE_MESSAGE 0x00000800
#endif

typedef struct 
{
	PurplePluginProtocolInfo parent;

	#if !PURPLE_VERSION_CHECK(2, 14, 0)
		char *(*get_cb_alias)(PurpleConnection *gc, int id, const char *who);
		gboolean (*chat_can_receive_file)(PurpleConnection *, int id);
		void (*chat_send_file)(PurpleConnection *, int id, const char *filename);
	#endif
} PurplePluginProtocolInfoExt;

#if !PURPLE_VERSION_CHECK(3, 0, 0)
static void
plugin_init(PurplePlugin *plugin)
{
	PurplePluginInfo *info = g_new0(PurplePluginInfo, 1);
	PurplePluginProtocolInfoExt *prpl_info_ext = g_new0(PurplePluginProtocolInfoExt, 1);
	PurplePluginProtocolInfo *prpl_info = (PurplePluginProtocolInfo *) prpl_info_ext;
#endif

#if PURPLE_VERSION_CHECK(3, 0, 0)
static void 
chatgpt_protocol_init(PurpleProtocol *prpl_info) 
{
	PurpleProtocol *info = prpl_info;
#endif
	PurpleAccountOption *opt;
	PurpleBuddyIconSpec icon_spec = {"jpeg", 0, 0, 96, 96, 0, PURPLE_ICON_SCALE_DISPLAY};
	GList *providers = NULL;
	PurpleKeyValuePair *kvp;

	info->id = CHATGPT_PLUGIN_ID;
	info->name = "ChatGPT";
	prpl_info->options = OPT_PROTO_IM_IMAGE;

#if !PURPLE_VERSION_CHECK(3, 0, 0)
#	define PRPL_APPEND_ACCOUNT_OPTION(opt) prpl_info->protocol_options = g_list_append(prpl_info->protocol_options, (opt));
	prpl_info->icon_spec = icon_spec;
#else
#	define PRPL_APPEND_ACCOUNT_OPTION(opt) prpl_info->account_options = g_list_append(prpl_info->account_options, (opt));
	prpl_info->icon_spec = &icon_spec;
#endif

#define ADD_PROVIDER(k, v) \
	kvp = g_new(PurpleKeyValuePair, 1); \
	kvp->key = g_strdup(v); \
	kvp->value = g_strdup(k); \
	providers = g_list_append(providers, kvp);

	ADD_PROVIDER("openai", "OpenAI (ChatGPT)");
	ADD_PROVIDER("gemini", "Google Gemini");
	ADD_PROVIDER("claude", "Anthropic Claude");
	ADD_PROVIDER("custom", "Custom / OpenAI Compatible");
#undef ADD_PROVIDER

	opt = purple_account_option_list_new(_("Provider"), "provider", providers);
	PRPL_APPEND_ACCOUNT_OPTION(opt);

	opt = purple_account_option_string_new(_("API Key / Token (leave blank to use account password)"), "api_key", NULL);
	PRPL_APPEND_ACCOUNT_OPTION(opt);

	opt = purple_account_option_string_new(_("API Endpoint / Base URL (optional override)"), "api_url", NULL);
	PRPL_APPEND_ACCOUNT_OPTION(opt);

	opt = purple_account_option_string_new(_("Model Name (leave blank for provider default)"), "model", NULL);
	PRPL_APPEND_ACCOUNT_OPTION(opt);

	opt = purple_account_option_string_new(_("System Instructions"), "system_prompt", "You are a helpful AI assistant.");
	PRPL_APPEND_ACCOUNT_OPTION(opt);

#undef PRPL_APPEND_ACCOUNT_OPTION

#if PURPLE_VERSION_CHECK(3, 0, 0)
}

static void 
chatgpt_protocol_class_init(PurpleProtocolClass *prpl_info) 
{
#endif
	prpl_info->login = chatgpt_login;
	prpl_info->close = chatgpt_close;
	prpl_info->status_types = chatgpt_status_types;
	prpl_info->list_icon = chatgpt_list_icon;
#if PURPLE_VERSION_CHECK(3, 0, 0)
}

static void 
chatgpt_protocol_client_iface_init(PurpleProtocolClientIface *prpl_info) 
{
	PurpleProtocolClientIface *info = prpl_info;
#endif

#if !PURPLE_VERSION_CHECK(3, 0, 0)
	info->actions = chatgpt_actions;
#else
	info->get_actions = chatgpt_actions;
#endif
	prpl_info->tooltip_text = chatgpt_tooltip_text;
	prpl_info->normalize = purple_normalize_nocase;
	prpl_info->offline_message = chatgpt_offline_message;
	prpl_info->get_account_text_table = NULL;
#if PURPLE_VERSION_CHECK(3, 0, 0)
}

static void 
chatgpt_protocol_server_iface_init(PurpleProtocolServerIface *prpl_info) 
{
#endif
	prpl_info->group_buddy = chatgpt_fake_group_buddy;
	prpl_info->rename_group = chatgpt_fake_group_rename;
#if PURPLE_VERSION_CHECK(3, 0, 0)
}

static void 
chatgpt_protocol_im_iface_init(PurpleProtocolIMIface *prpl_info) 
{
#endif
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	prpl_info->send_im = chatgpt_send_im;
#else
	prpl_info->send = chatgpt_send_im;
#endif
#if PURPLE_VERSION_CHECK(3, 0, 0)
}
#endif

#if !PURPLE_VERSION_CHECK(3, 0, 0)
	info->magic = PURPLE_PLUGIN_MAGIC;
	info->major_version = 2;
	info->minor_version = MIN(PURPLE_MINOR_VERSION, 8);
	info->type = PURPLE_PLUGIN_PROTOCOL;
	info->priority = PURPLE_PRIORITY_DEFAULT;
	info->version = CHATGPT_PLUGIN_VERSION;
	info->summary = N_("ChatGPT & AI Multi-Provider Protocol Plugin");
	info->description = N_("Chat with AI models from OpenAI, Google Gemini, Anthropic Claude, or local/custom endpoints.");
	info->author = "Eion Robb <eionrobb@gmail.com>";
	info->homepage = "http://github.com/EionRobb/pidgin-chatgpt";
	info->load = plugin_load;
	info->unload = plugin_unload;
	info->extra_info = prpl_info;

	#if PURPLE_MINOR_VERSION >= 5
		prpl_info->struct_size = sizeof(PurplePluginProtocolInfoExt);
	#endif

	plugin->info = info;
}
#endif

#if PURPLE_VERSION_CHECK(3, 0, 0)
PURPLE_DEFINE_TYPE_EXTENDED(
	ChatGptProtocol, chatgpt_protocol, PURPLE_TYPE_PROTOCOL, 0,
	PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_CLIENT_IFACE, chatgpt_protocol_client_iface_init)
	PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_SERVER_IFACE, chatgpt_protocol_server_iface_init)
	PURPLE_IMPLEMENT_INTERFACE_STATIC(PURPLE_TYPE_PROTOCOL_IM_IFACE, chatgpt_protocol_im_iface_init)
);

static gboolean
libpurple3_plugin_load(PurplePlugin *plugin, GError **error)
{
	chatgpt_protocol_register_type(plugin);
	chatgpt_protocol = purple_protocols_add(PURPLE_TYPE_PROTOCOL, error);
	if (!chatgpt_protocol)
		return FALSE;
	return plugin_load(plugin, error);
}

static gboolean
libpurple3_plugin_unload(PurplePlugin *plugin, GError **error)
{
	if (!plugin_unload(plugin, error))
		return FALSE;
	if (!purple_protocols_remove(chatgpt_protocol, error))
		return FALSE;
	return TRUE;
}

static PurplePluginInfo *
plugin_query(GError **error)
{
	return purple_plugin_info_new(
		"id",           CHATGPT_PLUGIN_ID,
		"name",         "ChatGPT Protocol",
		"version",      CHATGPT_PLUGIN_VERSION,
		"category",     N_("Protocol"),
		"summary",      N_("ChatGPT & AI Multi-Provider Protocol Plugin"),
		"description",  N_("Chat with AI models from OpenAI, Google Gemini, Anthropic Claude, or local/custom endpoints."),
		"website",      "http://github.com/EionRobb/pidgin-chatgpt",
		"abi-version",  PURPLE_ABI_VERSION,
		"flags",        PURPLE_PLUGIN_INFO_FLAGS_INTERNAL |
		                PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD,
		NULL
	);
}

PURPLE_PLUGIN_INIT(chatgpt, plugin_query, libpurple3_plugin_load, libpurple3_plugin_unload);
#else
static PurplePluginInfo aLovelyBunchOfCoconuts;
PURPLE_INIT_PLUGIN(chatgpt, plugin_init, aLovelyBunchOfCoconuts);
#endif
