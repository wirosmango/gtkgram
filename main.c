/*
 * GTKgram — Telegram client skeleton: GTK4 + TDLib (JSON interface), C.
 *
 * Flow:
 *   - a worker thread calls td_receive() and hands every JSON string
 *     to the GTK main thread via g_idle_add()
 *   - the main thread parses it with json-glib and drives the UI
 *   - authorization states switch pages of the top-level GtkStack:
 *     loading -> phone / qr -> code -> (password) -> main
 *   - "main" is a split view: chat list on the left, messages on the right
 *
 * Env: TG_API_ID, TG_API_HASH  (from https://my.telegram.org)
 */
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <qrencode.h>
#include <td/telegram/td_json_client.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int client_id;
    GThread *receiver;
    gint running;

    GtkWidget *window;
    GtkWidget *stack;    /* loading / phone / code / password / qr / main */
    GtkWidget *status;
    GtkWidget *phone_entry;
    GtkWidget *code_entry;
    GtkWidget *password_entry;

    /* QR login */
    GtkWidget *qr_area;
    QRcode *qr;          /* current QR code, NULL until a link arrives */
    gboolean qr_wanted;  /* user is on the QR page */

    /* sidebar */
    GtkWidget *sidebar;
    GtkWidget *chat_list;
    GtkWidget *search_entry;
    char *filter_text;
    GHashTable *chats;   /* gint64* chat_id -> GtkWidget* (content box of the row) */

    /* content pane */
    GtkWidget *content_stack; /* empty / chat */
    GtkWidget *back_btn;      /* chat -> list, visible in narrow mode only */
    gboolean narrow;          /* window is phone-sized */
    GtkWidget *chat_title;
    GtkWidget *profile_btn;
    GtkWidget *profile_pages;
    GtkWidget *profile_details;
    GtkWidget *profile_avatar;
    GtkWidget *message_entry;
    GtkWidget *send_button;
    gint64 profile_user_id;
    GHashTable *avatar_targets; /* file_id -> widgets waiting for local photo */
    GtkWidget *msg_list;
    GtkWidget *msg_scroll;
    GHashTable *messages;     /* gint64* message_id -> GtkWidget* (message box) */
    GHashTable *users;        /* gint64* user_id -> char* display name */
    GHashTable *file_slots;   /* gint64* file_id -> GPtrArray* of MediaSlot* waiting for it */
    gint64 current_chat;      /* 0 = no chat open */
    gint64 oldest_msg_id;     /* 0 = nothing loaded yet */
    guint history_target;     /* keep loading until this many messages are shown */
    gboolean history_loading;
    gboolean history_done;    /* reached the beginning of the chat */
    gboolean stick_bottom;    /* keep the view scrolled to the newest message */
    gboolean prepending;      /* older messages are being inserted on top */
    double last_upper;
} App;

static App app;

/* Window narrower than this (phone-sized): show either the chat list or the chat */
#define NARROW_WIDTH 600

static void update_layout(void);

/* ---------- TDLib helpers ---------- */

static JsonObject *td_request(const char *type)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "@type", type);
    return o;
}

/* Takes ownership of obj. */
static void td_send_object(JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    char *s = json_to_string(node, FALSE);
    td_send(app.client_id, s);
    g_free(s);
    json_node_unref(node);
}

static void td_send_string(const char *type, const char *field, const char *value)
{
    JsonObject *o = td_request(type);
    json_object_set_string_member(o, field, value);
    td_send_object(o);
}

static void set_status(const char *text)
{
    gtk_label_set_text(GTK_LABEL(app.status), text ? text : "");
}

static void send_tdlib_parameters(void)
{
    const char *api_id = g_getenv("TG_API_ID");
    const char *api_hash = g_getenv("TG_API_HASH");
    if (!api_id || !api_hash) {
        set_status("Set TG_API_ID and TG_API_HASH environment variables");
        return;
    }

    char *dir = g_build_filename(g_get_user_data_dir(), "tgtk", NULL);
    g_mkdir_with_parents(dir, 0700);

    JsonObject *o = td_request("setTdlibParameters");
    json_object_set_boolean_member(o, "use_test_dc", FALSE);
    json_object_set_string_member(o, "database_directory", dir);
    json_object_set_boolean_member(o, "use_file_database", TRUE);
    json_object_set_boolean_member(o, "use_chat_info_database", TRUE);
    json_object_set_boolean_member(o, "use_message_database", TRUE);
    json_object_set_boolean_member(o, "use_secret_chats", FALSE);
    json_object_set_int_member(o, "api_id", atoi(api_id));
    json_object_set_string_member(o, "api_hash", api_hash);
    json_object_set_string_member(o, "system_language_code", "en");
    json_object_set_string_member(o, "device_model", "Desktop");
    json_object_set_string_member(o, "application_version", "0.1");
    td_send_object(o);
    g_free(dir);
}

/* ---------- JSON helpers ---------- */

/* Like json_object_get_object_member(), but NULL instead of a warning when missing. */
static JsonObject *obj_member(JsonObject *o, const char *name)
{
    return json_object_has_member(o, name) ? json_object_get_object_member(o, name) : NULL;
}

/* int64 fields (e.g. chatPosition.order) arrive as JSON strings, int53 as numbers. */
static gint64 json_int64_flex(JsonObject *o, const char *name)
{
    if (!json_object_has_member(o, name))
        return 0;
    JsonNode *n = json_object_get_member(o, name);
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        return g_ascii_strtoll(json_node_get_string(n), NULL, 10);
    return json_node_get_int(n);
}

/* Plain-text rendering of a message content object. */
static char *content_to_text(JsonObject *content)
{
    const char *t = json_object_get_string_member_with_default(content, "@type", "");

    if (!strcmp(t, "messageText")) {
        JsonObject *text = obj_member(content, "text");
        return g_strdup(text ? json_object_get_string_member_with_default(text, "text", "") : "");
    }

    static const struct { const char *type, *label; } labels[] = {
        { "messagePhoto", "Photo" },       { "messageVideo", "Video" },
        { "messageAnimation", "GIF" },     { "messageAudio", "Audio" },
        { "messageVoiceNote", "Voice message" }, { "messageVideoNote", "Video message" },
        { "messageDocument", "File" },     { "messageSticker", "Sticker" },
        { "messageLocation", "Location" }, { "messageContact", "Contact" },
        { "messagePoll", "Poll" },         { "messageCall", "Call" },
    };

    JsonObject *cap = obj_member(content, "caption");
    const char *caption = cap ? json_object_get_string_member_with_default(cap, "text", "") : "";

    for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
        if (!strcmp(t, labels[i].type))
            return *caption ? g_strdup_printf("[%s] %s", labels[i].label, caption)
                            : g_strdup_printf("[%s]", labels[i].label);
    }
    /* service messages and everything else: show the type name */
    return g_strdup_printf("[%s]", t + (g_str_has_prefix(t, "message") ? 7 : 0));
}

/* ---------- Chat list (sidebar) ---------- */

#define AVATAR_COLORS 7

static void set_avatar_initial(GtkWidget *avatar, const char *title)
{
    char buf[8] = "?";
    if (title && *title && g_utf8_validate(title, -1, NULL)) {
        gunichar c = g_unichar_toupper(g_utf8_get_char(title));
        int n = g_unichar_to_utf8(c, buf);
        buf[n] = '\0';
    }
    gtk_label_set_text(GTK_LABEL(avatar), buf);
}

static void apply_avatar_file(JsonObject *file);

static JsonObject *avatar_small_file(JsonObject *photo)
{
    if (!photo) return NULL;
    /* chatPhotoInfo uses small_file; userProfilePhoto commonly uses small. */
    JsonObject *f = obj_member(photo, "small_file");
    if (!f) f = obj_member(photo, "small");
    return f;
}

static void request_avatar_file(JsonObject *photo, GtkWidget *target)
{
    JsonObject *small = avatar_small_file(photo);
    gint64 file_id = small ? json_object_get_int_member_with_default(small, "id", 0) : 0;
    if (!file_id || !target) return;

    GPtrArray *targets = g_hash_table_lookup(app.avatar_targets, &file_id);
    if (!targets) {
        gint64 *key = g_new(gint64, 1); *key = file_id;
        targets = g_ptr_array_new_with_free_func(g_object_unref);
        g_hash_table_insert(app.avatar_targets, key, targets);
    }
    if (!g_ptr_array_find(targets, target, NULL))
        g_ptr_array_add(targets, g_object_ref(target));

    JsonObject *local = obj_member(small, "local");
    const char *path = local ? json_object_get_string_member_with_default(local, "path", "") : "";
    gboolean complete = local && json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE);
    if (complete && *path) {
        JsonObject *fake = json_object_new();
        json_object_set_int_member(fake, "id", file_id);
        json_object_set_object_member(fake, "local", json_object_ref(local));
        apply_avatar_file(fake);
        json_object_unref(fake);
        return;
    }

    /* A target is registered before the request: updateFile can arrive quickly. */
    JsonObject *req = td_request("downloadFile");
    json_object_set_int_member(req, "file_id", file_id);
    json_object_set_int_member(req, "priority", 32);
    json_object_set_int_member(req, "offset", 0);
    json_object_set_int_member(req, "limit", 0);
    json_object_set_boolean_member(req, "synchronous", FALSE);
    td_send_object(req);
}

