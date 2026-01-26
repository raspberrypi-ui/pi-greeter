/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <glib-unix.h>

#include <locale.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <cairo-xlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gslist.h>
#include <gtk-layer-shell/gtk-layer-shell.h>

#include <lightdm.h>

static LightDMGreeter *greeter;
static GKeyFile *state;
static gchar *state_filename;

/* Defaults */
static GdkPixbuf *default_background_pixbuf = NULL;

/* Panel Widgets */
static GtkWidget *menubar, *power_menuitem, *session_menuitem, *language_menuitem;
static GtkMenu *session_menu, *language_menu;

/* Login Window Widgets */
static GtkWindow *login_window;
static GtkImage *user_image;
static GtkComboBox *user_combo;
static GtkEntry *username_entry, *password_entry;
static GtkLabel *message_label;
static GtkInfoBar *info_bar;
static GtkButton *cancel_button, *login_button;

static GtkWindow *onboard_window;

#define MAX_SCREENS 5
static GtkWindow *bg_window[MAX_SCREENS];

/* Pending Questions */
static GSList *pending_questions = NULL;

/* Current choices */
static gchar *current_session;
static gchar *current_language;

/* Screensaver values */
int timeout, interval, prefer_blanking, allow_exposures;
int screensaver_timeout;

static GdkRGBA *default_background_color = NULL;
static gboolean cancelling = FALSE, prompted = FALSE;
static gboolean prompt_active = FALSE, password_prompted = FALSE;
static gchar *wp_mode = NULL;

static gboolean wayland = FALSE;

typedef struct
{
  gboolean is_prompt;
  union {
    LightDMMessageType message;
    LightDMPromptType prompt;
  } type;
  gchar *text;
} PAMConversationMessage;

typedef struct
{
    gint value;
    /* +0 and -0 */
    gint sign;
    /* interpret 'value' as percentage of screen width/height */
    gboolean percentage;
    /* -1: left/top, 0: center, +1: right,bottom */
    gint anchor;
} DimensionPosition;

typedef struct
{
    DimensionPosition x, y;
} WindowPosition;

const WindowPosition CENTERED_WINDOW_POS = { .x = {50, +1, TRUE, 0}, .y = {50, +1, TRUE, 0} };
WindowPosition main_window_pos;

GdkPixbuf* default_user_pixbuf = NULL;
gchar* default_user_icon = "avatar-default";

static void
pam_message_finalize (PAMConversationMessage *message)
{
    g_free (message->text);
    g_free (message);
}


static void
add_indicator_to_panel (GtkWidget *indicator_item, gint index)
{
    gint insert_pos = 0;
    GList* items = gtk_container_get_children (GTK_CONTAINER (menubar));
    GList* item;
    for (item = items; item; item = item->next)
    {
        if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item->data), "indicator-custom-index-data")) < index)
            break;
        insert_pos++;
    }
    g_list_free (items);

    gtk_menu_shell_insert (GTK_MENU_SHELL (menubar), GTK_WIDGET (indicator_item), insert_pos);
}


static gboolean
menu_item_accel_closure_cb (GtkAccelGroup *accel_group,
                            GObject *acceleratable, guint keyval,
                            GdkModifierType modifier, gpointer data)
{
    gtk_menu_item_activate (data);
    return FALSE;
}

/* Maybe unnecessary (in future) trick to enable accelerators for hidden/detached menu items */
static void
reassign_menu_item_accel (GtkWidget *item)
{
    GtkAccelKey key;
    const gchar *accel_path = gtk_menu_item_get_accel_path (GTK_MENU_ITEM (item));

    if (accel_path && gtk_accel_map_lookup_entry (accel_path, &key))
    {
        GClosure *closure = g_cclosure_new (G_CALLBACK (menu_item_accel_closure_cb), item, NULL);
        gtk_accel_group_connect (gtk_menu_get_accel_group (GTK_MENU (gtk_widget_get_parent (item))),
                                 key.accel_key, key.accel_mods, key.accel_flags, closure);
        g_closure_unref (closure);
    }

    gtk_container_foreach (GTK_CONTAINER (gtk_menu_item_get_submenu (GTK_MENU_ITEM (item))),
                           (GtkCallback)reassign_menu_item_accel, NULL);
}

static void
init_indicators (GKeyFile* config)
{
    gchar **names = NULL;
    gsize length = 0;
    guint i;
    GHashTable *builtin_items = NULL;
    GHashTableIter iter;
    gpointer iter_value;
    gboolean fallback = FALSE;

    if (g_key_file_has_key (config, "greeter", "indicators", NULL))
    {   /* no option = default list, empty value = empty list */
        names = g_key_file_get_string_list (config, "greeter", "indicators", &length, NULL);
    }
    else if (g_key_file_has_key (config, "greeter", "show-indicators", NULL))
    {   /* fallback mode: no option = empty value = default list */
        names = g_key_file_get_string_list (config, "greeter", "show-indicators", &length, NULL);
        if (length == 0)
            fallback = TRUE;
    }

    if (names && !fallback)
    {
        builtin_items = g_hash_table_new (g_str_hash, g_str_equal);

        g_hash_table_insert (builtin_items, "~power", power_menuitem);
        g_hash_table_insert (builtin_items, "~session", session_menuitem);
        g_hash_table_insert (builtin_items, "~language", language_menuitem);

        g_hash_table_iter_init (&iter, builtin_items);
        while (g_hash_table_iter_next (&iter, NULL, &iter_value))
            gtk_container_remove (GTK_CONTAINER (menubar), iter_value);
    }

    for (i = 0; i < length; ++i)
    {
        if (names[i][0] == '~' && g_hash_table_lookup_extended (builtin_items, names[i], NULL, &iter_value))
        {   /* Built-in indicators */
            g_object_set_data (G_OBJECT (iter_value), "indicator-custom-index-data", GINT_TO_POINTER (i));
            add_indicator_to_panel (iter_value, i);
            g_hash_table_remove (builtin_items, (gconstpointer)names[i]);
            continue;
        }
    }
    if (names)
        g_strfreev (names);

    if (builtin_items)
    {
        g_hash_table_iter_init (&iter, builtin_items);
        while (g_hash_table_iter_next (&iter, NULL, &iter_value))
        {
            reassign_menu_item_accel (iter_value);
            gtk_widget_hide (iter_value);
        }

        g_hash_table_unref (builtin_items);
    }
}

