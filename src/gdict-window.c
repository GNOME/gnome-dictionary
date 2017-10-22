/* gdict-window.c - main application window
 *
 * This file is part of GNOME Dictionary
 *
 * Copyright (C) 2005 Emmanuele Bassi
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "gdict-about.h"
#include "gdict-common.h"
#include "gdict-database-chooser-button.h"
#include "gdict-print.h"
#include "gdict-pref-dialog.h"
#include "gdict-sidebar.h"
#include "gdict-window.h"

#define GDICT_WINDOW_COLUMNS      56
#define GDICT_WINDOW_ROWS         33

#define GDICT_WINDOW_MIN_WIDTH	  400
#define GDICT_WINDOW_MIN_HEIGHT	  330

/* sidebar pages logical ids */
#define GDICT_SIDEBAR_SPELLER_PAGE      "speller"
#define GDICT_SIDEBAR_STRATEGIES_PAGE   "strat-chooser"
#define GDICT_SIDEBAR_SOURCES_PAGE      "source-chooser"

enum
{
  COMPLETION_TEXT_COLUMN,

  COMPLETION_N_COLUMNS
};

enum
{
  PROP_0,

  PROP_ACTION,
  PROP_SOURCE_LOADER,
  PROP_SOURCE_NAME,
  PROP_DATABASE,
  PROP_STRATEGY,
  PROP_PRINT_FONT,
  PROP_DEFBOX_FONT,
  PROP_WORD,
  PROP_WINDOW_ID,

  LAST_PROP
};

enum
{
  CREATED,
  
  LAST_SIGNAL
};

static GParamSpec *gdict_window_properties[LAST_PROP] = { NULL, };
static guint gdict_window_signals[LAST_SIGNAL] = { 0 };

static const GtkTargetEntry drop_types[] =
{
  { "text/plain",    0, 0 },
  { "TEXT",          0, 0 },
  { "STRING",        0, 0 },
  { "UTF8_STRING",   0, 0 },
};
static const guint n_drop_types = G_N_ELEMENTS (drop_types);



G_DEFINE_TYPE (GdictWindow, gdict_window, GTK_TYPE_APPLICATION_WINDOW);


static void
gdict_window_finalize (GObject *gobject)
{
  GdictWindow *window = GDICT_WINDOW (gobject);

  g_free (window->source_name);
  g_free (window->print_font);
  g_free (window->defbox_font);
  g_free (window->word);
  g_free (window->database);
  g_free (window->strategy);
  
  G_OBJECT_CLASS (gdict_window_parent_class)->finalize (gobject);
}

static void
gdict_window_dispose (GObject *gobject)
{
  GdictWindow *window = GDICT_WINDOW (gobject);

  g_clear_object (&window->desktop_settings);
  g_clear_object (&window->settings);

  if (window->context)
    {
      if (window->lookup_start_id)
        {
          g_signal_handler_disconnect (window->context, window->lookup_start_id);
          g_signal_handler_disconnect (window->context, window->definition_id);
          g_signal_handler_disconnect (window->context, window->lookup_end_id);
          g_signal_handler_disconnect (window->context, window->error_id);

          window->lookup_start_id = 0;
          window->definition_id = 0;
          window->lookup_end_id = 0;
          window->error_id = 0;
        }

      g_object_unref (window->context);
      window->context = NULL;
    }

  g_clear_object (&window->loader);
  g_clear_object (&window->completion);
  g_clear_object (&window->completion_model);
  g_clear_object (&window->busy_cursor);

  G_OBJECT_CLASS (gdict_window_parent_class)->dispose (gobject);
}

static const gchar *toggle_actions[] = {
  "save-as",
  "preview",
  "print",
  "previous-def",
  "next-def",
  "first-def",
  "last-def",
  "find",
};

static gint n_toggle_state = G_N_ELEMENTS (toggle_actions);

static void
gdict_window_ensure_menu_state (GdictWindow *window)
{
  gint i;
  gboolean is_enabled;

  g_assert (GDICT_IS_WINDOW (window));

  is_enabled = !!(window->word != NULL);
  for (i = 0; i < n_toggle_state; i++)
    {
      GAction *action;

      action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                           toggle_actions[i]);
      if (!action)
        continue;

      g_simple_action_set_enabled (G_SIMPLE_ACTION (action), is_enabled);
    }
}

static void
gdict_window_set_sidebar_visible (GdictWindow *window,
				  gboolean     is_visible)
{
  g_assert (GDICT_IS_WINDOW (window));

  is_visible = !!is_visible;

  if (is_visible != window->sidebar_visible)
    {
      GAction *action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                    "view-sidebar");
      g_action_change_state (action, g_variant_new_boolean (is_visible));
    }
}

static void
gdict_window_definition_cb (GdictContext    *context,
			    GdictDefinition *definition,
			    GdictWindow     *window)
{
  g_assert (GDICT_IS_WINDOW (window));

  while (gtk_events_pending ())
    gtk_main_iteration ();

  window->current_definition++;
}

static void
gdict_window_lookup_start_cb (GdictContext *context,
			      GdictWindow  *window)
{
  if (!window->word)
    return;

  if (!window->busy_cursor)
    {
      GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (window));
      window->busy_cursor = gdk_cursor_new_for_display (display, GDK_WATCH);
    }

  window->max_definition = -1;
  window->last_definition = 0;
  window->current_definition = 0;

  gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (window)), window->busy_cursor);

  gtk_spinner_start (GTK_SPINNER (window->spinner));
  gtk_widget_show (window->spinner);
  gtk_stack_set_visible_child_name (GTK_STACK (window->stack), "spinner");
}

