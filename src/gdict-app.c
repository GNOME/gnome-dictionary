/* gdict-app.c - main application class
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>

#include "gdict-common.h"
#include "gdict-about.h"
#include "gdict-pref-dialog.h"
#include "gdict-app.h"

G_DEFINE_TYPE (GdictApp, gdict_app, GTK_TYPE_APPLICATION)

static GOptionEntry gdict_app_goptions[] = {
  {
    "look-up", 0,
    0,
    G_OPTION_ARG_STRING_ARRAY, NULL,
    N_("Words to look up"), N_("WORD")
  },
  {
    "match", 0,
    0,
    G_OPTION_ARG_STRING_ARRAY, NULL,
    N_("Words to match"), N_("WORD")
  },
  {
    "source", 's',
    0,
    G_OPTION_ARG_STRING, NULL,
    N_("Dictionary source to use"), N_("NAME")
  },
  {
    "database", 'D',
    0,
    G_OPTION_ARG_STRING, NULL,
    N_("Database to use"), N_("NAME")
  },
  {
    "strategy", 'S',
    0,
    G_OPTION_ARG_STRING, NULL,
    N_("Strategy to use"), N_("NAME")
  },
  {
    G_OPTION_REMAINING, 0, 0,
    G_OPTION_ARG_STRING_ARRAY, NULL,
    N_("Words to look up"), N_("WORDS")
  },
  { NULL },
};

static void
gdict_app_cmd_new (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  GdictApp *app = user_data;
  GtkWidget *window = gdict_window_new (GDICT_WINDOW_ACTION_CLEAR,
                                        GTK_APPLICATION (app),
                                        app->loader,
                                        NULL, NULL, NULL,
                                        NULL);

  gtk_widget_show (window);
}

static void
gdict_app_cmd_preferences (GSimpleAction *action,
                           GVariant      *parameter,
                           gpointer       user_data)
{
  GtkApplication *app = user_data;
  GdictWindow *window;

  g_assert (GTK_IS_APPLICATION (app));

  window = GDICT_WINDOW (gtk_application_get_windows (app)->data);
  gdict_show_pref_dialog (GTK_WIDGET (window),
                          _("Dictionary Preferences"),
                          window->loader);
}

static void
gdict_app_cmd_help (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  GtkApplication *app = user_data;
  GdictWindow *window;
  GError *err = NULL;

  g_return_if_fail (GTK_IS_APPLICATION (app));

  window = GDICT_WINDOW (gtk_application_get_windows (app)->data);

#if GTK_CHECK_VERSION (3, 22, 0)
  gtk_show_uri_on_window (GTK_WINDOW (window),
                          "help:gnome-dictionary",
                          gtk_get_current_event_time (),
                          &err);
#else
  gtk_show_uri (gtk_widget_get_screen (GTK_WIDGET (window)),
                "help:gnome-dictionary",
                gtk_get_current_event_time (), &err);
#endif

  if (err)
    {
      gdict_show_gerror_dialog (GTK_WINDOW (window),
                                _("There was an error while displaying help"),
                                err);
      g_error_free (err);
    }
}

static void
gdict_app_cmd_about (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  GtkApplication *app = user_data;
  GdictWindow *window;

  g_assert (GTK_IS_APPLICATION (app));

  window = GDICT_WINDOW (gtk_application_get_windows (app)->data);
  gdict_show_about_dialog (GTK_WIDGET (window));
}

static void
gdict_app_cmd_quit (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  GtkApplication *app = user_data;
  GList *windows;

  g_assert (GTK_IS_APPLICATION (app));

  windows = gtk_application_get_windows (app);
  g_list_foreach (windows, (GFunc)gtk_widget_destroy, NULL);
}

static const GActionEntry app_entries[] =
{
  { "new", gdict_app_cmd_new, NULL, NULL, NULL },
  { "preferences", gdict_app_cmd_preferences, NULL, NULL, NULL },
  { "help", gdict_app_cmd_help, NULL, NULL, NULL },
  { "about", gdict_app_cmd_about, NULL, NULL, NULL },
  { "quit", gdict_app_cmd_quit, NULL, NULL, NULL }
};

static void
gdict_app_dispose (GObject *object)
{
  GdictApp *app = GDICT_APP (object);

  g_clear_object (&app->loader);

  G_OBJECT_CLASS (gdict_app_parent_class)->dispose (object);
}

static gchar **
strv_concat (gchar **strv1, gchar **strv2)
{
  gchar **tmp;
  guint len1, len2;
  gint i;

  len1 = g_strv_length (strv1);
  len2 = g_strv_length (strv2);
  tmp = g_realloc (strv1, len1 + len2 + 1);
  for (i = 0; i < len2; i++)
    tmp[len1 + i] = (gchar *)strv2[i];
  tmp[len1 + len2] = NULL;

  return tmp;
}

static int
gdict_app_command_line (GApplication            *application,
                        GApplicationCommandLine *cmd_line)
{
  GdictApp *app = GDICT_APP (application);
  gsize words_len, i;
  GVariantDict *options;
  gchar **gdict_lookup_words = NULL;
  gchar **gdict_match_words = NULL;
  gchar **remaining = NULL;
  const gchar *gdict_source_name = NULL;
  const gchar *gdict_database_name = NULL;
  const gchar *gdict_strategy_name = NULL;

  options = g_application_command_line_get_options_dict (cmd_line);

  g_variant_dict_lookup (options, "look-up", "^as", &gdict_lookup_words);
  g_variant_dict_lookup (options, "match", "^as", &gdict_match_words);
  g_variant_dict_lookup (options, "source", "&s", &gdict_source_name);
  g_variant_dict_lookup (options, "database", "&s", &gdict_database_name);
  g_variant_dict_lookup (options, "strategy", "&s", &gdict_strategy_name);
  g_variant_dict_lookup (options, G_OPTION_REMAINING, "^as", &remaining);

  if (remaining != NULL)
    {
      if (gdict_match_words != NULL)
        {
          gchar **tmp;
          tmp = strv_concat (gdict_match_words, remaining);
          g_strfreev (gdict_match_words);
          g_strfreev (remaining);
          gdict_match_words = tmp;
        }
      else
        {
          gdict_match_words = remaining;
        }
      remaining = NULL;
    }

  if (gdict_lookup_words == NULL &&
      gdict_match_words == NULL)
    {
      GtkWidget *window = gdict_window_new (GDICT_WINDOW_ACTION_CLEAR,
                                            GTK_APPLICATION (application),
                                            app->loader,
                                            gdict_source_name,
                                            gdict_database_name,
                                            gdict_strategy_name,
                                            NULL);
      gtk_widget_show (window);

      goto out;
    }

  if (gdict_lookup_words != NULL)
    words_len = g_strv_length (gdict_lookup_words);
  else
    words_len = 0;

  for (i = 0; i < words_len; i++)
    {
      const gchar *word = gdict_lookup_words[i];
      GtkWidget *window;

      window = gdict_window_new (GDICT_WINDOW_ACTION_LOOKUP,
                                 GTK_APPLICATION (application),
                                 app->loader,
                                 gdict_source_name,
                                 gdict_database_name,
                                 gdict_strategy_name,
                                 word);
      
      gtk_widget_show (window);
    }

  if (gdict_match_words != NULL)
    words_len = g_strv_length (gdict_match_words);
  else
    words_len = 0;

  for (i = 0; i < words_len; i++)
    {
      const gchar *word = gdict_match_words[i];
      GtkWidget *window;

      window = gdict_window_new (GDICT_WINDOW_ACTION_MATCH,
                                 GTK_APPLICATION (application),
                                 app->loader,
				 gdict_source_name,
                                 gdict_database_name,
                                 gdict_strategy_name,
				 word);
      
      gtk_widget_show (window);
    }

out:
  g_strfreev (gdict_lookup_words);
  g_strfreev (gdict_match_words);

  return 0;
}

static void
gdict_app_activate (GApplication *application)
{
  GdictApp *app = GDICT_APP (application);
  GtkWidget *window = gdict_window_new (GDICT_WINDOW_ACTION_CLEAR,
                                        GTK_APPLICATION (application),
                                        app->loader,
                                        NULL, NULL, NULL,
                                        NULL);

  gtk_widget_show (window);
}

static void
gdict_app_startup (GApplication *application)
{
  static const char *lookup_accels[2] = { "<Primary>l", NULL };
  static const char *escape_accels[2] = { "Escape", NULL };

  G_APPLICATION_CLASS (gdict_app_parent_class)->startup (application);

  g_action_map_add_action_entries (G_ACTION_MAP (application),
                                   app_entries, G_N_ELEMENTS (app_entries),
                                   application);

  gtk_application_set_accels_for_action (GTK_APPLICATION (application), "win.lookup", lookup_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (application), "win.escape", escape_accels);
}

static void
gdict_app_class_init (GdictAppClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

  gobject_class->dispose = gdict_app_dispose;

  application_class->startup = gdict_app_startup;
  application_class->activate = gdict_app_activate;
  application_class->command_line = gdict_app_command_line;
}

static void
gdict_app_init (GdictApp *app)
{
  char *loader_path;

  /* add user's path for fetching dictionary sources */  
  app->loader = gdict_source_loader_new ();
  loader_path = gdict_get_config_dir ();
  gdict_source_loader_add_search_path (app->loader, loader_path);
  g_free (loader_path);

  /* Add the command line options */
  g_application_add_main_option_entries (G_APPLICATION (app), gdict_app_goptions);

  /* Set main application icon */
  gtk_window_set_default_icon_name ("org.gnome.Dictionary");
}

GApplication *
gdict_app_new (void)
{
  return g_object_new (gdict_app_get_type (),
                       "application-id", "org.gnome.Dictionary",
                       "resource-base-path", "/org/gnome/Dictionary",
                       "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                       NULL);
}