static gboolean
is_valid_session (GList* items, const gchar* session)
{
    for (; items; items = g_list_next (items))
        if (g_strcmp0 (session, lightdm_session_get_key (items->data)) == 0)
            return TRUE;
    return FALSE;
}

static gchar *
get_session (void)
{
    return g_strdup (lightdm_greeter_get_default_session_hint (greeter));
}

static void
set_session (const gchar *session)
{
    gchar *last_session = NULL;
    GList *sessions = lightdm_get_sessions ();

    /* Validation */
    if (!session || !is_valid_session (sessions, session))
    {
        /* previous session */
        last_session = g_key_file_get_value (state, "greeter", "last-session", NULL);
        if (last_session && g_strcmp0 (session, last_session) != 0 &&
            is_valid_session (sessions, last_session))
            session = last_session;
        else
        {
            /* default */
            const gchar* default_session = lightdm_greeter_get_default_session_hint (greeter);
            if (g_strcmp0 (session, default_session) != 0 &&
                is_valid_session (sessions, default_session))
                session = default_session;
            /* first in the sessions list */
            else if (sessions)
                session = lightdm_session_get_key (sessions->data);
            /* give up */
            else
                session = NULL;
        }
    }

    if (gtk_widget_get_visible (session_menuitem))
    {
        GList *menu_iter = NULL;
        GList *menu_items = gtk_container_get_children (GTK_CONTAINER (session_menu));
        if (session)
        {
            for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
                if (g_strcmp0 (session, g_object_get_data (G_OBJECT (menu_iter->data), "session-key")) == 0)
                {
                    break;
                }
        }
        if (!menu_iter)
            menu_iter = menu_items;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_iter->data), TRUE);
    }

    g_free (current_session);
    current_session = g_strdup(session);
    g_free (last_session);
}

static gchar *
get_language (void)
{
    GList *menu_items, *menu_iter;

    /* if the user manually selected a language, use it */
    if (current_language)
        return g_strdup (current_language);

    menu_items = gtk_container_get_children(GTK_CONTAINER(language_menu));    
    for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
    {
        if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu_iter->data)))
        {
            return g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code"));
        }
    }

    return NULL;
}

static void
set_language (const gchar *language)
{
    const gchar *default_language = NULL;    
    GList *menu_items, *menu_iter;

    if (!gtk_widget_get_visible (language_menuitem))
    {
        g_free (current_language);
        current_language = g_strdup (language);
        return;
    }

    menu_items = gtk_container_get_children(GTK_CONTAINER(language_menu));

    if (language)
    {
        for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
        {
            gchar *s;
            gboolean matched;
            s = g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code"));
            matched = g_strcmp0 (s, language) == 0;
            g_free (s);
            if (matched)
            {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_iter->data), TRUE);
                g_free (current_language);
                current_language = g_strdup(language);
                gtk_menu_item_set_label(GTK_MENU_ITEM(language_menuitem),language);
                return;
            }
        }
    }

    /* If failed to find this language, then try the default */
    if (lightdm_get_language ()) {
        default_language = lightdm_language_get_code (lightdm_get_language ());
        gtk_menu_item_set_label(GTK_MENU_ITEM (language_menuitem), default_language);
    }
    if (default_language && g_strcmp0 (default_language, language) != 0)
        set_language (default_language);
    /* If all else fails, just use the first language from the menu */
    else {
        for (menu_iter = menu_items; menu_iter != NULL; menu_iter = g_list_next(menu_iter))
        {
            if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu_iter->data)))
                gtk_menu_item_set_label(GTK_MENU_ITEM(language_menuitem), g_strdup(g_object_get_data (G_OBJECT (menu_iter->data), "language-code")));
        }
    }
}

static void
set_message_label (const gchar *text)
{
    gtk_widget_set_visible (GTK_WIDGET (info_bar), g_strcmp0 (text, "") != 0);
    gtk_label_set_text (message_label, text);
}

static void
set_login_button_label (LightDMGreeter *greeter, const gchar *username)
{
    LightDMUser *user;
    gboolean logged_in = FALSE;

    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
        logged_in = lightdm_user_get_logged_in (user);
    if (logged_in)
        gtk_button_set_label (login_button, _("Unlock"));
    else
        gtk_button_set_label (login_button, _("Log In"));
    gtk_widget_set_can_default (GTK_WIDGET (login_button), TRUE);
    gtk_widget_grab_default (GTK_WIDGET (login_button));
    /* and disable the session and language widgets */
    gtk_widget_set_sensitive (GTK_WIDGET (session_menuitem), !logged_in);
    gtk_widget_set_sensitive (GTK_WIDGET (language_menuitem), !logged_in);
}

static void
set_user_image (const gchar *username)
{
    const gchar *path;
    LightDMUser *user;
    GdkPixbuf *image = NULL;
    GError *error = NULL;

    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
    {
        path = lightdm_user_get_image (user);
        if (path)
        {
            image = gdk_pixbuf_new_from_file_at_scale (path, 80, 80, FALSE, &error);
            if (image)
            {
                gtk_image_set_from_pixbuf (GTK_IMAGE (user_image), image);
                g_object_unref (image);
                return;
            }
            else
            {
                g_warning ("Failed to load user image: %s", error->message);
                g_clear_error (&error);
            }
        }
    }
    
    if (default_user_pixbuf)
        gtk_image_set_from_pixbuf (GTK_IMAGE (user_image), default_user_pixbuf);
    else
        gtk_image_set_from_icon_name (GTK_IMAGE (user_image), default_user_icon, GTK_ICON_SIZE_DIALOG);
}

/* Function translate user defined coordinates to absolute value */
static gint
get_absolute_position (const DimensionPosition *p, gint screen, gint window)
{
    gint x = p->percentage ? (screen*p->value)/100 : p->value;
    x = p->sign < 0 ? screen - x : x;
    if (p->anchor > 0)
        x -= window;
    else if (p->anchor == 0)
        x -= window/2;

    if (x < 0)                     /* Offscreen: left/top */
        return 0;
    else if (x + window > screen)  /* Offscreen: right/bottom */
        return screen - window;
    else
        return x;
}