static void
gdict_window_lookup_end_cb (GdictContext *context,
			    GdictWindow  *window)
{
  gint count;
  GtkTreeIter iter;
  GdictSource *source;
  GdictContext *speller_context;
  
  count = window->current_definition;

  window->max_definition = count - 1;

  /* we clone the context, so that the signals that it
   * fires do not get caught by the signal handlers we
   * use for getting the definitions.
   */
  source = gdict_source_loader_get_source (window->loader, window->source_name);
  speller_context = gdict_source_get_context (source);
  gdict_speller_set_context (GDICT_SPELLER (window->speller), speller_context);
  g_object_unref (speller_context);
  g_object_unref (source);

  /* search for similar words; if we have a no-match we already started
   * looking in the error signal handler
   */
  if (count != 0 && window->word)
    {
      gdict_speller_set_strategy (GDICT_SPELLER (window->speller), window->strategy);
      gdict_speller_match (GDICT_SPELLER (window->speller), window->word);
      gtk_list_store_append (window->completion_model, &iter);
      gtk_list_store_set (window->completion_model, &iter,
                          COMPLETION_TEXT_COLUMN, window->word,
                          -1);
    }

  gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (window)), NULL);

  gtk_stack_set_visible_child_name (GTK_STACK (window->stack), "main");
  gtk_spinner_stop (GTK_SPINNER (window->spinner));
  gtk_widget_hide (window->spinner);

  if (count == 0)
    {
      g_free (window->word);
      window->word = NULL;
    }

  gdict_window_ensure_menu_state (window);
}

static void
gdict_window_error_cb (GdictContext *context,
		       const GError *error,
		       GdictWindow  *window)
{
  gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (window)), NULL);

  gtk_stack_set_visible_child_name (GTK_STACK (window->stack), "main");
  gtk_spinner_stop (GTK_SPINNER (window->spinner));
  gtk_widget_hide (window->spinner);
  
  /* launch the speller only on NO_MATCH */
  if (error->code == GDICT_CONTEXT_ERROR_NO_MATCH)
    {
      GdictSource *source;
      GdictContext *context;

      gdict_window_set_sidebar_visible (window, TRUE);
      gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar),
                               GDICT_SIDEBAR_SPELLER_PAGE);

      /* we clone the context, so that the signals that it
       * fires do not get caught by the signal handlers we
       * use for getting the definitions.
       */
      source = gdict_source_loader_get_source (window->loader,
		      			       window->source_name);
      context = gdict_source_get_context (source);

      gdict_speller_set_context (GDICT_SPELLER (window->speller),
		      		 context);
      g_object_unref (context);
      g_object_unref (source);
      
      gdict_speller_set_strategy (GDICT_SPELLER (window->speller),
			          window->strategy);
      
      gdict_speller_match (GDICT_SPELLER (window->speller),
		           window->word);
    }

  /* unset the word and update the UI */
  g_free (window->word);
  window->word = NULL;

  gdict_window_ensure_menu_state (window);
}

static void
gdict_window_set_database (GdictWindow *window,
			   const gchar *database)
{
  if (g_strcmp0 (window->database, database) == 0)
    return;

  g_free (window->database);

  if (database != NULL && *database != '\0')
    window->database = g_strdup (database);
  else
    window->database = g_settings_get_string (window->settings, GDICT_SETTINGS_DATABASE_KEY);

  if (window->defbox)
    gdict_defbox_set_database (GDICT_DEFBOX (window->defbox),
			       window->database);

  if (window->db_chooser)
    gdict_database_chooser_button_set_current_database (GDICT_DATABASE_CHOOSER_BUTTON (window->db_chooser),
							window->database);

  g_object_notify_by_pspec (G_OBJECT (window), gdict_window_properties[PROP_DATABASE]);
}

static void
gdict_window_set_strategy (GdictWindow *window,
			   const gchar *strategy)
{
  if (g_strcmp0 (window->strategy, strategy) == 0)
    return;

  g_free (window->strategy);

  if (strategy != NULL && *strategy != '\0')
    window->strategy = g_strdup (strategy);
  else
    window->strategy = g_settings_get_string (window->settings, GDICT_SETTINGS_STRATEGY_KEY);

  if (window->speller)
    gdict_speller_set_strategy (GDICT_SPELLER (window->speller),
                                window->strategy);

  if (window->strat_chooser)
    gdict_strategy_chooser_set_current_strategy (GDICT_STRATEGY_CHOOSER (window->strat_chooser),
                                                 window->strategy);

  g_object_notify_by_pspec (G_OBJECT (window), gdict_window_properties[PROP_STRATEGY]);
}

static GdictContext *
get_context_from_loader (GdictWindow *window)
{
  GdictSource *source;
  GdictContext *retval;

  if (!window->source_name)
    window->source_name = g_strdup (GDICT_DEFAULT_SOURCE_NAME);

  source = gdict_source_loader_get_source (window->loader,
		  			   window->source_name);
  if (!source &&
      strcmp (window->source_name, GDICT_DEFAULT_SOURCE_NAME) != 0)
    {
      g_free (window->source_name);
      window->source_name = g_strdup (GDICT_DEFAULT_SOURCE_NAME);

      source = gdict_source_loader_get_source (window->loader,
                                               window->source_name);
    }
  
  if (!source)
    {
      gchar *detail;
      
      detail = g_strdup_printf (_("No dictionary source available with name “%s”"),
      				window->source_name);

      gdict_show_error_dialog (GTK_WINDOW (window),
                               _("Unable to find dictionary source"),
                               detail);
      
      g_free (detail);

      return NULL;
    }
  
  gdict_window_set_database (window, gdict_source_get_database (source));
  gdict_window_set_strategy (window, gdict_source_get_strategy (source));
  
  retval = gdict_source_get_context (source);
  if (!retval)
    {
      gchar *detail;
      
      detail = g_strdup_printf (_("No context available for source “%s”"),
      				gdict_source_get_description (source));
      				
      gdict_show_error_dialog (GTK_WINDOW (window),
                               _("Unable to create a context"),
                               detail);
      
      g_free (detail);
      g_object_unref (source);
      
      return NULL;
    }
  
  g_object_unref (source);
  
  return retval;
}

static void
gdict_window_set_defbox_font (GdictWindow *window,
			      const gchar *defbox_font)
{
  g_free (window->defbox_font);

  if (defbox_font != NULL && *defbox_font != '\0')
    window->defbox_font = g_strdup (defbox_font);
  else
    window->defbox_font = g_settings_get_string (window->desktop_settings, DOCUMENT_FONT_KEY);

  gdict_defbox_set_font_name (GDICT_DEFBOX (window->defbox), window->defbox_font);
}

static void
gdict_window_set_print_font (GdictWindow *window,
			     const gchar *print_font)
{
  g_free (window->print_font);

  if (print_font != NULL && *print_font != '\0')
    window->print_font = g_strdup (print_font);
  else
    window->print_font = g_settings_get_string (window->settings, GDICT_SETTINGS_PRINT_FONT_KEY);
}