static void apply_avatar_file(JsonObject *file)
{
    JsonObject *local = obj_member(file, "local");
    if (!local || !json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE))
        return;
    gint64 id = json_object_get_int_member_with_default(file, "id", 0);
    GPtrArray *targets = g_hash_table_lookup(app.avatar_targets, &id);
    const char *path = json_object_get_string_member_with_default(local, "path", "");
    if (!targets || !*path) return;
    for (guint i = 0; i < targets->len; i++) {
        GtkWidget *picture = g_ptr_array_index(targets, i);
        if (GTK_IS_PICTURE(picture)) {
            gtk_picture_set_filename(GTK_PICTURE(picture), path);
            gtk_widget_set_visible(picture, TRUE);
            GtkWidget *placeholder = g_object_get_data(G_OBJECT(picture), "placeholder");
            if (placeholder) gtk_widget_set_visible(placeholder, FALSE);
        }
    }
    g_hash_table_remove(app.avatar_targets, &id);
}

/* [avatar circle] [title / last message preview] */
static GtkWidget *build_chat_row(gint64 id, const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget *avatar = gtk_overlay_new();
    GtkWidget *placeholder = gtk_label_new("");
    set_avatar_initial(placeholder, title);
    gtk_widget_add_css_class(placeholder, "avatar");
    char cls[16];
    g_snprintf(cls, sizeof cls, "avatar-%d", (int)(llabs(id) % AVATAR_COLORS));
    gtk_widget_add_css_class(placeholder, cls);
    gtk_widget_set_size_request(avatar, 48, 48);
    gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(avatar), placeholder);
    GtkWidget *picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(picture, 48, 48);
    gtk_widget_set_visible(picture, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(avatar), picture);
    g_object_set_data(G_OBJECT(picture), "placeholder", placeholder);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

    GtkWidget *title_label = gtk_label_new(title);
    gtk_widget_add_css_class(title_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);

    GtkWidget *preview = gtk_label_new("");
    gtk_widget_add_css_class(preview, "chat-preview");
    gtk_label_set_xalign(GTK_LABEL(preview), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(preview), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(preview), TRUE);

    gtk_box_append(GTK_BOX(text), title_label);
    gtk_box_append(GTK_BOX(text), preview);
    gtk_box_append(GTK_BOX(box), avatar);
    gtk_box_append(GTK_BOX(box), text);

    g_object_set_data(G_OBJECT(box), "avatar", placeholder);
    g_object_set_data(G_OBJECT(box), "avatar-picture", picture);
    g_object_set_data(G_OBJECT(box), "title", title_label);
    g_object_set_data(G_OBJECT(box), "preview", preview);
    return box;
}

static const char *chat_title_text(gint64 id)
{
    GtkWidget *box = g_hash_table_lookup(app.chats, &id);
    if (!box)
        return NULL;
    return gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title")));
}

static void add_or_update_chat(gint64 id, const char *title)
{
    GtkWidget *box = g_hash_table_lookup(app.chats, &id);
    if (box) {
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title")), title);
        set_avatar_initial(g_object_get_data(G_OBJECT(box), "avatar"), title);
        if (id == app.current_chat)
            gtk_label_set_text(GTK_LABEL(app.chat_title), title);
        return;
    }

    box = build_chat_row(id, title);
    gtk_list_box_append(GTK_LIST_BOX(app.chat_list), box);

    gint64 *key = g_new(gint64, 1);
    *key = id;
    g_hash_table_insert(app.chats, key, box);
    g_object_set_data(G_OBJECT(box), "chat-id", key);

    /* -1 = position not known yet (row stays visible); 0 = not in the main list */
    gint64 *order = g_new(gint64, 1);
    *order = -1;
    g_object_set_data_full(G_OBJECT(box), "order", order, g_free);
}

static void set_chat_preview(gint64 id, JsonObject *last_message)
{
    GtkWidget *box = g_hash_table_lookup(app.chats, &id);
    if (!box)
        return;

    char *text = g_strdup("");
    JsonObject *content = last_message ? obj_member(last_message, "content") : NULL;
    if (content) {
        char *body = content_to_text(content);
        g_free(text);
        gboolean out = json_object_get_boolean_member_with_default(last_message, "is_outgoing", FALSE);
        text = out ? g_strdup_printf("You: %s", body) : g_strdup(body);
        g_free(body);
        g_strdelimit(text, "\n", ' ');
    }
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "preview")), text);
    g_free(text);
}

static void set_chat_order(gint64 id, gint64 order)
{
    GtkWidget *box = g_hash_table_lookup(app.chats, &id);
    if (!box)
        return;
    gint64 *slot = g_object_get_data(G_OBJECT(box), "order");
    *slot = order;
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(app.chat_list));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(app.chat_list));
}

/* Order of the chat in the main chat list (0 if it is not there). */
static gint64 main_list_order(JsonArray *positions)
{
    for (guint i = 0; positions && i < json_array_get_length(positions); i++) {
        JsonObject *p = json_array_get_object_element(positions, i);
        JsonObject *list = p ? obj_member(p, "list") : NULL;
        if (list && !strcmp(json_object_get_string_member_with_default(list, "@type", ""), "chatListMain"))
            return json_int64_flex(p, "order");
    }
    return 0;
}

/* A full chat object (updateNewChat.chat or a getChat response). */
static void handle_chat_object(JsonObject *chat)
{
    gint64 id = json_object_get_int_member_with_default(chat, "id", 0);
    add_or_update_chat(id, json_object_get_string_member_with_default(chat, "title", "(untitled)"));
    set_chat_preview(id, obj_member(chat, "last_message"));
    GtkWidget *row = g_hash_table_lookup(app.chats, &id);
    JsonObject *photo = obj_member(chat, "photo");
    if (row && photo)
        request_avatar_file(photo, g_object_get_data(G_OBJECT(row), "avatar-picture"));
    if (json_object_has_member(chat, "positions"))
        set_chat_order(id, main_list_order(json_object_get_array_member(chat, "positions")));
}

/* Newest chats first; pinned chats come first thanks to TDLib's order values. */
static gint chat_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer d)
{
    (void)d;
    gint64 *oa = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(a)), "order");
    gint64 *ob = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(b)), "order");
    gint64 x = oa ? *oa : 0, y = ob ? *ob : 0;
    return (x < y) - (x > y);
}

static gboolean chat_filter(GtkListBoxRow *row, gpointer d)
{
    (void)d;
    GtkWidget *box = gtk_list_box_row_get_child(row);
    gint64 *order = g_object_get_data(G_OBJECT(box), "order");
    if (order && *order == 0)
        return FALSE; /* archived or otherwise not in the main list */

    if (!app.filter_text || !*app.filter_text)
        return TRUE;

    GtkLabel *title = g_object_get_data(G_OBJECT(box), "title");
    char *a = g_utf8_casefold(gtk_label_get_text(title), -1);
    char *b = g_utf8_casefold(app.filter_text, -1);
    gboolean match = strstr(a, b) != NULL;
    g_free(a);
    g_free(b);
    return match;
}

static void request_chats(void)
{
    JsonObject *list = td_request("chatListMain");
    JsonObject *o = td_request("loadChats");
    json_object_set_object_member(o, "chat_list", list);
    json_object_set_int_member(o, "limit", 50);
    json_object_set_string_member(o, "@extra", "load_chats");
    td_send_object(o);
}

/* ---------- Messages (content pane) ---------- */

#define HISTORY_PAGE 30

static void store_user(JsonObject *u)
{
    gint64 id = json_object_get_int_member_with_default(u, "id", 0);
    const char *first = json_object_get_string_member_with_default(u, "first_name", "");
    const char *last = json_object_get_string_member_with_default(u, "last_name", "");
    char *name = g_strstrip(g_strdup_printf("%s %s", first, last));

    if (*name) {
        gint64 *key = g_new(gint64, 1);
        *key = id;
        g_hash_table_insert(app.users, key, name);
    } else {
        g_free(name);
    }
}