static void draw_background (cairo_t *c, GdkPixbuf *bg, gint m_width, gint m_height)
{
    GdkPixbuf *p = NULL;
    gint p_height, p_width, offset_x = 0, offset_y = 0;
    gdouble scale_x, scale_y;

    gdk_cairo_set_source_rgba (c, default_background_color);
    cairo_rectangle (c, 0, 0, m_width, m_height);
    cairo_fill (c);

    if (bg && strcmp (wp_mode, "color"))
    {
        p_width = gdk_pixbuf_get_width (bg);
        p_height = gdk_pixbuf_get_height (bg);
        scale_x = (float) m_width / p_width;
        scale_y = (float) m_height / p_height;

        if (!strcmp (wp_mode, "tile")) p = gdk_pixbuf_copy (bg);
        else
        {
            if (!strcmp (wp_mode, "fit"))
            {
                p_width *= scale_y < scale_x ? scale_y : scale_x;
                p_height *= scale_y < scale_x ? scale_y : scale_x;
            }
            else if (!strcmp (wp_mode, "crop"))
            {
                p_width *= scale_x < scale_y ? scale_y : scale_x;
                p_height *= scale_x < scale_y ? scale_y : scale_x;
            }
            else if (!strcmp (wp_mode, "stretch"))
            {
                p_width = m_width;
                p_height = m_height;
            }
            offset_x = (m_width - p_width) / 2;
            offset_y = (m_height - p_height) / 2;
            if (gdk_pixbuf_get_width (bg) == m_width && gdk_pixbuf_get_height (bg) == m_height)
                p = gdk_pixbuf_copy (bg);
            else p = gdk_pixbuf_scale_simple (bg, p_width, p_height, GDK_INTERP_BILINEAR);
        }
        gdk_cairo_set_source_pixbuf (c, p, offset_x, offset_y);
        if (!strcmp (wp_mode, "tile")) cairo_pattern_set_extend (cairo_get_source (c), CAIRO_EXTEND_REPEAT);
    }
    cairo_paint (c);
}

static gboolean
background_window_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    GdkWindow *wd = gtk_widget_get_window (widget);

    draw_background (cr, default_background_pixbuf, gdk_window_get_width (wd), gdk_window_get_height (wd));
    return FALSE;
}

static void
start_authentication (const gchar *username)
{
    gchar *data;
    gsize data_length;
    GError *error = NULL;

    cancelling = FALSE;
    prompted = FALSE;
    password_prompted = FALSE;
    prompt_active = FALSE;

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    g_key_file_set_value (state, "greeter", "last-user", username);
    data = g_key_file_to_data (state, &data_length, &error);
    if (error)
        g_warning ("Failed to save state file: %s", error->message);
    g_clear_error (&error);
    if (data)
    {
        g_file_set_contents (state_filename, data, data_length, &error);
        if (error)
            g_warning ("Failed to save state file: %s", error->message);
        g_clear_error (&error);
    }
    g_free (data);

    if (g_strcmp0 (username, "*other") == 0)
    {
        gtk_widget_show (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        lightdm_greeter_authenticate (greeter, NULL, NULL);
    }
    else if (g_strcmp0 (username, "*guest") == 0)
    {
        lightdm_greeter_authenticate_as_guest (greeter, NULL);
    }
    else
    {
        LightDMUser *user;

        user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
        if (user)
        {
            if (!current_session)
                set_session (lightdm_user_get_session (user));
            if (!current_language)
                set_language (lightdm_user_get_language (user));
        }
        else
        {
            set_session (NULL);
            set_language (NULL);
        }

        lightdm_greeter_authenticate (greeter, username, NULL);
    }
}

static void
cancel_authentication (void)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean other = FALSE;

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    /* If in authentication then stop that first */
    cancelling = FALSE;
    if (lightdm_greeter_get_in_authentication (greeter))
    {
        cancelling = TRUE;
        lightdm_greeter_cancel_authentication (greeter, NULL);
        set_message_label ("");
    }

    /* Make sure password entry is back to normal */
    gtk_entry_set_visibility (password_entry, FALSE);

    /* Force refreshing the prompt_box for "Other" */
    model = gtk_combo_box_get_model (user_combo);

    if (gtk_combo_box_get_active_iter (user_combo, &iter))
    {
        gchar *user;

        gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, 0, &user, -1);
        other = (g_strcmp0 (user, "*other") == 0);
        g_free (user);
    }

    /* Start a new login or return to the user list */
    if (other || lightdm_greeter_get_hide_users_hint (greeter))
        start_authentication ("*other");
    else
        gtk_widget_grab_focus (GTK_WIDGET (user_combo));
}

static void
start_session (void)
{
    gchar *language;
    gchar *session;
    gchar *data;
    gsize data_length;
    GError *error = NULL;

    language = get_language ();
    if (language)
        lightdm_greeter_set_language (greeter, language, NULL);
    g_free (language);

    session = get_session ();

    /* Remember last choice */
    g_key_file_set_value (state, "greeter", "last-session", session);

    data = g_key_file_to_data (state, &data_length, &error);
    if (error)
        g_warning ("Failed to save state file: %s", error->message);
    g_clear_error (&error);
    if (data)
    {
        g_file_set_contents (state_filename, data, data_length, &error);
        if (error)
            g_warning ("Failed to save state file: %s", error->message);
        g_clear_error (&error);
    }
    g_free (data);

    if (!lightdm_greeter_start_session_sync (greeter, session, NULL))
    {
        set_message_label (_("Failed to start session"));
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
    }
    g_free (session);
}

void
session_selected_cb(GtkMenuItem *menuitem, gpointer user_data);
G_MODULE_EXPORT
void
session_selected_cb(GtkMenuItem *menuitem, gpointer user_data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)))
    {
       gchar *session = g_object_get_data (G_OBJECT (menuitem), "session-key");
       set_session(session);
    }
}