static void
gdict_window_set_word (GdictWindow *window,
		       const gchar *word,
		       const gchar *database)
{
  gchar *title;
  
  g_free (window->word);
  window->word = NULL;

  if (word && word[0] != '\0')
    window->word = g_strdup (word);
  else
    return;

  if (!database || database[0] == '\0')
    database = window->database;

  if (window->word)
    title = g_strdup_printf (_("%s — Dictionary"), window->word);
  else
    title = g_strdup (_("Dictionary"));
  
  gtk_window_set_title (GTK_WINDOW (window), title);
  g_free (title);

  if (window->defbox)
    {
      gdict_defbox_set_database (GDICT_DEFBOX (window->defbox), database);
      gdict_defbox_lookup (GDICT_DEFBOX (window->defbox), word);
    }
}

static void
gdict_window_set_context (GdictWindow  *window,
			  GdictContext *context)
{
  if (window->context)
    {
      g_signal_handler_disconnect (window->context, window->definition_id);
      g_signal_handler_disconnect (window->context, window->lookup_start_id);
      g_signal_handler_disconnect (window->context, window->lookup_end_id);
      g_signal_handler_disconnect (window->context, window->error_id);
      
      window->definition_id = 0;
      window->lookup_start_id = 0;
      window->lookup_end_id = 0;
      window->error_id = 0;
      
      g_object_unref (window->context);
      window->context = NULL;
    }

  if (window->defbox)
    gdict_defbox_set_context (GDICT_DEFBOX (window->defbox), context);

  if (window->db_chooser)
    gdict_database_chooser_button_set_context (GDICT_DATABASE_CHOOSER_BUTTON (window->db_chooser),
					       context);

  if (window->strat_chooser)
    gdict_strategy_chooser_set_context (GDICT_STRATEGY_CHOOSER (window->strat_chooser), context);

  if (!context)
    return;
  
  /* attach our callbacks */
  window->definition_id   = g_signal_connect (context, "definition-found",
		  			      G_CALLBACK (gdict_window_definition_cb),
					      window);
  window->lookup_start_id = g_signal_connect (context, "definition-lookup-start",
		  			      G_CALLBACK (gdict_window_lookup_start_cb),
					      window);
  window->lookup_end_id   = g_signal_connect (context, "definition-lookup-end",
		  			      G_CALLBACK (gdict_window_lookup_end_cb),
					      window);
  window->error_id        = g_signal_connect (context, "error",
		  			      G_CALLBACK (gdict_window_error_cb),
					      window);
  
  window->context = context;
}

static void
gdict_window_set_source_name (GdictWindow *window,
			      const gchar *source_name)
{
  GdictContext *context;

  if (window->source_name && source_name &&
      strcmp (window->source_name, source_name) == 0)
    return;

  g_free (window->source_name);

  if (source_name != NULL && *source_name != '\0')
    window->source_name = g_strdup (source_name);
  else
    window->source_name = g_settings_get_string (window->settings, GDICT_SETTINGS_SOURCE_KEY);

  context = get_context_from_loader (window);
  gdict_window_set_context (window, context);

  if (window->source_chooser)
    gdict_source_chooser_set_current_source (GDICT_SOURCE_CHOOSER (window->source_chooser),
                                             window->source_name);

  g_object_notify_by_pspec (G_OBJECT (window), gdict_window_properties[PROP_SOURCE_NAME]);
}

