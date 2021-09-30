/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-app-tracker"

#include "config.h"

#include "app-tracker.h"
#include "phosh-wayland.h"
#include "shell.h"
#include "toplevel-manager.h"
#include "phosh-marshalers.h"
#include "util.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

/**
 * SECTION:app-tracker
 * @short_description: Application state tracker
 * @Title: PhoshAppTracker
 *
 * Tracks the startup state of applications
 */

enum {
  APP_LAUNCHED,
  APP_READY,
  APP_FAILED,
  APP_ACTIVATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];


/**
 * PhoshAppStateFlags:
 *
 * Application state based on startup id
 *
 * PHOSH_APP_TRACKER_STATE_FLAG_UNKNOWN: App state unknown
 * PHOSH_APP_TRACKER_STATE_FLAG_LAUNCHED: process was spawned by us
 *     Gio told us it spawned the process
 * PHOSH_APP_TRACKER_STATE_FLAG_DBUS_LAUNCH: process launch seen on DBus via
 *     org.gtk.gio.DesktopAppInfo
 * PHOSH_APP_TRACKER_STATE_FLAG_WL_LAUNCH: startup id seen via gtk_shell1's notify_launch
 *     Sent by launcher to indicate launch. GTK specific.
 * PHOSH_APP_TRACKER_STATE_FLAG_WL_STARTUP_ID: startup id seen via gtk_shell1's set_startup_id
 *     Sent by launchee. This indicates startup is complete. GTK specific.
 */
typedef enum {
  PHOSH_APP_TRACKER_STATE_FLAG_UNKNOWN         = 0,
  PHOSH_APP_TRACKER_STATE_FLAG_LAUNCHED        = (1 << 0),
  PHOSH_APP_TRACKER_STATE_FLAG_DBUS_LAUNCH     = (1 << 1),
  PHOSH_APP_TRACKER_STATE_FLAG_WL_LAUNCH       = (1 << 2),
  PHOSH_APP_TRACKER_STATE_FLAG_WL_STARTUP_ID   = (1 << 3),
} PhoshAppStateFlags;

typedef struct  {
  gint64             pid;
  PhoshAppStateFlags state;

  char              *startup_id;
  GDesktopAppInfo   *info;
} PhoshAppState;

struct _PhoshAppTracker {
  GObject          parent;

  GDBusConnection *session_bus;
  guint            dbus_id;
  guint            idle_id;
  struct phosh_private_startup_tracker *wl_tracker; /* PhoshPrivate wayland interface */
  GHashTable      *apps;
};
G_DEFINE_TYPE (PhoshAppTracker, phosh_app_tracker, G_TYPE_OBJECT)


static PhoshAppState *
phosh_app_state_new (GDesktopAppInfo   *info,
                     const char        *startup_id,
                     gint64             pid,
                     PhoshAppStateFlags flags)
{
  PhoshAppState *state = g_new0 (PhoshAppState, 1);

  state->startup_id = g_strdup (startup_id);
  state->pid = pid;
  state->state = flags;
  state->info = g_object_ref (info);

  g_debug ("Pid %" G_GINT64_FORMAT ", '%s', startup-id: %s got state %d",
           state->pid,
           g_app_info_get_name (G_APP_INFO (info)),
           state->startup_id,
           state->state);

  return state;
}


static void
phosh_app_state_free (PhoshAppState *state)
{
  g_object_unref (state->info);
  g_free (state->startup_id);

  g_free (state);
}


static PhoshAppState*
update_app_state (PhoshAppTracker   *self,
                  const char        *startup_id,
                  PhoshAppStateFlags flags)
{
  PhoshAppState *state;

  state = g_hash_table_lookup (self->apps, startup_id);
  g_return_val_if_fail (state, NULL);

  g_debug ("Pid %" G_GINT64_FORMAT ", startup-id: %s got state %d",
           state->pid,
           state->startup_id,
           flags);

  state->state |= flags;

  return state;
}


static void
startup_tracker_handle_launched (void                                 *data,
                                 struct phosh_private_startup_tracker *startup_tracker,
                                 const char                           *startup_id,
                                 unsigned int                          protocol,
                                 unsigned int                          flags)
{
  PhoshAppState *state;
  PhoshAppTracker *self = PHOSH_APP_TRACKER (data);

  g_debug ("%s %s %d", __func__, startup_id, protocol);
  g_return_if_fail (PHOSH_IS_APP_TRACKER (self));
  g_return_if_fail (startup_id != NULL);

  state = g_hash_table_lookup (self->apps, startup_id);
  /*
   * TODO: this can be dropped once
   * https://gitlab.gnome.org/GNOME/glib/-/merge_requests/2227
   * is applied or another solution is in place for DBus activated
   * apps.
   */
  if (!state) {
    g_debug ("No info for startup_id '%s' found", startup_id);
    return;
  }

  update_app_state (self, startup_id, PHOSH_APP_TRACKER_STATE_FLAG_WL_LAUNCH);
}