void
language_selected_cb(GtkMenuItem *menuitem, gpointer user_data);
G_MODULE_EXPORT
void
language_selected_cb(GtkMenuItem *menuitem, gpointer user_data)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)))
    {
       gchar *language = g_object_get_data (G_OBJECT (menuitem), "language-code");
       set_language(language);
    }
}

gboolean
password_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
password_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if ((event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down) &&
        gtk_widget_get_visible(GTK_WIDGET(user_combo)))
    {
        gboolean available;
        GtkTreeIter iter;
        GtkTreeModel *model = gtk_combo_box_get_model (user_combo);

        /* Back to username_entry if it is available */
        if (event->keyval == GDK_KEY_Up &&
            gtk_widget_get_visible (GTK_WIDGET (username_entry)) && widget == GTK_WIDGET (password_entry))
        {
            gtk_widget_grab_focus (GTK_WIDGET (username_entry));
            return TRUE;
        }

        if (!gtk_combo_box_get_active_iter (user_combo, &iter))
            return FALSE;

        if (event->keyval == GDK_KEY_Up)
        {
            GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
            available = gtk_tree_path_prev (path);
            if (available)
                available = gtk_tree_model_get_iter (model, &iter, path);
            gtk_tree_path_free (path);
        }
        else
            available = gtk_tree_model_iter_next (model, &iter);

        if (available)
            gtk_combo_box_set_active_iter (user_combo, &iter);

        return TRUE;
    }
    return FALSE;
}

gboolean
username_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
username_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    if (!g_strcmp0(gtk_entry_get_text(username_entry), "") == 0)
        start_authentication(gtk_entry_get_text(username_entry));
    return FALSE;
}

gboolean
username_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
username_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    /* Acts as password_entry */
    if (event->keyval == GDK_KEY_Up)
        return password_key_press_cb (widget, event, user_data);
    /* Enter activates the password entry */
    else if (event->keyval == GDK_KEY_Return)
    {
        gtk_widget_grab_focus(GTK_WIDGET(password_entry));
        return TRUE;
    }
    else
        return FALSE;
}

gboolean
menubar_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
menubar_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    switch (event->keyval)
    {
    case GDK_KEY_Tab: case GDK_KEY_Escape:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R:
    case GDK_KEY_F9: case GDK_KEY_F10:
    case GDK_KEY_F11: case GDK_KEY_F12:
        gtk_menu_shell_cancel (GTK_MENU_SHELL (menubar));
        gtk_window_present (login_window);
        return TRUE;
    default:
        return FALSE;
    };
}

gboolean
login_window_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
G_MODULE_EXPORT
gboolean
login_window_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    GtkWidget *item = NULL;
    return FALSE;

    if (event->keyval == GDK_KEY_F9)
        item = session_menuitem;
    else if (event->keyval == GDK_KEY_F10)
        item = language_menuitem;
    else if (event->keyval == GDK_KEY_F12)
        item = power_menuitem;
    else if (event->keyval != GDK_KEY_Escape &&
             event->keyval != GDK_KEY_Super_L &&
             event->keyval != GDK_KEY_Super_R)
        return FALSE;

    if (GTK_IS_MENU_ITEM (item) && gtk_widget_is_sensitive (item) && gtk_widget_get_visible (item))
        gtk_menu_shell_select_item (GTK_MENU_SHELL (menubar), item);
    else
        gtk_menu_shell_select_first (GTK_MENU_SHELL (menubar), TRUE);
    return TRUE;
}