static char *sender_name(JsonObject *msg)
{
    if (json_object_get_boolean_member_with_default(msg, "is_outgoing", FALSE))
        return g_strdup("You");

    JsonObject *s = obj_member(msg, "sender_id");
    if (!s)
        return g_strdup("?");

    const char *st = json_object_get_string_member_with_default(s, "@type", "");
    if (!strcmp(st, "messageSenderUser")) {
        gint64 uid = json_object_get_int_member_with_default(s, "user_id", 0);
        const char *name = g_hash_table_lookup(app.users, &uid);
        return name ? g_strdup(name) : g_strdup_printf("User %" G_GINT64_FORMAT, uid);
    }

    gint64 cid = json_object_get_int_member_with_default(s, "chat_id", 0);
    const char *title = chat_title_text(cid);
    return title ? g_strdup(title) : g_strdup_printf("Chat %" G_GINT64_FORMAT, cid);
}

/* ---------- Photo downloads and rendering ---------- */
static void request_photo_file(JsonObject *file)
{
    if (!file) return;
    JsonObject *local = obj_member(file, "local");
    const char *path = local ? json_object_get_string_member_with_default(local, "path", "") : "";
    if (local && json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE) && *path)
        return;
    JsonObject *req = td_request("downloadFile");
    json_object_set_int_member(req, "file_id", json_object_get_int_member_with_default(file, "id", 0));
    json_object_set_int_member(req, "priority", 16);
    json_object_set_int_member(req, "offset", 0);
    json_object_set_int_member(req, "limit", 0);
    json_object_set_boolean_member(req, "synchronous", FALSE);
    td_send_object(req);
}

static void register_photo_picture(gint64 file_id, GtkWidget *picture, JsonObject *file)
{
    if (!file_id || !picture || !file) return;
    GPtrArray *slots = g_hash_table_lookup(app.file_slots, &file_id);
    if (!slots) {
        gint64 *key = g_new(gint64, 1); *key = file_id;
        slots = g_ptr_array_new_with_free_func(g_object_unref);
        g_hash_table_insert(app.file_slots, key, slots);
    }
    g_ptr_array_add(slots, g_object_ref(picture));
    JsonObject *local = obj_member(file, "local");
    const char *path = local ? json_object_get_string_member_with_default(local, "path", "") : "";
    if (local && json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE) && *path)
        gtk_picture_set_filename(GTK_PICTURE(picture), path);
    else
        request_photo_file(file);
}

static void apply_photo_file(JsonObject *file)
{
    gint64 id = json_object_get_int_member_with_default(file, "id", 0);
    GPtrArray *slots = g_hash_table_lookup(app.file_slots, &id);
    JsonObject *local = obj_member(file, "local");
    const char *path = local ? json_object_get_string_member_with_default(local, "path", "") : "";
    if (!slots || !local || !json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE) || !*path)
        return;
    for (guint i = 0; i < slots->len; i++) {
        GtkWidget *media = g_ptr_array_index(slots, i);
        if (GTK_IS_PICTURE(media)) {
            gtk_picture_set_filename(GTK_PICTURE(media), path);
        } else if (GTK_IS_VIDEO(media)) {
            GtkMediaStream *stream = gtk_media_file_new_for_filename(path);
            gtk_media_stream_set_loop(GTK_MEDIA_STREAM(stream), TRUE);
            gtk_video_set_media_stream(GTK_VIDEO(media), GTK_MEDIA_STREAM(stream));
            gtk_media_stream_play(GTK_MEDIA_STREAM(stream));
            g_object_unref(stream);
        }
    }
}

static void apply_media_file(JsonObject *file)
{
    apply_photo_file(file);
}

static GtkWidget *build_photo_widget(JsonObject *content)
{
    JsonObject *photo = obj_member(content, "photo");
    JsonArray *sizes = photo ? json_object_get_array_member(photo, "sizes") : NULL;
    JsonObject *best = NULL;
    gint64 best_area = -1;
    for (guint i = 0; sizes && i < json_array_get_length(sizes); i++) {
        JsonObject *size = json_array_get_object_element(sizes, i);
        JsonObject *file = size ? obj_member(size, "photo") : NULL;
        if (!file) continue;
        gint64 area = (gint64)json_object_get_int_member_with_default(size, "width", 0) *
                      json_object_get_int_member_with_default(size, "height", 0);
        if (area > best_area) { best_area = area; best = size; }
    }
    if (!best) return NULL;
    JsonObject *file = obj_member(best, "photo");
    gint64 file_id = json_object_get_int_member_with_default(file, "id", 0);
    if (!file_id) return NULL;
    GtkWidget *picture = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_halign(picture, GTK_ALIGN_START);
    gint width = json_object_get_int_member_with_default(best, "width", 0);
    gint height = json_object_get_int_member_with_default(best, "height", 0);
    if (width > 0 && height > 0) {
        double scale = MIN(1.0, MIN(560.0 / width, 560.0 / height));
        gtk_widget_set_size_request(picture, MAX(1, (int)(width * scale)), MAX(1, (int)(height * scale)));
    } else gtk_widget_set_size_request(picture, 560, 360);
    gtk_widget_add_css_class(picture, "chat-photo");
    register_photo_picture(file_id, picture, file);
    return picture;
}

static GtkWidget *build_file_media_widget(JsonObject *file, gint width, gint height, gboolean animated)
{
    if (!file) return NULL;
    gint64 file_id = json_object_get_int_member_with_default(file, "id", 0);
    if (!file_id) return NULL;

    GtkWidget *widget;
    if (animated) {
        widget = gtk_video_new();
        gtk_video_set_autoplay(GTK_VIDEO(widget), TRUE);
        gtk_video_set_loop(GTK_VIDEO(widget), TRUE);
    } else {
        widget = gtk_picture_new();
        gtk_picture_set_can_shrink(GTK_PICTURE(widget), TRUE);
        gtk_picture_set_content_fit(GTK_PICTURE(widget), GTK_CONTENT_FIT_CONTAIN);
    }
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    if (width > 0 && height > 0) {
        double scale = MIN(1.0, MIN(420.0 / width, 420.0 / height));
        gtk_widget_set_size_request(widget, MAX(1, (int)(width * scale)), MAX(1, (int)(height * scale)));
    } else {
        gtk_widget_set_size_request(widget, 280, 280);
    }
    gtk_widget_add_css_class(widget, "chat-media");

    GPtrArray *slots = g_hash_table_lookup(app.file_slots, &file_id);
    if (!slots) {
        gint64 *key = g_new(gint64, 1); *key = file_id;
        slots = g_ptr_array_new_with_free_func(g_object_unref);
        g_hash_table_insert(app.file_slots, key, slots);
    }
    g_ptr_array_add(slots, g_object_ref(widget));

    JsonObject *local = obj_member(file, "local");
    const char *path = local ? json_object_get_string_member_with_default(local, "path", "") : "";
    gboolean complete = local && json_object_get_boolean_member_with_default(local, "is_downloading_completed", FALSE);
    if (complete && *path) {
        JsonObject *fake = json_object_new();
        json_object_set_int_member(fake, "id", file_id);
        json_object_set_object_member(fake, "local", json_object_ref(local));
        apply_media_file(fake);
        json_object_unref(fake);
    } else {
        request_photo_file(file);
    }
    return widget;
}

static void on_open_sticker_set(GtkButton *button, gpointer data)
{
    (void)button;
    gint64 set_id = (gint64)(gintptr)data;
    if (!set_id) return;
    JsonObject *req = td_request("getStickerSet");
    json_object_set_int_member(req, "set_id", set_id);
    json_object_set_string_member(req, "@extra", "sticker_set");
    td_send_object(req);
}

static void show_sticker_set(JsonObject *set)
{
    const char *title = json_object_get_string_member_with_default(set, "title", "Sticker pack");
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 360, 420);
    if (app.window) gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app.window));
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(list, 12);
    gtk_widget_set_margin_bottom(list, 12);
    gtk_widget_set_margin_start(list, 12);
    gtk_widget_set_margin_end(list, 12);
    JsonArray *stickers = json_object_get_array_member(set, "stickers");
    for (guint i = 0; stickers && i < json_array_get_length(stickers); i++) {
        JsonObject *st = json_array_get_object_element(stickers, i);
        if (!st) continue;
        const char *emoji = json_object_get_string_member_with_default(st, "emoji", "");
        JsonObject *file = obj_member(st, "sticker");
        JsonObject *format = obj_member(st, "format");
        const char *ft = format ? json_object_get_string_member_with_default(format, "@type", "") : "";
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *preview = NULL;
        if (!strcmp(ft, "stickerFormatTgs")) {
            preview = gtk_label_new("Animated TGS sticker");
        } else {
            preview = build_file_media_widget(file,
                json_object_get_int_member_with_default(st, "width", 0),
                json_object_get_int_member_with_default(st, "height", 0),
                !strcmp(ft, "stickerFormatWebm"));
        }
        if (preview) {
            gtk_widget_set_size_request(preview, 88, 88);
            gtk_box_append(GTK_BOX(row), preview);
        }
        GtkWidget *emoji_label = gtk_label_new(*emoji ? emoji : "Sticker");
        gtk_widget_set_valign(emoji_label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), emoji_label);
        gtk_box_append(GTK_BOX(list), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_window_set_child(GTK_WINDOW(dialog), scroll);
    gtk_window_present(GTK_WINDOW(dialog));
}