static void
gdict_window_set_property (GObject      *object,
			   guint         prop_id,
			   const GValue *value,
			   GParamSpec   *pspec)
{
  GdictWindow *window = GDICT_WINDOW (object);
  
  switch (prop_id)
    {
    case PROP_ACTION:
      window->action = g_value_get_enum (value);
      break;
    case PROP_SOURCE_LOADER:
      if (window->loader)
        g_object_unref (window->loader);
      window->loader = g_value_get_object (value);
      g_object_ref (window->loader);
      break;
    case PROP_SOURCE_NAME:
      gdict_window_set_source_name (window, g_value_get_string (value));
      break;
    case PROP_DATABASE:
      gdict_window_set_database (window, g_value_get_string (value));
      break;
    case PROP_STRATEGY:
      gdict_window_set_strategy (window, g_value_get_string (value));
      break;
    case PROP_WORD:
      gdict_window_set_word (window, g_value_get_string (value), NULL);
      break;
    case PROP_PRINT_FONT:
      gdict_window_set_print_font (window, g_value_get_string (value));
      break;
    case PROP_DEFBOX_FONT:
      gdict_window_set_defbox_font (window, g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdict_window_get_property (GObject    *object,
			   guint       prop_id,
			   GValue     *value,
			   GParamSpec *pspec)
{
  GdictWindow *window = GDICT_WINDOW (object);
  
  switch (prop_id)
    {
    case PROP_ACTION:
      g_value_set_enum (value, window->action);
      break;
    case PROP_SOURCE_LOADER:
      g_value_set_object (value, window->loader);
      break;
    case PROP_SOURCE_NAME:
      g_value_set_string (value, window->source_name);
      break;
    case PROP_DATABASE:
      g_value_set_string (value, window->database);
      break;
    case PROP_STRATEGY:
      g_value_set_string (value, window->strategy);
      break;
    case PROP_WORD:
      g_value_set_string (value, window->word);
      break;
    case PROP_PRINT_FONT:
      g_value_set_string (value, window->print_font);
      break;
    case PROP_DEFBOX_FONT:
      g_value_set_string (value, window->defbox_font);
      break;
    case PROP_WINDOW_ID:
      g_value_set_uint (value, window->window_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdict_window_store_state (GdictWindow *window)
{
  gchar *state_dir, *state_file;
  GKeyFile *state_key;
  gchar *data;
  gsize data_len;
  GError *error;
  const gchar *page_id;

  state_dir = g_build_filename (g_get_user_cache_dir (),
                                "gnome-dictionary-3.0",
                                NULL);

  if (g_mkdir (state_dir, 0700) == -1)
    {
      if (errno != EEXIST)
        {
          g_warning ("Unable to create a cache directory: %s", g_strerror (errno));
          g_free (state_dir);
          return;
        }
    }

  state_file = g_build_filename (state_dir, "window.ini", NULL);
  state_key = g_key_file_new ();

  /* store the default size of the window and its state, so that
   * it's picked up by newly created windows
   */
  g_key_file_set_integer (state_key, "WindowState", "Width", window->current_width);
  g_key_file_set_integer (state_key, "WindowState", "Height", window->current_height);
  g_key_file_set_boolean (state_key, "WindowState", "IsMaximized", window->is_maximized);
  g_key_file_set_boolean (state_key, "WindowState", "SidebarVisible", window->sidebar_visible);
  g_key_file_set_double (state_key, "WindowState", "SidebarWidthPercentage", window->sidebar_width_percentage);

  page_id = gdict_sidebar_current_page (GDICT_SIDEBAR (window->sidebar));
  if (page_id == NULL)
    page_id = GDICT_SIDEBAR_SPELLER_PAGE;

  g_key_file_set_string (state_key, "WindowState", "SidebarPage", page_id);

  error = NULL;
  data = g_key_file_to_data (state_key, &data_len, &error);
  if (error != NULL)
    {
      g_warning ("Unable to create the window state file: %s", error->message);
      g_error_free (error);
    }
  else
    {
      g_file_set_contents (state_file, data, data_len, &error);
      if (error != NULL)
        {
          g_warning ("Unable to write the window state file: %s", error->message);
          g_error_free (error);
        }

      g_free (data);
    }

  g_key_file_free (state_key);
  g_free (state_file);
  g_free (state_dir);
}

static void
gdict_window_load_state (GdictWindow *window)
{
  gchar *state_file;
  GKeyFile *state_key;
  GError *error;

  state_file = g_build_filename (g_get_user_cache_dir (),
                                 "gnome-dictionary-3.0",
                                 "window.ini",
                                 NULL);
  state_key = g_key_file_new ();

  error = NULL;
  g_key_file_load_from_file (state_key, state_file, 0, &error);
  if (error != NULL)
    {
      g_warning ("Unable to load the window state file: %s", error->message);
      g_error_free (error);
      g_key_file_free (state_key);
      g_free (state_file);
      return;
    }

  window->default_width = g_key_file_get_integer (state_key, "WindowState", "Width", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->default_width = -1;
    }

  window->default_height = g_key_file_get_integer (state_key, "WindowState", "Height", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->default_height = -1;
    }

  window->is_maximized = g_key_file_get_boolean (state_key, "WindowState", "IsMaximized", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->is_maximized = FALSE;
    }

  window->sidebar_visible = g_key_file_get_boolean (state_key, "WindowState", "SidebarVisible", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->sidebar_visible = FALSE;
    }

  window->sidebar_width_percentage = g_key_file_get_double (state_key, "WindowState", "SidebarWidthPercentage", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->sidebar_width_percentage = -1;
    }

  window->sidebar_page = g_key_file_get_string (state_key, "WindowState", "SidebarPage", &error);
  if (error != NULL)
    {
      g_clear_error (&error);
      window->sidebar_page = NULL;
    }

  g_key_file_free (state_key);
  g_free (state_file);
}

static void
gdict_window_cmd_save_as (GSimpleAction   *action,
                          GVariant        *parameter,
                          gpointer         user_data)
{
  GdictWindow *window = user_data;
  GtkWidget *dialog;
  
  g_assert (GDICT_IS_WINDOW (window));
  
  dialog = gtk_file_chooser_dialog_new (_("Save a Copy"),
  					GTK_WINDOW (window),
  					GTK_FILE_CHOOSER_ACTION_SAVE,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        _("_Save"), GTK_RESPONSE_ACCEPT,
  					NULL);
  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);
  
  /* default to user's home */
  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_home_dir ());
  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), _("Untitled document"));
  
  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      gchar *filename;
      gchar *text;
      gsize len;
      GError *write_error = NULL;
      
      filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
      
      text = gdict_defbox_get_text (GDICT_DEFBOX (window->defbox), &len);
      
      g_file_set_contents (filename,
      			   text,
      			   len,
      			   &write_error);
      if (write_error)
        {
          gchar *message;
          
          message = g_strdup_printf (_("Error while writing to “%s”"), filename);
          
          gdict_show_gerror_dialog (GTK_WINDOW (window),
                                    message,
                                    write_error);

          g_free (message);
        }
      
      g_free (text);
      g_free (filename);
    }

  gtk_widget_destroy (dialog);
}

static void
gdict_window_cmd_file_preview (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  gdict_show_print_preview (GTK_WINDOW (window),
                            GDICT_DEFBOX (window->defbox));
}

static void
gdict_window_cmd_file_print (GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  gdict_show_print_dialog (GTK_WINDOW (window),
  			   GDICT_DEFBOX (window->defbox));
}

static void
gdict_window_cmd_edit_find (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  gdict_defbox_set_show_find (GDICT_DEFBOX (window->defbox), TRUE);
}

static void
gdict_window_cmd_change_view_sidebar (GSimpleAction *action,
                                      GVariant      *state,
                                      gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  window->sidebar_visible = g_variant_get_boolean (state);

  if (window->sidebar_visible)
    gtk_widget_show (window->sidebar_box);
  else
    gtk_widget_hide (window->sidebar_box);

  g_simple_action_set_state (action, state);
}

static void
gdict_window_cmd_view_speller (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar),
                           GDICT_SIDEBAR_SPELLER_PAGE);
  gdict_window_set_sidebar_visible (window, TRUE);
}

static void
gdict_window_cmd_view_strategies (GSimpleAction *action,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar),
                           GDICT_SIDEBAR_STRATEGIES_PAGE);
  gdict_window_set_sidebar_visible (window, TRUE);
}

static void
gdict_window_cmd_view_sources (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar),
                           GDICT_SIDEBAR_SOURCES_PAGE);
  gdict_window_set_sidebar_visible (window, TRUE);
}

static void
gdict_window_cmd_go_first_def (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  window->last_definition = 0;
  gdict_defbox_jump_to_definition (GDICT_DEFBOX (window->defbox),
                                   window->last_definition);
}

static void
gdict_window_cmd_go_previous_def (GSimpleAction *action,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  if (window->last_definition == 0)
    return;
  
  window->last_definition -= 1;
  gdict_defbox_jump_to_definition (GDICT_DEFBOX (window->defbox),
                                   window->last_definition);
}