static void set_displayed_user (LightDMGreeter *greeter, gchar *username)
{
    gchar *user_tooltip;
    LightDMUser *user;

    if (g_strcmp0 (username, "*other") == 0)
    {
        gtk_widget_show (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        user_tooltip = g_strdup(_("Other"));
    }
    else
    {
        gtk_widget_hide (GTK_WIDGET (username_entry));
        gtk_widget_show (GTK_WIDGET (cancel_button));
        gtk_widget_grab_focus (GTK_WIDGET (password_entry));
        user_tooltip = g_strdup(username);
    }

    if (g_strcmp0 (username, "*guest") == 0)
    {
        user_tooltip = g_strdup(_("Guest Session"));
    }

    set_login_button_label (greeter, username);
    //set_user_background (username);
    set_user_image (username);
    user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
    if (user)
    {
        set_language (lightdm_user_get_language (user));
        set_session (lightdm_user_get_session (user));
    }
    else
        set_language (lightdm_language_get_code (lightdm_get_language ()));
    gtk_widget_set_tooltip_text (GTK_WIDGET (user_combo), user_tooltip);
    start_authentication (username);
    g_free (user_tooltip);
}

void user_combobox_active_changed_cb (GtkComboBox *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
user_combobox_active_changed_cb (GtkComboBox *widget, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    model = gtk_combo_box_get_model (user_combo);

    if (gtk_combo_box_get_active_iter (user_combo, &iter))
    {
        gchar *user;

        gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, 0, &user, -1);

        set_displayed_user(greeter, user);

        g_free (user);
    }
    set_message_label ("");
}

static const gchar*
get_message_label (void)
{
    return gtk_label_get_text (message_label);
}

static void
process_prompts (LightDMGreeter *greeter)
{
    if (!pending_questions)
        return;

    /* always allow the user to change username again */
    gtk_widget_set_sensitive (GTK_WIDGET (username_entry), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET (password_entry), TRUE);

    /* Special case: no user selected from list, so PAM asks us for the user
     * via a prompt. For that case, use the username field */
    if (!prompted && pending_questions && !pending_questions->next &&
        ((PAMConversationMessage *) pending_questions->data)->is_prompt &&
        ((PAMConversationMessage *) pending_questions->data)->type.prompt != LIGHTDM_PROMPT_TYPE_SECRET &&
        gtk_widget_get_visible ((GTK_WIDGET (username_entry))) &&
        lightdm_greeter_get_authentication_user (greeter) == NULL)
    {
        prompted = TRUE;
        prompt_active = TRUE;
        gtk_widget_grab_focus (GTK_WIDGET (username_entry));
        return;
    }

    while (pending_questions)
    {
        PAMConversationMessage *message = (PAMConversationMessage *) pending_questions->data;
        pending_questions = g_slist_remove (pending_questions, (gconstpointer) message);

        if (!message->is_prompt)
        {
            /* FIXME: this doesn't show multiple messages, but that was
             * already the case before. */
            set_message_label (message->text);
            continue;
        }

        gtk_entry_set_text (password_entry, "");
        gtk_entry_set_visibility (password_entry, message->type.prompt != LIGHTDM_PROMPT_TYPE_SECRET);
        if (get_message_label()[0] == 0 && password_prompted)
        {
            /* No message was provided beforehand and this is not the
             * first password prompt, so use the prompt as label,
             * otherwise the user will be completely unclear of what
             * is going on. Actually, the fact that prompt messages are
             * not shown is problematic in general, especially if
             * somebody uses a custom PAM module that wants to ask
             * something different. */
            gchar *str = message->text;
            if (g_str_has_suffix (str, ": "))
                str = g_strndup (str, strlen (str) - 2);
            else if (g_str_has_suffix (str, ":"))
                str = g_strndup (str, strlen (str) - 1);
            set_message_label (str);
            if (str != message->text)
                g_free (str);
        }
        gtk_widget_grab_focus (GTK_WIDGET (password_entry));
        prompted = TRUE;
        password_prompted = TRUE;
        prompt_active = TRUE;

        /* If we have more stuff after a prompt, assume that other prompts are pending,
         * so stop here. */
        break;
    }
}

void login_cb (GtkWidget *widget);
G_MODULE_EXPORT
void
login_cb (GtkWidget *widget)
{
    /* Reset to default screensaver values */
    if (!wayland && lightdm_greeter_get_lock_hint (greeter))
        XSetScreenSaver(gdk_x11_display_get_xdisplay(gdk_display_get_default ()), timeout, interval, prefer_blanking, allow_exposures);        

    gtk_widget_set_sensitive (GTK_WIDGET (username_entry), FALSE);
    gtk_widget_set_sensitive (GTK_WIDGET (password_entry), FALSE);
    set_message_label ("");
    prompt_active = FALSE;

    if (lightdm_greeter_get_is_authenticated (greeter))
        start_session ();
    else if (lightdm_greeter_get_in_authentication (greeter))
    {
        lightdm_greeter_respond (greeter, gtk_entry_get_text (password_entry), NULL);
        /* If we have questions pending, then we continue processing
         * those, until we are done. (Otherwise, authentication will
         * not complete.) */
        if (pending_questions)
            process_prompts (greeter);
    }
    else
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
}

void cancel_cb (GtkWidget *widget);
G_MODULE_EXPORT
void
cancel_cb (GtkWidget *widget)
{
    //cancel_authentication ();
    lightdm_shutdown (NULL);
}

static void
show_prompt_cb (LightDMGreeter *greeter, const gchar *text, LightDMPromptType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = TRUE;
        message_obj->type.prompt = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
show_message_cb (LightDMGreeter *greeter, const gchar *text, LightDMMessageType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = FALSE;
        message_obj->type.message = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
authentication_complete_cb (LightDMGreeter *greeter)
{
    prompt_active = FALSE;
    gtk_entry_set_text (password_entry, "");

    if (cancelling)
    {
        cancel_authentication ();
        return;
    }

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    if (lightdm_greeter_get_is_authenticated (greeter))
    {
        if (prompted)
            start_session ();
    }
    else
    {
        if (prompted)
        {
            if (get_message_label()[0] == 0)
                set_message_label (_("Incorrect password, please try again"));
            start_authentication (lightdm_greeter_get_authentication_user (greeter));
        }
        else
            set_message_label (_("Failed to authenticate"));
    }
}

void suspend_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
suspend_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_suspend (NULL);
}

void hibernate_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
hibernate_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_hibernate (NULL);
}

void restart_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
restart_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_restart (NULL);
}

void shutdown_cb (GtkWidget *widget, LightDMGreeter *greeter);
G_MODULE_EXPORT
void
shutdown_cb (GtkWidget *widget, LightDMGreeter *greeter)
{
    lightdm_shutdown (NULL);
}

static void
user_added_cb (LightDMUserList *user_list, LightDMUser *user, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean logged_in = FALSE;

    model = gtk_combo_box_get_model (user_combo);

    logged_in = lightdm_user_get_logged_in (user);

    gtk_list_store_append (GTK_LIST_STORE (model), &iter);
    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, lightdm_user_get_name (user),
                        1, lightdm_user_get_display_name (user),
                        2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                        -1);
}

static gboolean
get_user_iter (const gchar *username, GtkTreeIter *iter)
{
    GtkTreeModel *model;

    model = gtk_combo_box_get_model (user_combo);

    if (!gtk_tree_model_get_iter_first (model, iter))
        return FALSE;
    do
    {
        gchar *name;
        gboolean matched;

        gtk_tree_model_get (model, iter, 0, &name, -1);
        matched = g_strcmp0 (name, username) == 0;
        g_free (name);
        if (matched)
            return TRUE;
    } while (gtk_tree_model_iter_next (model, iter));

    return FALSE;
}

static void
user_changed_cb (LightDMUserList *user_list, LightDMUser *user, LightDMGreeter *greeter)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean logged_in = FALSE;

    if (!get_user_iter (lightdm_user_get_name (user), &iter))
        return;
    logged_in = lightdm_user_get_logged_in (user);

    model = gtk_combo_box_get_model (user_combo);

    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, lightdm_user_get_name (user),
                        1, lightdm_user_get_display_name (user),
                        2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                        -1);
}

static void
user_removed_cb (LightDMUserList *user_list, LightDMUser *user)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!get_user_iter (lightdm_user_get_name (user), &iter))
        return;

    model = gtk_combo_box_get_model (user_combo);
    gtk_list_store_remove (GTK_LIST_STORE (model), &iter);
}

void a11y_font_cb (GtkCheckMenuItem *item);
G_MODULE_EXPORT
void
a11y_font_cb (GtkCheckMenuItem *item)
{
}

void a11y_contrast_cb (GtkCheckMenuItem *item);
G_MODULE_EXPORT
void
a11y_contrast_cb (GtkCheckMenuItem *item)
{
}

void a11y_keyboard_cb (GtkCheckMenuItem *item);
G_MODULE_EXPORT
void
a11y_keyboard_cb (GtkCheckMenuItem *item)
{
}