static GtkWidget *build_sticker_widget(JsonObject *content)
{
    JsonObject *sticker = obj_member(content, "sticker");
    JsonObject *file = sticker ? obj_member(sticker, "sticker") : NULL;
    if (!file) return NULL;
    JsonObject *format = obj_member(sticker, "format");
    const char *ft = format ? json_object_get_string_member_with_default(format, "@type", "") : "";
    gboolean video_sticker = !strcmp(ft, "stickerFormatWebm");
    gboolean unsupported_tgs = !strcmp(ft, "stickerFormatTgs");
    gint w = json_object_get_int_member_with_default(sticker, "width", 0);
    gint h = json_object_get_int_member_with_default(sticker, "height", 0);
    if (unsupported_tgs) {
        GtkWidget *label = gtk_label_new("[Animated sticker — TGS]");
        gtk_widget_add_css_class(label, "dim-label");
        return label;
    }
    GtkWidget *media = build_file_media_widget(file, w, h, video_sticker);
    gint64 set_id = json_int64_flex(sticker, "set_id");
    if (set_id) {
        GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        if (media) gtk_box_append(GTK_BOX(wrap), media);
        const char *emoji = json_object_get_string_member_with_default(sticker, "emoji", "");
        char *label = g_strdup_printf("%s  Sticker pack", emoji);
        GtkWidget *button = gtk_button_new_with_label(label);
        g_free(label);
        gtk_widget_set_halign(button, GTK_ALIGN_START);
        g_signal_connect(button, "clicked", G_CALLBACK(on_open_sticker_set), (gpointer)(gintptr)set_id);
        gtk_box_append(GTK_BOX(wrap), button);
        return wrap;
    }
    return media;
}

static GtkWidget *build_animation_widget(JsonObject *content)
{
    JsonObject *animation = obj_member(content, "animation");
    JsonObject *file = animation ? obj_member(animation, "animation") : NULL;
    if (!file) return NULL;
    gint w = json_object_get_int_member_with_default(animation, "width", 0);
    gint h = json_object_get_int_member_with_default(animation, "height", 0);
    return build_file_media_widget(file, w, h, TRUE);
}

static GtkWidget *build_message_widget(JsonObject *msg)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "message-card");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_halign(box, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char *name = sender_name(msg);
    GtkWidget *name_label = gtk_label_new(name);
    gtk_widget_add_css_class(name_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
    g_free(name);
    GDateTime *dt = g_date_time_new_from_unix_local(json_object_get_int_member_with_default(msg, "date", 0));
    char *ts = dt ? g_date_time_format(dt, "%d.%m %H:%M") : g_strdup("");
    GtkWidget *time_label = gtk_label_new(ts);
    gtk_widget_add_css_class(time_label, "dim-label");
    g_free(ts);
    if (dt) g_date_time_unref(dt);
    gtk_box_append(GTK_BOX(header), name_label);
    gtk_box_append(GTK_BOX(header), time_label);

    JsonObject *content = obj_member(msg, "content");
    const char *ctype = content ? json_object_get_string_member_with_default(content, "@type", "") : "";
    gboolean is_photo = !strcmp(ctype, "messagePhoto");
    gboolean is_sticker = !strcmp(ctype, "messageSticker");
    gboolean is_animation = !strcmp(ctype, "messageAnimation");
    char *text = content ? content_to_text(content) : g_strdup("");
    GtkWidget *body = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(body), TRUE);
    gtk_widget_set_halign(body, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(box), header);
    if (is_photo) {
        GtkWidget *photo_widget = build_photo_widget(content);
        if (photo_widget) gtk_box_append(GTK_BOX(box), photo_widget);
    } else if (is_sticker) {
        GtkWidget *sticker_widget = build_sticker_widget(content);
        if (sticker_widget) gtk_box_append(GTK_BOX(box), sticker_widget);
    } else if (is_animation) {
        GtkWidget *animation_widget = build_animation_widget(content);
        if (animation_widget) gtk_box_append(GTK_BOX(box), animation_widget);
    }
    if ((!is_photo && !is_sticker && !is_animation) ||
        (text && *text && strcmp(text, "[Photo]") && strcmp(text, "[Sticker]") && strcmp(text, "[GIF]"))) {
        gtk_box_append(GTK_BOX(box), body);
        g_object_set_data(G_OBJECT(box), "body", body);
    } else {
        g_object_unref(body);
        g_object_set_data(G_OBJECT(box), "body", NULL);
    }
    g_free(text);
    return box;
}

/* Returns TRUE if the message was new and has been added. */
static gboolean add_message(JsonObject *msg, gboolean at_end)
{
    gint64 chat_id = json_object_get_int_member_with_default(msg, "chat_id", 0);
    gint64 id = json_object_get_int_member_with_default(msg, "id", 0);

    if (chat_id != app.current_chat || g_hash_table_lookup(app.messages, &id))
        return FALSE;

    GtkWidget *w = build_message_widget(msg);
    if (at_end)
        gtk_list_box_append(GTK_LIST_BOX(app.msg_list), w);
    else
        gtk_list_box_prepend(GTK_LIST_BOX(app.msg_list), w);

    gint64 *key = g_new(gint64, 1);
    *key = id;
    g_hash_table_insert(app.messages, key, w);

    if (app.oldest_msg_id == 0 || id < app.oldest_msg_id)
        app.oldest_msg_id = id;
    return TRUE;
}

static void request_history(void)
{
    if (!app.current_chat || app.history_loading || app.history_done)
        return;
    app.history_loading = TRUE;

    JsonObject *o = td_request("getChatHistory");
    json_object_set_int_member(o, "chat_id", app.current_chat);
    json_object_set_int_member(o, "from_message_id", app.oldest_msg_id); /* 0 = newest */
    json_object_set_int_member(o, "offset", 0);
    json_object_set_int_member(o, "limit", HISTORY_PAGE);
    char *extra = g_strdup_printf("history:%" G_GINT64_FORMAT, app.current_chat);
    json_object_set_string_member(o, "@extra", extra);
    g_free(extra);
    td_send_object(o);
}

static gboolean clear_prepending(gpointer d)
{
    (void)d;
    app.prepending = FALSE;
    return G_SOURCE_REMOVE;
}

/* Response to getChatHistory: messages come newest-first, and each one is
 * prepended, which yields chronological order for any batch. */
static void handle_history(JsonObject *o, const char *extra)
{
    gint64 chat_id = g_ascii_strtoll(extra + strlen("history:"), NULL, 10);
    if (chat_id != app.current_chat)
        return; /* stale answer for a chat that was already closed */

    JsonArray *arr = json_object_get_array_member(o, "messages");
    guint added = 0;

    app.prepending = TRUE;
    for (guint i = 0; arr && i < json_array_get_length(arr); i++) {
        JsonNode *n = json_array_get_element(arr, i);
        if (JSON_NODE_HOLDS_OBJECT(n) && add_message(json_node_get_object(n), FALSE))
            added++;
    }
    g_timeout_add(200, clear_prepending, NULL);

    app.history_loading = FALSE;
    if (!added)
        app.history_done = TRUE; /* nothing new: start of the chat */
    else if (g_hash_table_size(app.messages) < app.history_target)
        request_history();       /* TDLib may return fewer messages than asked */
}

static void update_message_content(JsonObject *o)
{
    if (json_object_get_int_member_with_default(o, "chat_id", 0) != app.current_chat)
        return;
    gint64 id = json_object_get_int_member_with_default(o, "message_id", 0);
    GtkWidget *w = g_hash_table_lookup(app.messages, &id);
    JsonObject *content = obj_member(o, "new_content");
    if (!w || !content)
        return;

    char *text = content_to_text(content);
    GtkWidget *body = g_object_get_data(G_OBJECT(w), "body");
    if (body && GTK_IS_LABEL(body)) gtk_label_set_text(GTK_LABEL(body), text);
    g_free(text);
}

static void delete_messages(JsonObject *o)
{
    if (!json_object_get_boolean_member_with_default(o, "is_permanent", FALSE))
        return; /* only evicted from TDLib's cache */
    if (json_object_get_int_member_with_default(o, "chat_id", 0) != app.current_chat)
        return;

    JsonArray *ids = json_object_get_array_member(o, "message_ids");
    for (guint i = 0; ids && i < json_array_get_length(ids); i++) {
        gint64 id = json_array_get_int_element(ids, i);
        GtkWidget *w = g_hash_table_lookup(app.messages, &id);
        if (w) {
            gtk_list_box_remove(GTK_LIST_BOX(app.msg_list), gtk_widget_get_parent(w));
            g_hash_table_remove(app.messages, &id);
        }
    }
}