static void
gdict_window_cmd_go_next_def (GSimpleAction *action,
                              GVariant      *parameter,
                              gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  if (window->max_definition == -1)
    window->max_definition = gdict_defbox_count_definitions (GDICT_DEFBOX (window->defbox)) - 1;
    
  if (window->last_definition == window->max_definition)
    return;
  
  window->last_definition += 1;
  gdict_defbox_jump_to_definition (GDICT_DEFBOX (window->defbox),
                                   window->last_definition);
}

static void
gdict_window_cmd_go_last_def (GSimpleAction *action,
                              GVariant      *parameter,
                              gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  if (window->max_definition == -1)
    window->last_definition = gdict_defbox_count_definitions (GDICT_DEFBOX (window->defbox)) - 1;
  
  window->last_definition = window->max_definition;
  gdict_defbox_jump_to_definition (GDICT_DEFBOX (window->defbox),
                                   window->last_definition);
}

static void
activate_toggle (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean value = g_variant_get_boolean (state);

  g_action_change_state (G_ACTION (action), g_variant_new_boolean (!value));
  g_variant_unref (state);
}

static void
gdict_window_cmd_lookup (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));

  gtk_widget_grab_focus (window->entry);
}

static void
gdict_window_cmd_escape (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  GdictWindow *window = user_data;

  g_assert (GDICT_IS_WINDOW (window));
  
  gdict_defbox_set_show_find (GDICT_DEFBOX (window->defbox), FALSE);
}

static const GActionEntry entries[] =
{
  /* File menu */
  { "save-as", gdict_window_cmd_save_as, NULL, NULL, NULL },
  { "preview", gdict_window_cmd_file_preview, NULL, NULL, NULL },
  { "print", gdict_window_cmd_file_print, NULL, NULL, NULL },

  /* Find item */
  { "find", gdict_window_cmd_edit_find, NULL, NULL, NULL },

  /* Go menu */
  { "previous-def", gdict_window_cmd_go_previous_def, NULL, NULL, NULL },
  { "next-def", gdict_window_cmd_go_next_def, NULL, NULL, NULL },
  { "first-def", gdict_window_cmd_go_first_def, NULL, NULL, NULL },
  { "last-def", gdict_window_cmd_go_last_def, NULL, NULL, NULL },

  /* View menu */
  { "view-sidebar", activate_toggle, NULL, "false",
    gdict_window_cmd_change_view_sidebar },
  { "view-speller", gdict_window_cmd_view_speller, NULL, NULL, NULL },
  { "view-source", gdict_window_cmd_view_sources, NULL, NULL, NULL },
  { "view-strat", gdict_window_cmd_view_strategies, NULL, NULL, NULL },
  
  /* Accelerators */
  { "lookup", gdict_window_cmd_lookup, NULL, NULL, NULL },
  { "escape", gdict_window_cmd_escape, NULL, NULL, NULL }
};

static gboolean
gdict_window_delete_event_cb (GtkWidget *widget,
			      GdkEvent  *event,
			      gpointer   user_data)
{
  GdictWindow *window = GDICT_WINDOW (widget);

  g_assert (GDICT_IS_WINDOW (window));

  gdict_window_store_state (window);

  return FALSE;
}

static gboolean
gdict_window_state_event_cb (GtkWidget           *widget,
			     GdkEventWindowState *event,
			     gpointer             user_data)
{
  GdictWindow *window = GDICT_WINDOW (widget);

  if (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED)
    window->is_maximized = TRUE;
  else
    window->is_maximized = FALSE;

  gtk_window_get_size (GTK_WINDOW (window),
                       &window->current_width,
                       &window->current_height);

  /* Resize the panel as the window is minimised/maximised */
  gtk_paned_set_position (GTK_PANED (window->main_pane),
                          window->current_width * (1.0 - window->sidebar_width_percentage));

  return FALSE;
}

static gboolean
gdict_window_size_allocate_event_cb (GtkWidget           *widget,
                                     GdkEventWindowState *event,
                                     gpointer             user_data)
{
  GdictWindow *window = GDICT_WINDOW (widget);

  gtk_window_get_size (GTK_WINDOW (window),
                       &window->current_width,
                       &window->current_height);

  /* Resize the panel as the window is resized */
  gtk_paned_set_position (GTK_PANED (window->main_pane),
                          window->current_width * (1.0 - window->sidebar_width_percentage));

  return FALSE;
}

static void
lookup_word (GdictWindow *window,
             gpointer     dummy)
{
  const gchar *entry_text;
  gchar *word;
  
  g_assert (GDICT_IS_WINDOW (window));
  
  if (!window->context)
    return;
  
  entry_text = gtk_entry_get_text (GTK_ENTRY (window->entry));
  if (!entry_text || *entry_text == '\0')
    return;

  word = g_strdup (entry_text);
  gdict_window_set_word (window, g_strstrip (word), NULL);

  g_free (word);
}

static void
source_activated_cb (GdictSourceChooser *chooser,
                     const gchar        *source_name,
                     GdictSource        *source,
                     GdictWindow        *window)
{
  g_signal_handlers_block_by_func (chooser, source_activated_cb, window);
  gdict_window_set_source_name (window, source_name);
  g_signal_handlers_unblock_by_func (chooser, source_activated_cb, window);
}

static void
strategy_activated_cb (GdictStrategyChooser *chooser,
                       const gchar          *strat_name,
                       const gchar          *strat_desc,
                       GdictWindow          *window)
{
  g_signal_handlers_block_by_func (chooser, strategy_activated_cb, window);
  gdict_window_set_strategy (window, strat_name);
  g_signal_handlers_unblock_by_func (chooser, strategy_activated_cb, window);
}

static void
database_activated_cb (GdictDatabaseChooser *chooser,
		       const gchar          *db_name,
		       const gchar          *db_desc,
		       GdictWindow          *window)
{
  g_signal_handlers_block_by_func (chooser, database_activated_cb, window);
  gdict_window_set_database (window, db_name);
  g_signal_handlers_unblock_by_func (chooser, database_activated_cb, window);
}

static void
speller_word_activated_cb (GdictSpeller *speller,
			   const gchar  *word,
			   const gchar  *db_name,
			   GdictWindow  *window)
{
  gtk_entry_set_text (GTK_ENTRY (window->entry), word);
  
  gdict_window_set_word (window, word, db_name);
}