static void
startup_tracker_handle_startup_id (void                                 *data,
                                   struct phosh_private_startup_tracker *startup_tracker,
                                   const char                           *startup_id,
                                   unsigned int                          protocol,
                                   unsigned int                          flags)

{
  PhoshAppState *state;
  PhoshAppTracker *self = PHOSH_APP_TRACKER (data);

  g_debug ("%s %s %d", __func__, startup_id, protocol);
  g_return_if_fail (PHOSH_IS_APP_TRACKER (self));
  g_return_if_fail (startup_id != NULL);

  state = g_hash_table_lookup (self->apps, startup_id);
  /* Apps often reuse the the startup_id for multiple windows */
  if (!state) {
    g_debug ("No info for startup_id '%s' found", startup_id);
    return;
  }

  update_app_state (self, startup_id, PHOSH_APP_TRACKER_STATE_FLAG_WL_STARTUP_ID);
  g_signal_emit (self, signals[APP_READY], 0, state->info, startup_id);

  /* Startup sequence done */
  g_hash_table_remove (self->apps, startup_id);
}


static const struct phosh_private_startup_tracker_listener startup_tracker_listener = {
  .startup_id = startup_tracker_handle_startup_id,
  .launched = startup_tracker_handle_launched,
};


static void
on_app_launched (PhoshAppTracker   *self,
                 GDesktopAppInfo   *info,
                 GVariant          *platform_data,
                 GAppLaunchContext *context)
{
  g_autofree char *startup_id = NULL;
  PhoshAppState *state;
  gint32 pid;

  g_return_if_fail (G_IS_DESKTOP_APP_INFO (info));
  /*
   * We can't do anything useful if the compositor doesn't send events
   * so make sure the user is aware.
   */
  g_return_if_fail (self->wl_tracker);

  /* Application doesn't handle startup notifications */
  if (!g_desktop_app_info_get_boolean (info, "StartupNotify"))
    goto out;

  /* Launched via spawn */
  g_variant_lookup (platform_data, "startup-notification-id", "s", &startup_id);
  g_variant_lookup (platform_data, "pid", "i", &pid);

  /* No startup_id for e.g. Qt apps */
  if (!startup_id) {
    g_debug ("No startup_id for %s", g_app_info_get_id (G_APP_INFO (info)));
    goto out;
  }

  g_return_if_fail (startup_id);
  /* If we saw the startup-id already, something is wrong */
  g_return_if_fail (!g_hash_table_contains (self->apps, startup_id));

  state = phosh_app_state_new (info, startup_id, pid, PHOSH_APP_TRACKER_STATE_FLAG_LAUNCHED);
  g_hash_table_insert (self->apps, g_steal_pointer (&startup_id), state);

  g_debug ("Launched app '%s' with Startup id: '%s'",
           g_app_info_get_name (G_APP_INFO (info)),
           state->startup_id);

  g_signal_emit (self, signals[APP_LAUNCHED],
                 g_quark_from_static_string ("spawn"),
                 info,
                 state->startup_id);
 out:
  g_object_unref (context);
}


static void
on_app_launch_failed (PhoshAppTracker   *self,
                      char              *startup_id,
                      GAppLaunchContext *context)
{
  PhoshAppState *state;

  g_return_if_fail (PHOSH_IS_APP_TRACKER (self));
  g_return_if_fail (startup_id != NULL);

  state = g_hash_table_lookup (self->apps, startup_id);
  if (!state) {
    g_debug ("No info for startup_id '%s' found", startup_id);
    goto out;
  }

  g_warning ("Failed to launch app '%s' with startup id: '%s'",
             g_app_info_get_name (G_APP_INFO (state->info)),
             state->startup_id);

  g_signal_emit (self, signals[APP_FAILED], 0, state->info, startup_id);

  g_hash_table_remove (self->apps, startup_id);
 out:
  g_object_unref (context);
}