static void
load_user_list (void)
{
    const GList *items, *item;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *last_user;
    const gchar *selected_user;
    gboolean logged_in = FALSE;

    g_signal_connect (lightdm_user_list_get_instance (), "user-added", G_CALLBACK (user_added_cb), greeter);
    g_signal_connect (lightdm_user_list_get_instance (), "user-changed", G_CALLBACK (user_changed_cb), greeter);
    g_signal_connect (lightdm_user_list_get_instance (), "user-removed", G_CALLBACK (user_removed_cb), NULL);
    model = gtk_combo_box_get_model (user_combo);
    items = lightdm_user_list_get_users (lightdm_user_list_get_instance ());
    for (item = items; item; item = item->next)
    {
        LightDMUser *user = item->data;
        logged_in = lightdm_user_get_logged_in (user);

        gtk_list_store_append (GTK_LIST_STORE (model), &iter);
        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                            0, lightdm_user_get_name (user),
                            1, lightdm_user_get_display_name (user),
                            2, logged_in ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                            -1);
    }
    if (lightdm_greeter_get_has_guest_account_hint (greeter))
    {
        gtk_list_store_append (GTK_LIST_STORE (model), &iter);
        gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                            0, "*guest",
                            1, _("Guest Session"),
                            2, PANGO_WEIGHT_NORMAL,
                            -1);
    }

    gtk_list_store_append (GTK_LIST_STORE (model), &iter);
    gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                        0, "*other",
                        1, _("Other..."),
                        2, PANGO_WEIGHT_NORMAL,
                        -1);

    last_user = g_key_file_get_value (state, "greeter", "last-user", NULL);

    if (lightdm_greeter_get_select_user_hint (greeter))
        selected_user = lightdm_greeter_get_select_user_hint (greeter);
    else if (lightdm_greeter_get_select_guest_hint (greeter))
        selected_user = "*guest";
    else if (last_user)
        selected_user = last_user;
    else
        selected_user = NULL;

    if (gtk_tree_model_get_iter_first (model, &iter))
    {
        gchar *name;
        gboolean matched = FALSE;
        
        if (selected_user)
        {
            do
            {
                gtk_tree_model_get (model, &iter, 0, &name, -1);
                matched = g_strcmp0 (name, selected_user) == 0;
                g_free (name);
                if (matched)
                {
                    gtk_combo_box_set_active_iter (user_combo, &iter);
                    name = g_strdup(selected_user);
                    set_displayed_user(greeter, name);
                    g_free(name);
                    break;
                }
            } while (gtk_tree_model_iter_next (model, &iter));
        }
        if (!matched)
        {
            gtk_tree_model_get_iter_first (model, &iter);
            gtk_tree_model_get (model, &iter, 0, &name, -1);
            gtk_combo_box_set_active_iter (user_combo, &iter);
            set_displayed_user(greeter, name);
            g_free(name);
        }
        
    }

    g_free (last_user);
}

static gboolean
read_position_from_str (const gchar *s, DimensionPosition *x)
{
    DimensionPosition p;
    gchar *end = NULL;
    gchar **parts = g_strsplit(s, ",", 2);
    if (parts[0])
    {
        p.value = g_ascii_strtoll(parts[0], &end, 10);
        p.percentage = end && end[0] == '%';
        p.sign = (p.value < 0 || (p.value == 0 && parts[0][0] == '-')) ? -1 : +1;
        if (p.value < 0)
            p.value *= -1;
        if (g_strcmp0(parts[1], "start") == 0)
            p.anchor = -1;
        else if (g_strcmp0(parts[1], "center") == 0)
            p.anchor = 0;
        else if (g_strcmp0(parts[1], "end") == 0)
            p.anchor = +1;
        else
            p.anchor = p.sign > 0 ? -1 : +1;
        *x = p;
    }
    else
        x = NULL;
    g_strfreev (parts);
    return x != NULL;
}

static GdkFilterReturn
focus_upon_map (GdkXEvent *gxevent, GdkEvent *event, gpointer  data)
{
    XEvent* xevent = (XEvent*)gxevent;
    GdkWindow* keyboard_win = onboard_window ? gtk_widget_get_window (GTK_WIDGET (onboard_window)) : NULL;
    if (xevent->type == MapNotify)
    {
        Window xwin = xevent->xmap.window;
        Window keyboard_xid = 0;
        GdkDisplay* display = gdk_x11_lookup_xdisplay (xevent->xmap.display);
        GdkWindow* win = gdk_x11_window_foreign_new_for_display (display, xwin);
        GdkWindowTypeHint win_type = gdk_window_get_type_hint (win);

        /* Check to see if this window is our onboard window, since we don't want to focus it. */
        if (keyboard_win)
                keyboard_xid = gdk_x11_window_get_xid (keyboard_win);

        if (xwin != keyboard_xid
            && win_type != GDK_WINDOW_TYPE_HINT_TOOLTIP
            && win_type != GDK_WINDOW_TYPE_HINT_NOTIFICATION)
        {
            gdk_window_focus (win, GDK_CURRENT_TIME);
            /* Make sure to keep keyboard above */
            if (onboard_window)
            {
                if (keyboard_win)
                    gdk_window_raise (keyboard_win);
            }
        }
    }
    else if (xevent->type == UnmapNotify)
    {
        Window xwin;
        int revert_to;
        XGetInputFocus (xevent->xunmap.display, &xwin, &revert_to);

        if (revert_to == RevertToNone)
        {
            gdk_window_focus (gtk_widget_get_window (GTK_WIDGET (login_window)), GDK_CURRENT_TIME);
            /* Make sure to keep keyboard above */
            if (onboard_window)
            {
                if (keyboard_win)
                    gdk_window_raise (keyboard_win);
            }
        }
    }
    return GDK_FILTER_CONTINUE;
}