static void
sidebar_page_changed_cb (GdictSidebar *sidebar,
			 GdictWindow  *window)
{
  const gchar *page_id;
  const gchar *message G_GNUC_UNUSED;

  page_id = gdict_sidebar_current_page (sidebar);

  g_free (window->sidebar_page);
  window->sidebar_page = g_strdup (page_id);

  switch (page_id[0])
    {
    case 's':
      {
      switch (page_id[1])
        {
        case 'p': /* speller */
          message = _("Double-click on the word to look up");
          if (window->word)
            gdict_speller_match (GDICT_SPELLER (window->speller),
                                 window->word);
          break;
        case 't': /* strat-chooser */
          message = _("Double-click on the matching strategy to use");
          
          gdict_strategy_chooser_refresh (GDICT_STRATEGY_CHOOSER (window->strat_chooser));
          break;
        case 'o': /* source-chooser */
          message = _("Double-click on the source to use");
          gdict_source_chooser_refresh (GDICT_SOURCE_CHOOSER (window->source_chooser));
          break;
        default:
          message = NULL;
        }
      }
      break;
    default:
      message = NULL;
      break;
    }
}

static void
sidebar_closed_cb (GdictSidebar *sidebar,
		   GdictWindow  *window)
{
  gdict_window_set_sidebar_visible (window, FALSE); 
}

static void
gdict_window_link_clicked (GdictDefbox *defbox,
                           const gchar *link_text,
                           GdictWindow *window)
{
  GtkWidget *new_window;
  GtkApplication *application = gtk_window_get_application (GTK_WINDOW (window));

  gdict_window_store_state (window);

  new_window = gdict_window_new (GDICT_WINDOW_ACTION_LOOKUP,
                                 application,
                                 window->loader,
                                 NULL,
                                 NULL,
                                 NULL,
                                 link_text);
  gtk_widget_show (new_window);
  
  g_signal_emit (window, gdict_window_signals[CREATED], 0, new_window);
}

static void
gdict_window_drag_data_received_cb (GtkWidget        *widget,
				    GdkDragContext   *context,
				    gint              x,
				    gint              y,
				    GtkSelectionData *data,
				    guint             info,
				    guint             time_,
				    gpointer          user_data)
{
  GdictWindow *window = GDICT_WINDOW (user_data);
  gchar *text;
  
  text = (gchar *) gtk_selection_data_get_text (data);
  if (text)
    {
      gtk_entry_set_text (GTK_ENTRY (window->entry), text);

      gdict_window_set_word (window, text, NULL);
      g_free (text);
      
      gtk_drag_finish (context, TRUE, FALSE, time_);
    }
  else
    gtk_drag_finish (context, FALSE, FALSE, time_);
}

static void
gdict_window_size_allocate (GtkWidget     *widget,
			    GtkAllocation *allocation)
{
  if (GTK_WIDGET_CLASS (gdict_window_parent_class)->size_allocate != NULL)
    GTK_WIDGET_CLASS (gdict_window_parent_class)->size_allocate (widget, allocation);
}

static void
gdict_window_handle_notify_position_cb (GtkWidget  *widget,
					GParamSpec *pspec,
					gpointer    user_data)
{
  GdictWindow *window = GDICT_WINDOW (user_data);
  gint pos;
  gdouble new_sidebar_width_percentage;
  gdouble diff_sidebar_width_percentage;

  pos = gtk_paned_get_position (GTK_PANED (window->main_pane));

  new_sidebar_width_percentage = 1.0 - pos / (gdouble) window->current_width;
  diff_sidebar_width_percentage = fabs (window->sidebar_width_percentage - new_sidebar_width_percentage); 
  if (pos < window->current_width && diff_sidebar_width_percentage > 1e-2)
	  window->sidebar_width_percentage = new_sidebar_width_percentage; 

  gtk_paned_set_position (GTK_PANED (window->main_pane),
                          window->current_width * (1.0 - window->sidebar_width_percentage));
}