static void clear_messages(void)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app.msg_list)))
        gtk_list_box_remove(GTK_LIST_BOX(app.msg_list), child);
    g_hash_table_remove_all(app.messages);
    g_hash_table_remove_all(app.file_slots);
}

static void close_current_chat(void)
{
    if (!app.current_chat)
        return;
    JsonObject *o = td_request("closeChat");
    json_object_set_int_member(o, "chat_id", app.current_chat);
    td_send_object(o);
    app.current_chat = 0;
}

static void profile_set_line(const char *label, const char *value)
{
    if (!app.profile_details) return;
    char *line = g_strdup_printf("%s: %s", label, (value && *value) ? value : "—");
    GtkWidget *row = gtk_label_new(line);
    gtk_label_set_xalign(GTK_LABEL(row), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(row), TRUE);
    gtk_box_append(GTK_BOX(app.profile_details), row);
    g_free(line);
}

static void show_profile(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (!app.current_chat || !app.profile_pages) return;
    if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(app.profile_pages)), "profile")) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.profile_pages), "messages");
        gtk_button_set_icon_name(GTK_BUTTON(app.profile_btn), "contact-new-symbolic");
        gtk_widget_set_tooltip_text(app.profile_btn, "Chat information");
        return;
    }

    gtk_stack_set_visible_child_name(GTK_STACK(app.profile_pages), "profile");
    gtk_button_set_icon_name(GTK_BUTTON(app.profile_btn), "go-previous-symbolic");
    gtk_widget_set_tooltip_text(app.profile_btn, "Back to chat");

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app.profile_details)))
        gtk_box_remove(GTK_BOX(app.profile_details), child);
    profile_set_line("Loading", "Chat information…");

    JsonObject *req = td_request("getChat");
    json_object_set_int_member(req, "chat_id", app.current_chat);
    json_object_set_string_member(req, "@extra", "profile_chat");
    td_send_object(req);
}

static void hide_profile(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (!app.profile_pages) return;
    gtk_stack_set_visible_child_name(GTK_STACK(app.profile_pages), "messages");
    gtk_button_set_icon_name(GTK_BUTTON(app.profile_btn), "contact-new-symbolic");
    gtk_widget_set_tooltip_text(app.profile_btn, "Chat information");
}

static void handle_profile_chat(JsonObject *chat)
{
    if (!app.profile_details) return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app.profile_details)))
        gtk_box_remove(GTK_BOX(app.profile_details), child);
    char idbuf[64];
    g_snprintf(idbuf, sizeof idbuf, "%" G_GINT64_FORMAT,
               json_object_get_int_member_with_default(chat, "id", 0));
    profile_set_line("Chat ID", idbuf);
    JsonObject *type = obj_member(chat, "type");
    const char *kind = type ? json_object_get_string_member_with_default(type, "@type", "") : "";
    profile_set_line("Type", kind);
    const char *username = type ? json_object_get_string_member_with_default(type, "username", "") : "";
    if (*username) { char *u = g_strdup_printf("@%s", username); profile_set_line("Username", u); g_free(u); }
    const char *desc = json_object_get_string_member_with_default(chat, "description", "");
    if (*desc) profile_set_line("Description", desc);
    const char *title = json_object_get_string_member_with_default(chat, "title", "");
    if (*title) profile_set_line("Name", title);
    JsonObject *photo = obj_member(chat, "photo");
    if (photo && app.profile_avatar)
        request_avatar_file(photo, app.profile_avatar);
    if (type && !strcmp(kind, "chatTypePrivate")) {
        app.profile_user_id = json_object_get_int_member_with_default(type, "user_id", 0);
        if (app.profile_user_id) {
            JsonObject *req = td_request("getUserFullInfo");
            json_object_set_int_member(req, "user_id", app.profile_user_id);
            td_send_object(req);
        }
    } else if (type && !strcmp(kind, "chatTypeSupergroup")) {
        JsonObject *req = td_request("getSupergroupFullInfo");
        json_object_set_int_member(req, "supergroup_id",
            json_object_get_int_member_with_default(type, "supergroup_id", 0));
        td_send_object(req);
    } else if (type && !strcmp(kind, "chatTypeBasicGroup")) {
        JsonObject *req = td_request("getBasicGroupFullInfo");
        json_object_set_int_member(req, "basic_group_id",
            json_object_get_int_member_with_default(type, "basic_group_id", 0));
        td_send_object(req);
    }
}

static void send_current_message(void)
{
    if (!app.current_chat || !app.message_entry) return;
    const char *entry_text = gtk_editable_get_text(GTK_EDITABLE(app.message_entry));
    if (!entry_text || !*entry_text) return;

    char *message_text = g_strdup(entry_text);
    g_strstrip(message_text);
    if (!*message_text) { g_free(message_text); return; }

    JsonObject *formatted_text = td_request("formattedText");
    json_object_set_string_member(formatted_text, "text", message_text);
    json_object_set_array_member(formatted_text, "entities", json_array_new());

    JsonObject *input_content = td_request("inputMessageText");
    json_object_set_object_member(input_content, "text", formatted_text);
    json_object_set_object_member(input_content, "link_preview_options", td_request("linkPreviewOptions"));
    json_object_set_boolean_member(input_content, "clear_draft", TRUE);

    JsonObject *request = td_request("sendMessage");
    json_object_set_int_member(request, "chat_id", app.current_chat);
    json_object_set_object_member(request, "input_message_content", input_content);
    json_object_set_string_member(request, "@extra", "send_message");
    td_send_object(request);

    gtk_editable_set_text(GTK_EDITABLE(app.message_entry), "");
    if (app.send_button) gtk_widget_set_sensitive(app.send_button, FALSE);
    set_status(NULL);
    g_free(message_text);
}

static void on_message_entry_activate(GtkEntry *entry, gpointer data)
{
    (void)entry; (void)data;
    send_current_message();
}

static void on_send_button_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    send_current_message();
}

static void open_chat(gint64 id, const char *title)
{
    close_current_chat();
    clear_messages();

    app.current_chat = id;
    if (app.profile_pages)
        gtk_stack_set_visible_child_name(GTK_STACK(app.profile_pages), "messages");
    if (app.profile_btn) {
        gtk_button_set_icon_name(GTK_BUTTON(app.profile_btn), "contact-new-symbolic");
        gtk_widget_set_tooltip_text(app.profile_btn, "Chat information");
    }
    if (app.message_entry)
        gtk_editable_set_text(GTK_EDITABLE(app.message_entry), "");
    if (app.send_button)
        gtk_widget_set_sensitive(app.send_button, FALSE);
    if (app.profile_avatar) {
        gtk_picture_set_paintable(GTK_PICTURE(app.profile_avatar), NULL);
        gtk_widget_set_visible(app.profile_avatar, FALSE);
        GtkWidget *placeholder = g_object_get_data(G_OBJECT(app.profile_avatar), "placeholder");
        if (placeholder) gtk_widget_set_visible(placeholder, TRUE);
    }
    app.oldest_msg_id = 0;
    app.history_target = HISTORY_PAGE;
    app.history_loading = FALSE;
    app.history_done = FALSE;
    app.stick_bottom = TRUE;
    app.prepending = FALSE;

    gtk_label_set_text(GTK_LABEL(app.chat_title), title);
    gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "chat");
    update_layout(); /* narrow mode: the chat now covers the list */

    /* openChat makes TDLib deliver live updates for this chat */
    JsonObject *o = td_request("openChat");
    json_object_set_int_member(o, "chat_id", id);
    td_send_object(o);

    request_history();
}

/* ---------- Authorization state machine ---------- */

static void handle_auth_state(JsonObject *state)
{
    const char *t = json_object_get_string_member_with_default(state, "@type", "");

    if (!strcmp(t, "authorizationStateWaitTdlibParameters")) {
        send_tdlib_parameters();
    } else if (!strcmp(t, "authorizationStateWaitEncryptionKey")) {
        /* Older TDLib versions only */
        JsonObject *o = td_request("checkDatabaseEncryptionKey");
        json_object_set_string_member(o, "encryption_key", "");
        td_send_object(o);
    } else if (!strcmp(t, "authorizationStateWaitPhoneNumber")) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "phone");
    } else if (!strcmp(t, "authorizationStateWaitOtherDeviceConfirmation")) {
        /* TDLib sends this again with a fresh link every ~30 s */
        if (app.qr_wanted) {
            const char *link = json_object_get_string_member_with_default(state, "link", "");
            if (app.qr)
                QRcode_free(app.qr);
            app.qr = QRcode_encodeString(link, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
            gtk_widget_queue_draw(app.qr_area);
            gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "qr");
        }
    } else if (!strcmp(t, "authorizationStateWaitCode")) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "code");
    } else if (!strcmp(t, "authorizationStateWaitPassword")) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "password");
    } else if (!strcmp(t, "authorizationStateReady")) {
        set_status(NULL);
        gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "main");
        request_chats();
    } else if (!strcmp(t, "authorizationStateClosed")) {
        g_atomic_int_set(&app.running, 0);
        gtk_window_destroy(GTK_WINDOW(app.window));
    } else {
        /* TODO: WaitRegistration, WaitEmailAddress... */
        g_message("Unhandled authorization state: %s", t);
    }
}