static void read_config (void)
{
    GKeyFile *config;
    GError *error = NULL;
    GdkPixbuf *background_pixbuf;
    GdkRGBA background_color;
    gchar *value, *x, *y;

    config = g_key_file_new ();
    g_key_file_load_from_file (config, CONFIG_FILE, G_KEY_FILE_NONE, &error);
    if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load configuration from %s: %s\n", CONFIG_FILE, error->message);
    g_clear_error (&error);

    /* Get background colour */
    gdk_rgba_parse (&background_color, "#C0C0C0");
    value = g_key_file_get_value (config, "greeter", "desktop_bg", NULL);
    if (value)
    {
        gdk_rgba_parse (&background_color, value);
        g_free (value);
    }
    default_background_color = gdk_rgba_copy (&background_color);

    /* Get background image */
    value = g_key_file_get_value (config, "greeter", "wallpaper", NULL);
    if (value)
    {
        g_debug ("Loading background %s", value);
        background_pixbuf = gdk_pixbuf_new_from_file (value, NULL);
        if (background_pixbuf)
        {
            /* Need to add an alpha channel here to make redraw work properly */
            default_background_pixbuf = gdk_pixbuf_add_alpha (background_pixbuf, FALSE, 0, 0, 0);
            g_object_unref (background_pixbuf);
        }
        g_free (value);
    }

    /* Get display mode */
    value = g_key_file_get_value (config, "greeter", "wallpaper_mode", NULL);
    if (value)
    {
        g_debug ("Using wallpaper mode %s", value);
        wp_mode = g_strdup (value);
        g_free (value);
    }
    else wp_mode = g_strdup ("color");

    /* Set GTK+ settings */
    value = g_key_file_get_value (config, "greeter", "gtk-theme-name", NULL);
    if (value)
    {
        g_debug ("Using Gtk+ theme %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-theme-name", value, NULL);
        g_free (value);
    }

    value = g_key_file_get_value (config, "greeter", "gtk-icon-theme-name", NULL);
    if (value)
    {
        g_debug ("Using icon theme %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-icon-theme-name", value, NULL);
        g_free (value);
    }

    value = g_key_file_get_value (config, "greeter", "gtk-font-name", NULL);
    if (value)
    {
        g_debug ("Using font %s", value);
        g_object_set (gtk_settings_get_default (), "gtk-font-name", value, NULL);
        g_free (value);
    }
    else g_object_set (gtk_settings_get_default (), "gtk-font-name", "Sans 10", NULL);

    screensaver_timeout = 60;
    value = g_key_file_get_value (config, "greeter", "screensaver-timeout", NULL);
    if (value)
    {
        g_debug ("Screensaver timeout %s", value);
        screensaver_timeout = g_ascii_strtoll (value, NULL, 0);
        g_free (value);
    }

    value = g_key_file_get_value (config, "greeter", "default-user-image", NULL);
    if (value)
    {
        g_debug ("Default user image %s", value);
        if (value[0] == '#')
            default_user_icon = g_strdup (value + 1);
        else
            default_user_pixbuf = gdk_pixbuf_new_from_file_at_scale (value, -1, 80, TRUE, &error);
        g_free (value);
    }

    /* Window position - default: x-center, y-center */
    main_window_pos = CENTERED_WINDOW_POS;
    value = g_key_file_get_value (config, "greeter", "position", NULL);
    if (value)
    {
        g_debug ("Window position %s", value);
        x = value;
        y = strchr (value, ' ');
        if (y) *y++ = 0;

        /* If there is no y-part then y = x */
        if (read_position_from_str (x, &main_window_pos.x))
            if (!y || !read_position_from_str (y, &main_window_pos.y))
                main_window_pos.y = main_window_pos.x;

        g_free (value);
    }

    init_indicators (config);

    g_key_file_free (config);
}