static void
on_dbus_app_launched (GDBusConnection *connection,
                      const char      *sender_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *signal_name,
                      GVariant        *parameters,
                      PhoshAppTracker *self)
{
  gint64 pid;

  g_autoptr (GVariant) var_dict = NULL, var_desktop_file = NULL;
  g_autofree char *startup_id = NULL;
  const char *desktop_file = NULL;
  GVariantDict dict;

  g_return_if_fail (PHOSH_IS_APP_TRACKER (self));
  /*
   * We can't do anything useful if the compositor doesn't send events
   * so make sure the user is aware.
   */
  g_return_if_fail (self->wl_tracker);

  g_variant_get (parameters, "(@aysxas@a{sv})", &var_desktop_file, NULL, &pid, NULL, &var_dict);

  desktop_file = g_variant_get_bytestring (var_desktop_file);

  if (desktop_file == NULL || *desktop_file == '\0')
    return;

  g_variant_dict_init (&dict, var_dict);
  g_variant_dict_lookup (&dict, "startup-id", "s", &startup_id);

  if (!startup_id)
    return;

  if (g_hash_table_contains (self->apps, startup_id)) {
    /* App already known, likely launched by us */
    g_debug ("'%s' (%s) already known", startup_id, desktop_file);
    update_app_state (self, startup_id, PHOSH_APP_TRACKER_STATE_FLAG_DBUS_LAUNCH);
  } else {
    GDesktopAppInfo *info;
    PhoshAppState *state;
    g_autofree char *app_id = g_path_get_basename (desktop_file);

    info = g_desktop_app_info_new (app_id);
    if (!info) {
      g_debug ("No desktop file for '%s'", app_id);
      return;
    }

    g_debug ("DBus launch %s startup-id %s", desktop_file, startup_id);
    state = phosh_app_state_new (info, startup_id, pid, PHOSH_APP_TRACKER_STATE_FLAG_DBUS_LAUNCH);
    g_hash_table_insert (self->apps, g_steal_pointer (&startup_id), state);

    g_signal_emit (self, signals[APP_LAUNCHED],
                   g_quark_from_static_string ("gio-dbus"),
                   info,
                   state->startup_id);

  }
}


static void
on_bus_get_finished (GObject         *source_object,
                     GAsyncResult    *res,
                     PhoshAppTracker *self)
{
  g_autoptr (GError) err = NULL;
  GDBusConnection *session_bus;

  session_bus = g_bus_get_finish (res, &err);
  if (!session_bus) {
    g_warning ("Failed to attach to session bus: %s", err->message);
    return;
  }
  self->session_bus = session_bus;

  /* Listen for spawned apps */
  self->dbus_id = g_dbus_connection_signal_subscribe (self->session_bus,
                                                      NULL,
                                                      "org.gtk.gio.DesktopAppInfo",
                                                      "Launched",
                                                      "/org/gtk/gio/DesktopAppInfo",
                                                      NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                                      (GDBusSignalCallback)on_dbus_app_launched,
                                                      self, NULL);
}


static gboolean
on_idle (PhoshAppTracker *self)
{
  g_bus_get (G_BUS_TYPE_SESSION,
             NULL,
             (GAsyncReadyCallback)on_bus_get_finished,
             self);

  self->idle_id = 0;
  return G_SOURCE_REMOVE;
}


static void
phosh_app_tracker_finalize (GObject *object)
{
  PhoshAppTracker *self = PHOSH_APP_TRACKER (object);

  g_clear_pointer (&self->apps, g_hash_table_destroy);
  g_clear_pointer (&self->wl_tracker, phosh_private_startup_tracker_destroy);

  g_clear_handle_id (&self->idle_id, g_source_remove);
  if (self->dbus_id) {
    g_dbus_connection_signal_unsubscribe (self->session_bus, self->dbus_id);
    self->dbus_id = 0;
  }
  g_clear_object (&self->session_bus);


  G_OBJECT_CLASS (phosh_app_tracker_parent_class)->finalize (object);
}