/* ---------- Incoming JSON dispatch (main thread) ---------- */

static void handle_object(JsonObject *o)
{
    const char *type = json_object_get_string_member_with_default(o, "@type", "");
    const char *extra = json_object_get_string_member_with_default(o, "@extra", "");

    if (!strcmp(type, "updateAuthorizationState")) {
        handle_auth_state(json_object_get_object_member(o, "authorization_state"));
    } else if (!strcmp(type, "updateConnectionState")) {
        JsonObject *st = obj_member(o, "state");
        const char *s = st ? json_object_get_string_member_with_default(st, "@type", "") : "";
        if (!strcmp(s, "connectionStateReady"))
            set_status(NULL);
        else if (!strcmp(s, "connectionStateWaitingForNetwork"))
            set_status("Waiting for network...");
        else if (!strcmp(s, "connectionStateConnectingToProxy"))
            set_status("Connecting to proxy...");
        else if (!strcmp(s, "connectionStateConnecting"))
            set_status("Connecting to Telegram...");
    } else if (!strcmp(type, "updateFile")) {
        JsonObject *file = obj_member(o, "file");
        if (file) { apply_avatar_file(file); apply_photo_file(file); }
    } else if (!strcmp(type, "userFullInfo") || !strcmp(type, "supergroupFullInfo") ||
               !strcmp(type, "basicGroupFullInfo")) {
        JsonObject *info = obj_member(o, "user_full_info");
        if (!info) info = obj_member(o, "supergroup_full_info");
        if (!info) info = obj_member(o, "basic_group_full_info");
        if (info && app.profile_details) {
            const char *bio = json_object_get_string_member_with_default(info, "bio", "");
            if (!*bio) bio = json_object_get_string_member_with_default(info, "description", "");
            if (*bio) profile_set_line("Description", bio);
            const char *phone = json_object_get_string_member_with_default(info, "phone_number", "");
            if (*phone) profile_set_line("Phone", phone);
            if (json_object_has_member(info, "member_count")) {
                char count[64];
                g_snprintf(count, sizeof count, "%" G_GINT64_FORMAT, json_object_get_int_member(info, "member_count"));
                profile_set_line("Members", count);
            }
            const char *share = json_object_get_string_member_with_default(info, "share_text", "");
            if (*share) profile_set_line("Info", share);
        }
    } else if (!strcmp(type, "updateUser")) {
        JsonObject *u = obj_member(o, "user");
        if (u)
            store_user(u);
    } else if (!strcmp(type, "updateNewMessage")) {
        JsonObject *m = obj_member(o, "message");
        if (m)
            add_message(m, TRUE);
    } else if (!strcmp(type, "updateMessageContent")) {
        update_message_content(o);
    } else if (!strcmp(type, "updateDeleteMessages")) {
        delete_messages(o);
    } else if (!strcmp(type, "messages") && g_str_has_prefix(extra, "history:")) {
        handle_history(o, extra);
    } else if (!strcmp(type, "updateNewChat")) {
        JsonObject *chat = obj_member(o, "chat");
        if (chat)
            handle_chat_object(chat);
    } else if (!strcmp(type, "stickerSet") && !strcmp(extra, "sticker_set")) {
        show_sticker_set(o);
    } else if (!strcmp(type, "chat") && !strcmp(extra, "profile_chat")) {
        handle_profile_chat(o);
    } else if (!strcmp(type, "chat")) {
        handle_chat_object(o);
    } else if (!strcmp(type, "updateChatTitle")) {
        add_or_update_chat(json_object_get_int_member_with_default(o, "chat_id", 0),
                           json_object_get_string_member_with_default(o, "title", "(untitled)"));
    } else if (!strcmp(type, "updateChatLastMessage")) {
        gint64 id = json_object_get_int_member_with_default(o, "chat_id", 0);
        set_chat_preview(id, obj_member(o, "last_message"));
        if (json_object_has_member(o, "positions"))
            set_chat_order(id, main_list_order(json_object_get_array_member(o, "positions")));
    } else if (!strcmp(type, "updateChatPosition")) {
        JsonObject *p = obj_member(o, "position");
        JsonObject *list = p ? obj_member(p, "list") : NULL;
        if (list && !strcmp(json_object_get_string_member_with_default(list, "@type", ""), "chatListMain"))
            set_chat_order(json_object_get_int_member_with_default(o, "chat_id", 0),
                           json_int64_flex(p, "order"));
    } else if (!strcmp(type, "chats")) {
        JsonArray *ids = json_object_get_array_member(o, "chat_ids");
        for (guint i = 0; ids && i < json_array_get_length(ids); i++) {
            gint64 id = json_array_get_int_element(ids, i);
            if (g_hash_table_lookup(app.chats, &id))
                continue;
            JsonObject *req = td_request("getChat");
            json_object_set_int_member(req, "chat_id", id);
            td_send_object(req);
        }
    } else if (!strcmp(type, "error")) {
        gint64 code = json_object_get_int_member_with_default(o, "code", 0);
        /* loadChats answers 404 once everything is loaded — not a real error */
        if (!(code == 404 && !strcmp(extra, "load_chats")))
            set_status(json_object_get_string_member_with_default(o, "message", "Unknown error"));
    }

    /* a failed history request must not block further loading */
    if (!strcmp(type, "error") && g_str_has_prefix(extra, "history:"))
        app.history_loading = FALSE;

    /* loadChats finished (ok or 404): now the chat list can be fetched */
    if (!strcmp(extra, "load_chats")) {
        JsonObject *list = td_request("chatListMain");
        JsonObject *req = td_request("getChats");
        json_object_set_object_member(req, "chat_list", list);
        json_object_set_int_member(req, "limit", 50);
        td_send_object(req);
    }
}

static gboolean on_td_result(gpointer data)
{
    char *json = data;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            handle_object(json_node_get_object(root));
    }
    g_object_unref(parser);
    g_free(json);
    return G_SOURCE_REMOVE;
}

/* ---------- Receiver thread ---------- */

static gpointer receiver_thread(gpointer data)
{
    (void)data;
    while (g_atomic_int_get(&app.running)) {
        const char *res = td_receive(1.0);
        if (res) /* string is only valid until the next td_receive() */
            g_idle_add(on_td_result, g_strdup(res));
    }
    return NULL;
}

static void start_tdlib(void)
{
    td_execute("{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}");
    app.client_id = td_create_client_id();
    g_atomic_int_set(&app.running, 1);
    app.receiver = g_thread_new("td-receiver", receiver_thread, NULL);
    /* any request starts the flow of updates */
    td_send(app.client_id, "{\"@type\":\"getOption\",\"name\":\"version\"}");
}

/* ---------- UI: login pages ---------- */

static void on_phone(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    set_status(NULL);
    td_send_string("setAuthenticationPhoneNumber", "phone_number",
                   gtk_editable_get_text(GTK_EDITABLE(app.phone_entry)));
}

static void on_code(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    set_status(NULL);
    td_send_string("checkAuthenticationCode", "code",
                   gtk_editable_get_text(GTK_EDITABLE(app.code_entry)));
}

static void on_password(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    set_status(NULL);
    td_send_string("checkAuthenticationPassword", "password",
                   gtk_editable_get_text(GTK_EDITABLE(app.password_entry)));
}

static void on_qr_request(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    set_status(NULL);
    app.qr_wanted = TRUE;
    JsonObject *o = td_request("requestQrCodeAuthentication");
    json_object_set_array_member(o, "other_user_ids", json_array_new());
    td_send_object(o);
}

static void on_qr_back(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    /* WaitOtherDeviceConfirmation still accepts setAuthenticationPhoneNumber */
    app.qr_wanted = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "phone");
}

static void draw_qr(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer d)
{
    (void)area; (void)d;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!app.qr)
        return;

    const int quiet = 2; /* quiet-zone modules around the code */
    int n = app.qr->width;
    double scale = MIN(w, h) / (double)(n + 2 * quiet);
    double ox = (w - scale * n) / 2.0;
    double oy = (h - scale * n) / 2.0;

    cairo_set_source_rgb(cr, 0, 0, 0);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (app.qr->data[y * n + x] & 1)
                cairo_rectangle(cr, ox + x * scale, oy + y * scale, scale, scale);
    cairo_fill(cr);
}

static GtkWidget *make_login_page(const char *prompt, GtkWidget *entry, GCallback cb)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(box, 300, -1);

    GtkWidget *btn = gtk_button_new_with_label("Next");
    gtk_widget_add_css_class(btn, "suggested-action");

    gtk_box_append(GTK_BOX(box), gtk_label_new(prompt));
    gtk_box_append(GTK_BOX(box), entry);
    gtk_box_append(GTK_BOX(box), btn);

    g_signal_connect(btn, "clicked", cb, NULL);
    g_signal_connect(entry, "activate", cb, NULL);
    return box;
}