static void draw_windows (void)
{
    GdkDisplay *display = gdk_display_get_default ();
    GdkRectangle monitor_geometry;
    GtkCellRenderer *renderer;
    GtkBuilder *builder;
    gint monitor;
    GtkAllocation allocation;

    builder = gtk_builder_new ();
	gtk_builder_add_from_file (builder, GREETER_DATA_DIR "/pi-greeter.glade", NULL);

    /* Login window */
    login_window = GTK_WINDOW (gtk_builder_get_object (builder, "login_window"));
    user_image = GTK_IMAGE (gtk_builder_get_object (builder, "user_image"));
    user_combo = GTK_COMBO_BOX (gtk_builder_get_object (builder, "user_combobox"));
    username_entry = GTK_ENTRY (gtk_builder_get_object (builder, "username_entry"));
    password_entry = GTK_ENTRY (gtk_builder_get_object (builder, "password_entry"));
    info_bar = GTK_INFO_BAR (gtk_builder_get_object (builder, "greeter_infobar"));
    message_label = GTK_LABEL (gtk_builder_get_object (builder, "message_label"));
    cancel_button = GTK_BUTTON (gtk_builder_get_object (builder, "cancel_button"));
    login_button = GTK_BUTTON (gtk_builder_get_object (builder, "login_button"));

    /* Users combobox */
    renderer = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (user_combo), renderer, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (user_combo), renderer, "text", 1);

    if (lightdm_greeter_get_hide_users_hint (greeter))
    {
        start_authentication ("*other");
    }
    else
    {
        load_user_list ();
        gtk_widget_show (GTK_WIDGET (cancel_button));
        gtk_widget_show (GTK_WIDGET (user_combo));
    }

    gtk_builder_connect_signals (builder, greeter);

    if (!wayland)
    {
        gtk_widget_show (GTK_WIDGET (login_window));
        gdk_monitor_get_geometry (gdk_display_get_primary_monitor (display), &monitor_geometry);
        gtk_widget_get_allocation (GTK_WIDGET (login_window), &allocation);
        gtk_window_move (login_window,
            monitor_geometry.x + get_absolute_position (&(main_window_pos.x), monitor_geometry.width, allocation.width),
            monitor_geometry.y + get_absolute_position (&(main_window_pos.y), monitor_geometry.height, allocation.height));
    }
    else
    {
        gtk_layer_init_for_window (GTK_WINDOW (login_window));
        gtk_layer_set_layer (GTK_WINDOW (login_window), GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_monitor (GTK_WINDOW (login_window), gdk_display_get_primary_monitor (display));
        gtk_layer_set_keyboard_interactivity (GTK_WINDOW (login_window), TRUE);
        gtk_window_set_decorated (login_window, TRUE);
        gtk_widget_show (GTK_WIDGET (login_window));
    }

    gtk_widget_hide (GTK_WIDGET (info_bar));

    /* Background windows */
    for (monitor = 0; monitor < gdk_display_get_n_monitors (display); monitor++)
    {
        if (monitor >= MAX_SCREENS) break;
        gdk_monitor_get_geometry (gdk_display_get_monitor (display, monitor), &monitor_geometry);

        bg_window[monitor] = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
        gtk_window_set_type_hint (bg_window[monitor], GDK_WINDOW_TYPE_HINT_DESKTOP);
        gtk_window_set_keep_below (bg_window[monitor], TRUE);
        gtk_window_set_resizable (bg_window[monitor], FALSE);
        gtk_widget_set_app_paintable (GTK_WIDGET (bg_window[monitor]), TRUE);
        if (!wayland)
        {
            gtk_widget_set_size_request (GTK_WIDGET (bg_window[monitor]), monitor_geometry.width, monitor_geometry.height);
            gtk_window_move (bg_window[monitor], monitor_geometry.x, monitor_geometry.y);
        }
        else
        {
            gtk_window_set_default_size (bg_window[monitor], monitor_geometry.width, monitor_geometry.height);
            gtk_layer_init_for_window (bg_window[monitor]);
            gtk_layer_set_layer (bg_window[monitor], GTK_LAYER_SHELL_LAYER_BACKGROUND);
            gtk_layer_set_monitor (bg_window[monitor], gdk_display_get_monitor (display, monitor));
            gtk_layer_set_anchor (bg_window[monitor], GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor (bg_window[monitor], GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
            gtk_layer_set_anchor (bg_window[monitor], GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
            gtk_layer_set_anchor (bg_window[monitor], GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        }

        gtk_widget_show (GTK_WIDGET (bg_window[monitor]));
        gtk_window_set_decorated (bg_window[monitor], FALSE);
        g_signal_connect (G_OBJECT (bg_window[monitor]), "draw", G_CALLBACK (background_window_draw), NULL);
        gtk_widget_queue_draw (GTK_WIDGET (bg_window[monitor]));
    }

    if (!wayland)
	{
		gdk_window_focus (gtk_widget_get_window (GTK_WIDGET (login_window)), GDK_CURRENT_TIME);

		/* focus fix (source: unity-greeter) */
		GdkWindow* root_window = gdk_get_default_root_window ();
		gdk_window_set_events (root_window, gdk_window_get_events (root_window) | GDK_SUBSTRUCTURE_MASK);
		gdk_window_add_filter (root_window, focus_upon_map, NULL);
	}

    g_object_unref (builder);
}

static void on_mon_add (GdkDisplay *display, GdkMonitor *, gpointer)
{
    gint monitor;

    gtk_widget_destroy (GTK_WIDGET (login_window));
    for (monitor = 0; monitor < gdk_display_get_n_monitors (display); monitor++)
    {
        if (monitor >= MAX_SCREENS) break;
        gtk_widget_destroy (GTK_WIDGET (bg_window[monitor]));
    }
    draw_windows ();
}

int
main (int argc, char **argv)
{
    gchar *state_dir;
    GError *error = NULL;

    if (getenv ("WAYLAND_DISPLAY")) wayland = TRUE;

    /* Prevent memory from being swapped out, as we are dealing with passwords */
    mlockall (MCL_CURRENT | MCL_FUTURE);

    /* Disable global menus */
    g_unsetenv ("UBUNTU_MENUPROXY");

    /* Initialize i18n */
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_unix_signal_add (SIGTERM, (GSourceFunc) gtk_main_quit, NULL);

    /* init gtk */
    gtk_init (&argc, &argv);
    
    state_dir = g_build_filename (g_get_user_cache_dir (), "pi-greeter", NULL);
    g_mkdir_with_parents (state_dir, 0775);
    state_filename = g_build_filename (state_dir, "state", NULL);
    g_free (state_dir);

    state = g_key_file_new ();
    g_key_file_load_from_file (state, state_filename, G_KEY_FILE_NONE, &error);
    if (error && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load state from %s: %s\n", state_filename, error->message);
    g_clear_error (&error);

    greeter = lightdm_greeter_new ();
    g_signal_connect (greeter, "show-prompt", G_CALLBACK (show_prompt_cb), NULL);  
    g_signal_connect (greeter, "show-message", G_CALLBACK (show_message_cb), NULL);
    g_signal_connect (greeter, "authentication-complete", G_CALLBACK (authentication_complete_cb), NULL);
    g_signal_connect (greeter, "autologin-timer-expired", G_CALLBACK (lightdm_greeter_authenticate_autologin), NULL);
    if (!lightdm_greeter_connect_sync (greeter, NULL)) return EXIT_FAILURE;

    /* Set default cursor */
    gdk_window_set_cursor (gdk_get_default_root_window (), gdk_cursor_new_for_display (gdk_display_get_default (), GDK_LEFT_PTR));

    read_config ();

    /* Make the greeter behave a bit more like a screensaver if used as un/lock-screen by blanking the screen */
    Display *display = gdk_x11_display_get_xdisplay (gdk_display_get_default ());
    if (!wayland && lightdm_greeter_get_lock_hint (greeter))
    {
        XGetScreenSaver (display, &timeout, &interval, &prefer_blanking, &allow_exposures);
        XForceScreenSaver (display, ScreenSaverActive);
        XSetScreenSaver (display, screensaver_timeout, 0, ScreenSaverActive, DefaultExposures);
    }

    draw_windows ();

    g_signal_connect (gdk_display_get_default (), "monitor-added", G_CALLBACK (on_mon_add), NULL);

    gtk_main ();

    if (default_background_pixbuf) g_object_unref (default_background_pixbuf);
    if (default_background_color) gdk_rgba_free (default_background_color);
    if (default_user_pixbuf) g_object_unref (default_user_pixbuf);

    if (!wayland)
    {
        int screen = XDefaultScreen (display);
        Window w = RootWindow (display, screen);
        Atom id = XInternAtom (display, "AT_SPI_BUS", True);
        if (id != None)
	    {
            XDeleteProperty (display, w, id);
            XSync (display, FALSE);
	    }
    }

    return EXIT_SUCCESS;
}