static void
gdict_window_constructed (GObject *gobject)
{
  GApplication *app;
  GdictWindow *window;
  GtkWidget *button;
  PangoFontDescription *font_desc;
  gchar *font_name;
  GMenu *menu;
  
  window = GDICT_WINDOW (gobject);

  window->in_construction = TRUE;

  /* recover the state */
  gdict_window_load_state (window);

  /* Set a default value for the sidebar width (percentage) */
  if (window->sidebar_width_percentage <= 0)
    window->sidebar_width_percentage = 0.3333;

  /* build menus */
  g_action_map_add_action_entries (G_ACTION_MAP (window),
                                   entries, G_N_ELEMENTS (entries),
                                   window);
  gdict_window_ensure_menu_state (window);

  /* The :application property of GtkWindow is not yet set inside
   * the constructed() vfunc, so we need to use the global singleton
   */
  app = g_application_get_default ();
  menu = gtk_application_get_menu_by_id (GTK_APPLICATION (app), "popup");
  button = gtk_menu_button_new ();
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (button), G_MENU_MODEL (menu));
  gtk_menu_button_set_direction (GTK_MENU_BUTTON (button), GTK_ARROW_NONE);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (window->header_bar), button);
  gtk_widget_show (button);

  window->completion_model = gtk_list_store_new (COMPLETION_N_COLUMNS, G_TYPE_STRING);
  
  window->completion = gtk_entry_completion_new ();
  gtk_entry_completion_set_popup_completion (window->completion, TRUE);
  gtk_entry_completion_set_model (window->completion,
		  		  GTK_TREE_MODEL (window->completion_model));
  gtk_entry_completion_set_text_column (window->completion, COMPLETION_TEXT_COLUMN);
  
  if (window->word)
    gtk_entry_set_text (GTK_ENTRY (window->entry), window->word);
  
  gtk_entry_set_completion (GTK_ENTRY (window->entry), window->completion);
  g_signal_connect_swapped (window->entry, "activate",
                            G_CALLBACK (lookup_word),
                            window);

  /* Defbox */
  if (window->context)
    gdict_defbox_set_context (GDICT_DEFBOX (window->defbox), window->context);

  g_signal_connect (window->defbox, "link-clicked",
                    G_CALLBACK (gdict_window_link_clicked),
                    window);
  gtk_drag_dest_set (window->defbox,
  		     GTK_DEST_DEFAULT_ALL,
  		     drop_types, n_drop_types,
  		     GDK_ACTION_COPY);
  g_signal_connect (window->defbox, "drag-data-received",
  		    G_CALLBACK (gdict_window_drag_data_received_cb),
  		    window);

  /* Sidebar */
  g_signal_connect (window->sidebar, "page-changed",
		    G_CALLBACK (sidebar_page_changed_cb),
		    window);
  g_signal_connect (window->sidebar, "closed",
		    G_CALLBACK (sidebar_closed_cb),
		    window);
  
  /* Speller */
  window->speller = gdict_speller_new ();
  if (window->context)
    gdict_speller_set_context (GDICT_SPELLER (window->speller), window->context);
  g_signal_connect (window->speller, "word-activated",
		    G_CALLBACK (speller_word_activated_cb),
		    window);
  gdict_sidebar_add_page (GDICT_SIDEBAR (window->sidebar),
		          GDICT_SIDEBAR_SPELLER_PAGE,
			  _("Similar words"),
			  window->speller);
  gtk_widget_show (window->speller);

  /* Database chooser */
  if (window->context)
    gdict_database_chooser_button_set_context (GDICT_DATABASE_CHOOSER_BUTTON (window->db_chooser),
					       window->context);
  g_signal_connect (window->db_chooser, "database-activated",
	  	    G_CALLBACK (database_activated_cb),
		    window);
  gtk_box_pack_start (GTK_BOX (window->header_box),
                      window->db_chooser,
                      TRUE, FALSE, 0);
  gtk_widget_show (window->db_chooser);

  /* bind the database property to the database setting */
  g_settings_bind (window->settings, GDICT_SETTINGS_DATABASE_KEY,
                   window, "database",
                   G_SETTINGS_BIND_DEFAULT);
  gdict_window_set_database (window, NULL);

  /* Strategy chooser */
  if (window->context)
    gdict_strategy_chooser_set_context (GDICT_STRATEGY_CHOOSER (window->strat_chooser),
                                        window->context);
  g_signal_connect (window->strat_chooser, "strategy-activated",
                    G_CALLBACK (strategy_activated_cb),
                    window);
  gdict_sidebar_add_page (GDICT_SIDEBAR (window->sidebar),
                          GDICT_SIDEBAR_STRATEGIES_PAGE,
                          _("Available strategies"),
                          window->strat_chooser);
  gtk_widget_show (window->strat_chooser);

  /* bind the strategy property to the strategy setting */
  g_settings_bind (window->settings, GDICT_SETTINGS_STRATEGY_KEY,
                   window, "strategy",
                   G_SETTINGS_BIND_DEFAULT);

  /* Source chooser */
  window->source_chooser = gdict_source_chooser_new_with_loader (window->loader);
  g_signal_connect (window->source_chooser, "source-activated",
                    G_CALLBACK (source_activated_cb),
                    window);
  gdict_sidebar_add_page (GDICT_SIDEBAR (window->sidebar),
                          GDICT_SIDEBAR_SOURCES_PAGE,
                          _("Dictionary sources"),
                          window->source_chooser);
  gtk_widget_show (window->source_chooser);

  /* bind the source-name property to the source setting */
  g_settings_bind (window->settings, GDICT_SETTINGS_SOURCE_KEY,
                   window, "source-name",
                   G_SETTINGS_BIND_DEFAULT);

  {
    GAction *action = g_action_map_lookup_action (G_ACTION_MAP (window), "view-sidebar");

    g_action_change_state (action, g_variant_new_boolean (window->sidebar_visible));
  }

  /* retrieve the document font size */
  font_name = g_settings_get_string (window->desktop_settings, DOCUMENT_FONT_KEY);
  gdict_window_set_defbox_font (window, font_name);
  font_desc = pango_font_description_from_string (font_name);
  g_free (font_name);

  g_settings_bind (window->desktop_settings, DOCUMENT_FONT_KEY,
                   window, "defbox-font",
                   G_SETTINGS_BIND_GET);

  /* if the (width, height) tuple is not defined, use the font to
   * calculate the right window geometry
   */
  if (window->default_width == -1 || window->default_height == -1)
    {
      gint font_size;
      gint width, height;
  
      font_size = pango_font_description_get_size (font_desc);
      font_size = PANGO_PIXELS (font_size);

      width = MAX (GDICT_WINDOW_COLUMNS * font_size, GDICT_WINDOW_MIN_WIDTH);
      height = MAX (GDICT_WINDOW_ROWS * font_size, GDICT_WINDOW_MIN_HEIGHT);

      window->default_width = width;
      window->default_height = height;
    }

  pango_font_description_free (font_desc);

  gtk_window_set_title (GTK_WINDOW (window), _("Dictionary"));
  gtk_window_set_default_size (GTK_WINDOW (window),
                               window->default_width,
                               window->default_height);
  if (window->is_maximized)
    gtk_window_maximize (GTK_WINDOW (window));

  if (window->sidebar_page != NULL)
    gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar), window->sidebar_page);
  else
    gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar), GDICT_SIDEBAR_SPELLER_PAGE);

  g_signal_connect (window, "delete-event",
		    G_CALLBACK (gdict_window_delete_event_cb),
		    NULL);
  g_signal_connect (window, "window-state-event",
		    G_CALLBACK (gdict_window_state_event_cb),
		    NULL);
  g_signal_connect (window, "size-allocate",
		    G_CALLBACK (gdict_window_size_allocate_event_cb),
		    NULL);
  g_signal_connect (window->main_pane, "notify::position",
		    G_CALLBACK (gdict_window_handle_notify_position_cb),
		    window);

  gtk_widget_grab_focus (window->entry);

  window->in_construction = FALSE;

  G_OBJECT_CLASS (gdict_window_parent_class)->constructed (gobject);
}