/* ---------- UI: main view (sidebar + content) ---------- */

/* Wide: list and chat side by side. Narrow: only one of them is visible —
 * the chat while one is open, otherwise the list. */
static void update_layout(void)
{
    gboolean chat_open = app.current_chat != 0;
    gtk_widget_set_visible(app.sidebar, !app.narrow || !chat_open);
    gtk_widget_set_visible(app.content_stack, !app.narrow || chat_open);
    gtk_widget_set_visible(app.back_btn, app.narrow);
}

static gboolean on_tick(GtkWidget *win, GdkFrameClock *clock, gpointer d)
{
    (void)clock; (void)d;
    int width = gtk_widget_get_width(win);
    if (width > 0) {
        gboolean narrow = width < NARROW_WIDTH;
        if (narrow != app.narrow) {
            app.narrow = narrow;
            update_layout();
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_back(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    close_current_chat();
    clear_messages();
    gtk_list_box_unselect_all(GTK_LIST_BOX(app.chat_list));
    gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "empty");
    update_layout();
}

static void on_chat_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d)
{
    (void)list; (void)d;
    GtkWidget *box = gtk_list_box_row_get_child(row);
    gint64 *id = g_object_get_data(G_OBJECT(box), "chat-id");
    if (id)
        open_chat(*id, chat_title_text(*id));
}

static void on_search_changed(GtkSearchEntry *entry, gpointer d)
{
    (void)d;
    g_free(app.filter_text);
    app.filter_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(app.chat_list));
}

static void on_search_mode(GObject *bar, GParamSpec *pspec, gpointer d)
{
    (void)pspec; (void)d;
    if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar)))
        gtk_editable_set_text(GTK_EDITABLE(app.search_entry), "");
}

/* Menu actions: exposed as win.logout / win.quit */
static void action_logout(GSimpleAction *action, GVariant *param, gpointer d)
{
    (void)action; (void)param; (void)d;
    set_status("Logging out...");
    td_send(app.client_id, "{\"@type\":\"logOut\"}");
}

static void action_quit(GSimpleAction *action, GVariant *param, gpointer d)
{
    (void)action; (void)param; (void)d;
    gtk_window_close(GTK_WINDOW(app.window));
}

static const GActionEntry win_actions[] = {
    { .name = "logout", .activate = action_logout },
    { .name = "quit", .activate = action_quit },
};

/* Scrolling behaviour of the message view:
 *  - stay glued to the newest message while the user is at the bottom
 *  - keep the visible position when older messages are inserted on top */
static void on_vadj_changed(GtkAdjustment *adj, gpointer d)
{
    (void)d;
    double upper = gtk_adjustment_get_upper(adj);
    if (app.stick_bottom)
        gtk_adjustment_set_value(adj, upper - gtk_adjustment_get_page_size(adj));
    else if (app.prepending && upper > app.last_upper)
        gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + (upper - app.last_upper));
    app.last_upper = upper;
}

static void on_vadj_value_changed(GtkAdjustment *adj, gpointer d)
{
    (void)d;
    app.stick_bottom = gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj)
                       >= gtk_adjustment_get_upper(adj) - 2.0;
}

static void on_edge_reached(GtkScrolledWindow *sw, GtkPositionType pos, gpointer d)
{
    (void)sw; (void)d;
    if (pos == GTK_POS_TOP && app.current_chat && !app.history_loading && !app.history_done) {
        app.history_target = g_hash_table_size(app.messages) + HISTORY_PAGE;
        request_history();
    }
}

/* Left pane: [search] GTKgram [menu] / search bar / chat list */
static GtkWidget *build_sidebar(void)
{
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app.sidebar = sidebar;
    gtk_widget_add_css_class(sidebar, "tg-sidebar");
    gtk_widget_set_size_request(sidebar, 260, -1);

    /* header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(header, 6);
    gtk_widget_set_margin_bottom(header, 6);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);

    GtkWidget *search_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(search_btn), "system-search-symbolic");
    gtk_widget_add_css_class(search_btn, "flat");

    GtkWidget *title = gtk_label_new("GTKgram");
    gtk_widget_add_css_class(title, "title-3");
    gtk_widget_set_hexpand(title, TRUE);

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_widget_add_css_class(menu_btn, "flat");

    /* GMenu model -> GtkPopoverMenu with native GTK4 menu items */
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Log out", "win.logout");
    g_menu_append(menu, "Quit", "win.quit");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);

    gtk_box_append(GTK_BOX(header), search_btn);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), menu_btn);

    /* search bar, toggled by the search button */
    GtkWidget *search_bar = gtk_search_bar_new();
    app.search_entry = gtk_search_entry_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(search_bar), app.search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(search_bar), GTK_EDITABLE(app.search_entry));
    g_object_bind_property(search_btn, "active", search_bar, "search-mode-enabled",
                           G_BINDING_BIDIRECTIONAL);
    g_signal_connect(app.search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(search_bar, "notify::search-mode-enabled", G_CALLBACK(on_search_mode), NULL);

    /* chat list: the selected (= open) row gets the accent color from the theme */
    app.chat_list = gtk_list_box_new();
    gtk_widget_add_css_class(app.chat_list, "chat-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.chat_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(app.chat_list), chat_sort, NULL, NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(app.chat_list), chat_filter, NULL, NULL);
    g_signal_connect(app.chat_list, "row-activated", G_CALLBACK(on_chat_activated), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app.chat_list);
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(sidebar), header);
    gtk_box_append(GTK_BOX(sidebar), search_bar);
    gtk_box_append(GTK_BOX(sidebar), scroll);
    return sidebar;
}

/* Right pane: placeholder until a chat is opened, then title + messages */
static void on_message_entry_changed(GtkEditable *editable, gpointer data)
{
    (void)data;
    const char *text = gtk_editable_get_text(editable);
    if (app.send_button)
        gtk_widget_set_sensitive(app.send_button, app.current_chat != 0 && text && *text);
}