static void
phosh_app_tracker_class_init (PhoshAppTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = phosh_app_tracker_finalize;

  /**
   * PhoshAppTracker:app-launched
   *
   * The app was launched by the shell.
   */
  signals[APP_LAUNCHED] = g_signal_new ("app-launched",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                        0, NULL, NULL,
                                        _phosh_marshal_VOID__OBJECT_STRING,
                                        G_TYPE_NONE,
                                        2,
                                        G_TYPE_APP_INFO,
                                        G_TYPE_STRING);
  g_signal_set_va_marshaller (signals[APP_LAUNCHED],
                              G_TYPE_FROM_CLASS (klass),
                              _phosh_marshal_VOID__OBJECT_STRINGv);
  /**
   * PhoshAppTracker:app-ready
   *
   * The app is ready to be used by the user
   */
  signals[APP_READY] = g_signal_new ("app-ready",
                                     G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL,
                                     _phosh_marshal_VOID__OBJECT_STRING,
                                     G_TYPE_NONE,
                                     2,
                                     G_TYPE_APP_INFO,
                                     G_TYPE_STRING);
  g_signal_set_va_marshaller (signals[APP_READY],
                              G_TYPE_FROM_CLASS (klass),
                              _phosh_marshal_VOID__OBJECT_STRINGv);

  /**
   * PhoshAppTracker:app-failed
   *
   * The app failed to launch
   */
  signals[APP_FAILED] = g_signal_new ("app-failed",
                                      G_TYPE_FROM_CLASS (klass),
                                      G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL,
                                      _phosh_marshal_VOID__OBJECT_STRING,
                                      G_TYPE_NONE,
                                      2,
                                      G_TYPE_APP_INFO,
                                      G_TYPE_STRING);
  g_signal_set_va_marshaller (signals[APP_FAILED],
                              G_TYPE_FROM_CLASS (klass),
                              _phosh_marshal_VOID__OBJECT_STRINGv);
  /**
   * PhoshAppTracker:app-activated
   *
   * Already running app was activated
   */
  signals[APP_ACTIVATED] = g_signal_new ("app-activated",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         1,
                                         G_TYPE_APP_INFO);
}


static void
phosh_app_tracker_init (PhoshAppTracker *self)
{
  PhoshWayland *wl = phosh_wayland_get_default ();
  struct phosh_private *phosh_private = phosh_wayland_get_phosh_private (wl);
  uint32_t version;

  self->apps = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      (GDestroyNotify) phosh_app_state_free);
  self->idle_id = g_idle_add ((GSourceFunc)on_idle, self);

  version = phosh_wayland_get_phosh_private_version (wl);
  if (!phosh_private || version < PHOSH_PRIVATE_STARTUP_NOTIFY_SINCE) {
    g_warning ("Compositor lacks app startup tracker support");
    return;
  }

  if ((self->wl_tracker = phosh_private_get_startup_tracker (phosh_private)) == NULL) {
    g_critical ("Failed to retrieve startup tracker from wayland interface");
    return;
  }

  phosh_private_startup_tracker_add_listener (self->wl_tracker, &startup_tracker_listener, self);
}


PhoshAppTracker *
phosh_app_tracker_new (void)
{
  return PHOSH_APP_TRACKER (g_object_new (PHOSH_TYPE_APP_TRACKER, NULL));
}

void
phosh_app_tracker_launch_app_info (PhoshAppTracker *self, GAppInfo *info)
{
  g_autoptr (GdkAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;
  PhoshToplevelManager *toplevel_manager = phosh_shell_get_toplevel_manager (phosh_shell_get_default ());
  g_autofree char *app_id = NULL;
  g_autofree char *startup_id = NULL;
  gboolean success;

  app_id = phosh_strip_suffix_from_app_id (g_app_info_get_id (G_APP_INFO (info)));
  g_debug ("Launching '%s'", app_id);

  for (guint i=0; i < phosh_toplevel_manager_get_num_toplevels (toplevel_manager); i++) {
    PhoshToplevel *toplevel = phosh_toplevel_manager_get_toplevel (toplevel_manager, i);
    const char *window_id = phosh_toplevel_get_app_id (toplevel);
    g_autofree char *fixed_id = phosh_fix_app_id (window_id);

    if (g_strcmp0 (app_id, window_id) == 0 || g_strcmp0 (app_id, fixed_id) == 0) {
      /* activate the first matching window for now, since we don't have toplevels sorted by last-focus yet */
      phosh_toplevel_activate (toplevel, phosh_wayland_get_wl_seat (phosh_wayland_get_default ()));
      g_signal_emit (self, signals[APP_ACTIVATED], 0, info);
      return;
    }
  }

  context = gdk_display_get_app_launch_context (gdk_display_get_default ());
  g_object_ref (context);
  g_signal_connect_swapped (G_APP_LAUNCH_CONTEXT (context),
                            "launched",
                            G_CALLBACK (on_app_launched),
                            self);
  g_signal_connect_swapped (G_APP_LAUNCH_CONTEXT (context),
                            "launch-failed",
                            G_CALLBACK (on_app_launch_failed),
                            self);

  success = g_desktop_app_info_launch_uris_as_manager (G_DESKTOP_APP_INFO (info),
                                                       NULL,
                                                       G_APP_LAUNCH_CONTEXT (context),
                                                       G_SPAWN_SEARCH_PATH,
                                                       NULL, NULL,
                                                       NULL, NULL,
                                                       &error);
  if (!success) {
    g_critical ("Failed to launch app %s: %s",
                g_app_info_get_id (info),
                error->message);
  }
}