static void
gdict_window_class_init (GdictWindowClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  g_type_ensure (GDICT_TYPE_DEFBOX);
  g_type_ensure (GDICT_TYPE_SIDEBAR);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/Dictionary/gdict-app-window.ui");

  gtk_widget_class_bind_template_child (widget_class, GdictWindow, header_bar);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, entry);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, main_box);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, spinner);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, stack);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, header_box);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, main_pane);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, defbox_box);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, sidebar_box);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, defbox);
  gtk_widget_class_bind_template_child (widget_class, GdictWindow, sidebar);

  gdict_window_properties[PROP_ACTION] =
    g_param_spec_enum ("action",
                       "Action",
                       "The default action performed by the window",
                       GDICT_TYPE_WINDOW_ACTION,
                       GDICT_WINDOW_ACTION_CLEAR,
                       G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS |
                       G_PARAM_CONSTRUCT_ONLY);

  gdict_window_properties[PROP_SOURCE_LOADER] =
    g_param_spec_object ("source-loader",
                         "Source Loader",
                         "The GdictSourceLoader to be used to load dictionary sources",
                         GDICT_TYPE_SOURCE_LOADER,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS |
                         G_PARAM_CONSTRUCT_ONLY);

  gdict_window_properties[PROP_SOURCE_NAME] =
    g_param_spec_string ("source-name",
                         "Source Name",
                         "The name of the GdictSource to be used",
                         GDICT_DEFAULT_SOURCE_NAME,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_DATABASE] =
    g_param_spec_string ("database",
                         "Database",
                         "The name of the database to search",
                         GDICT_DEFAULT_DATABASE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_STRATEGY] =
    g_param_spec_string ("strategy",
                         "Strategy",
                         "The name of the strategy",
                         GDICT_DEFAULT_STRATEGY,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_PRINT_FONT] =
    g_param_spec_string ("print-font",
                         "Print Font",
                         "The font name to be used when printing",
                         GDICT_DEFAULT_PRINT_FONT,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_DEFBOX_FONT] =
    g_param_spec_string ("defbox-font",
                         "Defbox Font",
                         "The font name to be used by the defbox widget",
                         GDICT_DEFAULT_DEFBOX_FONT,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_WORD] =
    g_param_spec_string ("word",
                         "Word",
                         "The word to search",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gdict_window_properties[PROP_WINDOW_ID] =
    g_param_spec_uint ("window-id",
                       "Window ID",
                       "The unique identifier for this window",
                       0, G_MAXUINT,
                       0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  gdict_window_signals[CREATED] =
    g_signal_new ("created",
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GdictWindowClass, created),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1,
                  GDICT_TYPE_WINDOW);

  gobject_class->finalize = gdict_window_finalize;
  gobject_class->dispose = gdict_window_dispose;
  gobject_class->set_property = gdict_window_set_property;
  gobject_class->get_property = gdict_window_get_property;
  gobject_class->constructed = gdict_window_constructed;

  g_object_class_install_properties (gobject_class,
                                     LAST_PROP,
                                     gdict_window_properties);

  widget_class->size_allocate = gdict_window_size_allocate;
}

static void
gdict_window_init (GdictWindow *window)
{
  window->action = GDICT_WINDOW_ACTION_CLEAR;
  
  window->loader = NULL;
  window->context = NULL;

  window->settings = g_settings_new (GDICT_SETTINGS_SCHEMA);
  window->desktop_settings = g_settings_new (DESKTOP_SETTINGS_SCHEMA);

  window->word = NULL;
  window->source_name = NULL;
  window->print_font = NULL;
  window->defbox_font = NULL;

  window->database = NULL;
  window->strategy = NULL;

  window->default_width = -1;
  window->default_height = -1;
  window->current_width = -1;
  window->current_height = -1;
  window->is_maximized = FALSE;
  window->sidebar_visible = FALSE;
  window->sidebar_page = NULL;
  
  window->window_id = (gulong) time (NULL);

  gtk_widget_init_template (GTK_WIDGET (window));

  /* we need to create the chooser widgets for the sidebar before
   * we set the construction properties
   */
  window->db_chooser = gdict_database_chooser_button_new ();
  window->strat_chooser = gdict_strategy_chooser_new ();
}

GtkWidget *
gdict_window_new (GdictWindowAction  action,
                  GtkApplication    *app,
		  GdictSourceLoader *loader,
		  const gchar       *source_name,
                  const gchar       *database_name,
                  const gchar       *strategy_name,
		  const gchar       *word)
{
  GtkWidget *retval;
  GdictWindow *window;

  g_return_val_if_fail (GDICT_IS_SOURCE_LOADER (loader), NULL);
  
  retval = g_object_new (GDICT_TYPE_WINDOW,
                         "application", app,
  			 "action", action,
                         "source-loader", loader,
			 "source-name", source_name,
                         "database", database_name,
                         "strategy", strategy_name,
			 NULL);

  window = GDICT_WINDOW (retval);

  if (word && word[0] != '\0')
    {
      switch (action)
        {
	case GDICT_WINDOW_ACTION_LOOKUP:
	  gtk_entry_set_text (GTK_ENTRY (window->entry), word);
	  gdict_window_set_word (window, word, NULL);
	  break;

	case GDICT_WINDOW_ACTION_MATCH:
          {
          GdictSource *source;
          GdictContext *context;

	  gtk_entry_set_text (GTK_ENTRY (window->entry), word);
          
          gdict_window_set_sidebar_visible (window, TRUE);
          gdict_sidebar_view_page (GDICT_SIDEBAR (window->sidebar),
                                   GDICT_SIDEBAR_SPELLER_PAGE);

          /* we clone the context, so that the signals that it
           * fires do not get caught by the signal handlers we
           * use for getting the definitions.
           */
          source = gdict_source_loader_get_source (window->loader,
                                                   window->source_name);
          context = gdict_source_get_context (source);

          gdict_speller_set_context (GDICT_SPELLER (window->speller), context);
          
          g_object_unref (context);
          g_object_unref (source);
      
          gdict_speller_set_strategy (GDICT_SPELLER (window->speller),
                                      window->strategy);
      
          gdict_speller_match (GDICT_SPELLER (window->speller), word);
          }
          break;

	case GDICT_WINDOW_ACTION_CLEAR:
          gdict_defbox_clear (GDICT_DEFBOX (window->defbox));
	  break;

	default:
	  g_assert_not_reached ();
	  break;
	}
    }

  return retval;
}

/* GdictWindowAction */
static const GEnumValue _gdict_window_action_values[] = {
  { GDICT_WINDOW_ACTION_LOOKUP, "GDICT_WINDOW_ACTION_LOOKUP", "lookup" },
  { GDICT_WINDOW_ACTION_MATCH, "GDICT_WINDOW_ACTION_MATCH", "match" },
  { GDICT_WINDOW_ACTION_CLEAR, "GDICT_WINDOW_ACTION_CLEAR", "clear" },
  { 0, NULL, NULL }
};

GType
gdict_window_action_get_type (void)
{
  static GType our_type = 0;

  if (!our_type)
    our_type = g_enum_register_static ("GdictWindowAction", _gdict_window_action_values);

  return our_type;
}