static GtkWidget *build_content(void)
{
    app.content_stack = gtk_stack_new();

    GtkWidget *empty = gtk_label_new("Select a chat to start reading");
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_stack_add_named(GTK_STACK(app.content_stack), empty, "empty");

    GtkWidget *chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app.chat_title = gtk_label_new("");
    gtk_widget_add_css_class(app.chat_title, "title-4");
    gtk_label_set_xalign(GTK_LABEL(app.chat_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app.chat_title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_top(app.chat_title, 14);
    gtk_widget_set_margin_bottom(app.chat_title, 14);
    gtk_widget_set_margin_start(app.chat_title, 16);
    gtk_widget_set_margin_end(app.chat_title, 16);
    gtk_widget_set_hexpand(app.chat_title, TRUE);

    app.back_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(app.back_btn, "flat");
    gtk_widget_set_valign(app.back_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(app.back_btn, 8);
    gtk_widget_set_visible(app.back_btn, FALSE);
    g_signal_connect(app.back_btn, "clicked", G_CALLBACK(on_back), NULL);

    GtkWidget *chat_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(chat_header), app.back_btn);
    gtk_box_append(GTK_BOX(chat_header), app.chat_title);
    app.profile_btn = gtk_button_new_from_icon_name("contact-new-symbolic");
    gtk_widget_add_css_class(app.profile_btn, "flat");
    gtk_widget_set_tooltip_text(app.profile_btn, "Chat information");
    g_signal_connect(app.profile_btn, "clicked", G_CALLBACK(show_profile), NULL);
    gtk_box_append(GTK_BOX(chat_header), app.profile_btn);

    app.profile_pages = gtk_stack_new();
    gtk_widget_set_vexpand(app.profile_pages, TRUE);

    /* Messages page, including the composer so it never disappears. */
    GtkWidget *messages_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app.msg_list = gtk_list_box_new();
    gtk_widget_add_css_class(app.msg_list, "msg-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.msg_list), GTK_SELECTION_NONE);
    app.msg_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app.msg_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app.msg_scroll), app.msg_list);
    gtk_widget_set_vexpand(app.msg_scroll, TRUE);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app.msg_scroll));
    g_signal_connect(vadj, "changed", G_CALLBACK(on_vadj_changed), NULL);
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_vadj_value_changed), NULL);
    g_signal_connect(app.msg_scroll, "edge-reached", G_CALLBACK(on_edge_reached), NULL);
    gtk_box_append(GTK_BOX(messages_page), app.msg_scroll);

    GtkWidget *composer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(composer, 10);
    gtk_widget_set_margin_bottom(composer, 10);
    gtk_widget_set_margin_start(composer, 12);
    gtk_widget_set_margin_end(composer, 12);
    app.message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app.message_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.message_entry), "Message...");
    g_signal_connect(app.message_entry, "activate", G_CALLBACK(on_message_entry_activate), NULL);
    g_signal_connect(app.message_entry, "changed", G_CALLBACK(on_message_entry_changed), NULL);
    app.send_button = gtk_button_new_from_icon_name("mail-send-symbolic");
    gtk_widget_add_css_class(app.send_button, "suggested-action");
    gtk_widget_set_tooltip_text(app.send_button, "Send message");
    gtk_widget_set_sensitive(app.send_button, FALSE);
    g_signal_connect(app.send_button, "clicked", G_CALLBACK(on_send_button_clicked), NULL);
    gtk_box_append(GTK_BOX(composer), app.message_entry);
    gtk_box_append(GTK_BOX(composer), app.send_button);
    gtk_box_append(GTK_BOX(messages_page), composer);
    gtk_stack_add_named(GTK_STACK(app.profile_pages), messages_page, "messages");

    /* Inline profile page: chat name remains in the top header; avatar and
       information are shown below it in the same chat pane. */
    GtkWidget *profile_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(profile_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *profile_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(profile_page, 22);
    gtk_widget_set_margin_bottom(profile_page, 22);
    gtk_widget_set_margin_start(profile_page, 24);
    gtk_widget_set_margin_end(profile_page, 24);
    gtk_widget_set_halign(profile_page, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(profile_page, TRUE);

    GtkWidget *avatar_overlay = gtk_overlay_new();
    GtkWidget *avatar_placeholder = gtk_label_new("?");
    gtk_widget_add_css_class(avatar_placeholder, "avatar");
    gtk_widget_add_css_class(avatar_placeholder, "profile-avatar");
    gtk_widget_set_size_request(avatar_placeholder, 112, 112);
    gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), avatar_placeholder);
    app.profile_avatar = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(app.profile_avatar), GTK_CONTENT_FIT_COVER);
    gtk_widget_add_css_class(app.profile_avatar, "profile-avatar");
    gtk_widget_set_size_request(app.profile_avatar, 112, 112);
    gtk_widget_set_visible(app.profile_avatar, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), app.profile_avatar);
    g_object_set_data(G_OBJECT(app.profile_avatar), "placeholder", avatar_placeholder);
    gtk_box_append(GTK_BOX(profile_page), avatar_overlay);

    app.profile_details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(app.profile_details, TRUE);
    gtk_box_append(GTK_BOX(profile_page), app.profile_details);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(profile_scroll), profile_page);
    gtk_stack_add_named(GTK_STACK(app.profile_pages), profile_scroll, "profile");
    gtk_stack_set_visible_child_name(GTK_STACK(app.profile_pages), "messages");

    gtk_box_append(GTK_BOX(chat), chat_header);
    gtk_box_append(GTK_BOX(chat), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(chat), app.profile_pages);
    gtk_stack_add_named(GTK_STACK(app.content_stack), chat, "chat");
    gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "empty");
    return app.content_stack;
}

static void load_css(void)
{
    static const char *css =
        ".tg-sidebar { background: alpha(currentColor, 0.06); }\n"
        ".chat-list { background: transparent; }\n"
        ".chat-list row { border-radius: 10px; margin: 2px 6px; padding: 0; }\n"
        ".chat-preview { opacity: 0.75; }\n"
        ".msg-list { background: transparent; }\n"
        ".avatar { color: white; font-weight: bold; font-size: 18px;"
        "         min-width: 44px; min-height: 44px; border-radius: 22px; }\n"
        "picture { border-radius: 24px; }\n"
        ".avatar-0 { background: #e17076; }\n"
        ".avatar-1 { background: #faa774; }\n"
        ".avatar-2 { background: #a695e7; }\n"
        ".avatar-3 { background: #7bc862; }\n"
        ".avatar-4 { background: #6ec9cb; }\n"
        ".avatar-5 { background: #65aadd; }\n"
        ".avatar-6 { background: #ee7aae; }\n"
        ".profile-avatar { border-radius: 56px; }\n"
        ".message-card { padding: 10px 12px; border-radius: 14px; background: alpha(@card_bg_color, 0.72); }\n"
        ".chat-photo, .chat-media { border-radius: 10px; }\n";

    GtkCssProvider *provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css);
#else
    gtk_css_provider_load_from_data(provider, css, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static gboolean on_close_request(GtkWindow *win, gpointer d)
{
    (void)win; (void)d;
    /* Let TDLib flush its database; the window is destroyed on
     * authorizationStateClosed. */
    set_status("Closing...");
    td_send(app.client_id, "{\"@type\":\"close\"}");
    return TRUE;
}

static void activate(GtkApplication *gtk_app, gpointer d)
{
    (void)d;
    load_css();

    app.window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app.window), "GTKgram");
    g_action_map_add_action_entries(G_ACTION_MAP(app.window), win_actions,
                                    G_N_ELEMENTS(win_actions), NULL);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 640);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app.stack = gtk_stack_new();
    gtk_widget_set_vexpand(app.stack, TRUE);

    /* loading */
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
    gtk_widget_set_size_request(spinner, 32, 32);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(app.stack), spinner, "loading");

    /* login: phone (+ QR button) */
    app.phone_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.phone_entry), "+1234567890");
    GtkWidget *phone_page = make_login_page("Phone number", app.phone_entry, G_CALLBACK(on_phone));
    GtkWidget *qr_btn = gtk_button_new_with_label("Log in with QR code");
    g_signal_connect(qr_btn, "clicked", G_CALLBACK(on_qr_request), NULL);
    gtk_box_append(GTK_BOX(phone_page), qr_btn);
    gtk_stack_add_named(GTK_STACK(app.stack), phone_page, "phone");

    /* login: QR page */
    GtkWidget *qr_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(qr_page, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(qr_page, GTK_ALIGN_CENTER);
    GtkWidget *qr_hint = gtk_label_new("On your phone: Settings → Devices →\nLink Desktop Device, then scan this code");
    gtk_label_set_justify(GTK_LABEL(qr_hint), GTK_JUSTIFY_CENTER);
    app.qr_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(app.qr_area), 280);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app.qr_area), 280);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app.qr_area), draw_qr, NULL, NULL);
    GtkWidget *qr_back = gtk_button_new_with_label("Use phone number instead");
    g_signal_connect(qr_back, "clicked", G_CALLBACK(on_qr_back), NULL);
    gtk_box_append(GTK_BOX(qr_page), qr_hint);
    gtk_box_append(GTK_BOX(qr_page), app.qr_area);
    gtk_box_append(GTK_BOX(qr_page), qr_back);
    gtk_stack_add_named(GTK_STACK(app.stack), qr_page, "qr");

    /* login: code and 2FA password */
    app.code_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.code_entry), "12345");
    gtk_stack_add_named(GTK_STACK(app.stack),
        make_login_page("Code from Telegram", app.code_entry, G_CALLBACK(on_code)), "code");

    app.password_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(app.password_entry), TRUE);
    gtk_stack_add_named(GTK_STACK(app.stack),
        make_login_page("2FA password", app.password_entry, G_CALLBACK(on_password)), "password");

    /* main: chat list on the left, messages take the rest */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), build_sidebar());
    gtk_paned_set_end_child(GTK_PANED(paned), build_content());
    gtk_paned_set_position(GTK_PANED(paned), 320);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_stack_add_named(GTK_STACK(app.stack), paned, "main");

    /* status line */
    app.status = gtk_label_new("");
    gtk_widget_add_css_class(app.status, "error");
    gtk_widget_set_margin_top(app.status, 6);
    gtk_widget_set_margin_bottom(app.status, 6);

    gtk_box_append(GTK_BOX(vbox), app.stack);
    gtk_box_append(GTK_BOX(vbox), app.status);
    gtk_window_set_child(GTK_WINDOW(app.window), vbox);

    g_signal_connect(app.window, "close-request", G_CALLBACK(on_close_request), NULL);
    gtk_widget_add_tick_callback(app.window, on_tick, NULL, NULL); /* watches the window width */

    gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "loading");
    gtk_window_present(GTK_WINDOW(app.window));

    start_tdlib();
}

int main(int argc, char **argv)
{
    app.chats = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    app.messages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    app.users = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    app.avatar_targets = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    app.file_slots = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

    GtkApplication *gtk_app = gtk_application_new("org.example.tgtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);

    g_atomic_int_set(&app.running, 0);
    if (app.receiver)
        g_thread_join(app.receiver);
    g_object_unref(gtk_app);
    g_hash_table_destroy(app.chats);
    g_hash_table_destroy(app.messages);
    g_hash_table_destroy(app.users);
    g_hash_table_destroy(app.avatar_targets);
    g_hash_table_destroy(app.file_slots);
    g_free(app.filter_text);
    if (app.qr)
        QRcode_free(app.qr);
    return status;
}
