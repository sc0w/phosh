/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 *
 * Once based on maynard's panel which is
 * Copyright (C) 2014 Collabora Ltd. *
 * Author: Jonny Lamb <jonny.lamb@collabora.co.uk>
 */

#define G_LOG_DOMAIN "phosh-shell"

#define WWAN_BACKEND_KEY "wwan-backend"

#include <stdlib.h>
#include <string.h>

#include <glib-object.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

#include "config.h"
#include "drag-surface.h"
#include "shell.h"
#include "app-tracker.h"
#include "batteryinfo.h"
#include "background-manager.h"
#include "bt-info.h"
#include "bt-manager.h"
#include "connectivity-info.h"
#include "calls-manager.h"
#include "docked-info.h"
#include "docked-manager.h"
#include "fader.h"
#include "feedbackinfo.h"
#include "feedback-manager.h"
#include "gnome-shell-manager.h"
#include "gtk-mount-manager.h"
#include "hks-info.h"
#include "home.h"
#include "idle-manager.h"
#include "keyboard-events.h"
#include "location-info.h"
#include "location-manager.h"
#include "lockscreen-manager.h"
#include "media-player.h"
#include "mode-manager.h"
#include "monitor-manager.h"
#include "monitor/monitor.h"
#include "mount-manager.h"
#include "settings.h"
#include "system-modal-dialog.h"
#include "network-auth-manager.h"
#include "notifications/notify-manager.h"
#include "notifications/notification-banner.h"
#include "osk-manager.h"
#include "phosh-private-client-protocol.h"
#include "phosh-wayland.h"
#include "polkit-auth-agent.h"
#include "proximity.h"
#include "quick-setting.h"
#include "run-command-manager.h"
#include "rotateinfo.h"
#include "rotation-manager.h"
#include "sensor-proxy-manager.h"
#include "screen-saver-manager.h"
#include "screenshot-manager.h"
#include "session-manager.h"
#include "splash-manager.h"
#include "system-prompter.h"
#include "top-panel.h"
#include "torch-manager.h"
#include "torch-info.h"
#include "util.h"
#include "vpn-info.h"
#include "wifiinfo.h"
#include "wwaninfo.h"
#include "wwan/phosh-wwan-ofono.h"
#include "wwan/phosh-wwan-mm.h"
#include "wwan/phosh-wwan-backend.h"

/**
 * SECTION:shell
 * @short_description: The shell singleton
 * @Title: PhoshShell
 *
 * #PhoshShell is responsible for instantiating the GUI
 * parts of the shell#PhoshTopPanel, #PhoshHome,… and the managers that
 * interface with DBus #PhoshMonitorManager, #PhoshFeedbackManager, …
 * and coordinates between them.
 */

enum {
  PROP_0,
  PROP_LOCKED,
  PROP_BUILTIN_MONITOR,
  PROP_PRIMARY_MONITOR,
  PROP_SHELL_STATE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

enum {
  READY,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct
{
  PhoshDragSurface *panel;
  PhoshLayerSurface *home;
  GPtrArray *faders;              /* for final fade out */

  GtkWidget *notification_banner;

  PhoshAppTracker *app_tracker;
  PhoshSessionManager *session_manager;
  PhoshBackgroundManager *background_manager;
  PhoshCallsManager *calls_manager;
  PhoshMonitor *primary_monitor;
  PhoshMonitor *builtin_monitor;
  PhoshMonitorManager *monitor_manager;
  PhoshLockscreenManager *lockscreen_manager;
  PhoshIdleManager *idle_manager;
  PhoshOskManager  *osk_manager;
  PhoshToplevelManager *toplevel_manager;
  PhoshWifiManager *wifi_manager;
  PhoshPolkitAuthAgent *polkit_auth_agent;
  PhoshScreenSaverManager *screen_saver_manager;
  PhoshScreenshotManager *screenshot_manager;
  PhoshNotifyManager *notify_manager;
  PhoshFeedbackManager *feedback_manager;
  PhoshBtManager *bt_manager;
  PhoshMountManager *mount_manager;
  PhoshWWan *wwan;
  PhoshTorchManager *torch_manager;
  PhoshModeManager *mode_manager;
  PhoshDockedManager *docked_manager;
  PhoshGtkMountManager *gtk_mount_manager;
  PhoshHksManager *hks_manager;
  PhoshKeyboardEvents *keyboard_events;
  PhoshLocationManager *location_manager;
  PhoshGnomeShellManager *gnome_shell_manager;
  PhoshSplashManager *splash_manager;
  PhoshRunCommandManager *run_command_manager;
  PhoshNetworkAuthManager *network_auth_manager;
  PhoshVpnManager *vpn_manager;

  /* sensors */
  PhoshSensorProxyManager *sensor_proxy_manager;
  PhoshProximity *proximity;
  PhoshRotationManager *rotation_manager;

  PhoshShellDebugFlags debug_flags;
  gboolean             startup_finished;
  guint                startup_finished_id;


  /* Mirrors PhoshLockscreenManager's locked property */
  gboolean locked;

  PhoshShellStateFlags shell_state;

  char           *theme_name;
  GtkCssProvider *css_provider;
} PhoshShellPrivate;


typedef struct _PhoshShell
{
  GObject parent;
} PhoshShell;

G_DEFINE_TYPE_WITH_PRIVATE (PhoshShell, phosh_shell, G_TYPE_OBJECT)


static void
settings_activated_cb (PhoshShell    *self,
                       PhoshTopPanel *window)
{
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);

  g_return_if_fail (PHOSH_IS_TOP_PANEL (priv->panel));
  phosh_top_panel_toggle_fold (PHOSH_TOP_PANEL(priv->panel));
}


static void
on_home_state_changed (PhoshShell *self, GParamSpec *pspec, PhoshHome *home)
{
  PhoshShellPrivate *priv;
  PhoshHomeState state;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_HOME (home));

  priv = phosh_shell_get_instance_private (self);

  g_object_get (priv->home, "state", &state, NULL);
  if (state == PHOSH_HOME_STATE_UNFOLDED) {
    phosh_top_panel_fold (PHOSH_TOP_PANEL (priv->panel));
    phosh_osk_manager_set_visible (priv->osk_manager, FALSE);
  }
  phosh_shell_set_state (self, PHOSH_STATE_OVERVIEW, state == PHOSH_HOME_STATE_UNFOLDED);
}


static void
panels_create (PhoshShell *self)
{
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);
  PhoshMonitor *monitor;
  PhoshWayland *wl = phosh_wayland_get_default ();
  PhoshAppGrid *app_grid;

  monitor = phosh_shell_get_primary_monitor (self);
  g_return_if_fail (monitor);

  priv->panel = PHOSH_DRAG_SURFACE (phosh_top_panel_new (phosh_wayland_get_zwlr_layer_shell_v1 (wl),
                                                         phosh_wayland_get_zphoc_layer_shell_effects_v1 (wl),
                                                         monitor->wl_output));
  gtk_widget_show (GTK_WIDGET (priv->panel));

  priv->home = PHOSH_LAYER_SURFACE(phosh_home_new (phosh_wayland_get_zwlr_layer_shell_v1(wl),
                                                    monitor->wl_output));
  gtk_widget_show (GTK_WIDGET (priv->home));

  g_signal_connect_swapped (
    priv->panel,
    "settings-activated",
    G_CALLBACK(settings_activated_cb),
    self);

  g_signal_connect_swapped (
    priv->home,
    "notify::state",
    G_CALLBACK(on_home_state_changed),
    self);

  app_grid = phosh_overview_get_app_grid (phosh_home_get_overview (PHOSH_HOME (priv->home)));
  g_object_bind_property (priv->docked_manager,
                          "enabled",
                          app_grid,
                          "filter-adaptive",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);
}


static void
panels_dispose (PhoshShell *self)
{
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);

  g_clear_pointer (&priv->panel, phosh_cp_widget_destroy);
  g_clear_pointer (&priv->home, phosh_cp_widget_destroy);
}


/* Select proper style sheet in case of high contrast */
static void
on_gtk_theme_name_changed (PhoshShell *self, GParamSpec *pspec, GtkSettings *settings)
{
  const char *style;
  g_autofree char *name = NULL;
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);
  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();

  g_object_get (settings, "gtk-theme-name", &name, NULL);

  if (g_strcmp0 (priv->theme_name, name) == 0)
    return;

  priv->theme_name = g_steal_pointer (&name);
  g_debug ("GTK theme: %s", priv->theme_name);

  if (priv->css_provider) {
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default (),
                                                 GTK_STYLE_PROVIDER (priv->css_provider));
  }

  if (g_strcmp0 (priv->theme_name, "HighContrast") == 0)
    style = "/sm/puri/phosh/stylesheet/adwaita-hc-light.css";
  else
    style = "/sm/puri/phosh/stylesheet/adwaita-dark.css";

  gtk_css_provider_load_from_resource (provider, style);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_set_object (&priv->css_provider, provider);
}


static void
phosh_shell_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
  PhoshShell *self = PHOSH_SHELL (object);
  PhoshShellPrivate *priv = phosh_shell_get_instance_private(self);

  switch (property_id) {
  case PROP_LOCKED:
    priv->locked = g_value_get_boolean (value);
    phosh_shell_set_state (self, PHOSH_STATE_LOCKED, priv->locked);
    break;
  case PROP_PRIMARY_MONITOR:
    phosh_shell_set_primary_monitor (self, g_value_get_object (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_shell_get_property (GObject *object,
                          guint property_id,
                          GValue *value,
                          GParamSpec *pspec)
{
  PhoshShell *self = PHOSH_SHELL (object);
  PhoshShellPrivate *priv = phosh_shell_get_instance_private(self);

  switch (property_id) {
  case PROP_LOCKED:
    g_value_set_boolean (value, priv->locked);
    break;
  case PROP_BUILTIN_MONITOR:
    g_value_set_object (value, phosh_shell_get_builtin_monitor (self));
    break;
  case PROP_PRIMARY_MONITOR:
    g_value_set_object (value, phosh_shell_get_primary_monitor (self));
    break;
  case PROP_SHELL_STATE:
    g_value_set_flags (value, priv->shell_state);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
phosh_shell_dispose (GObject *object)
{
  PhoshShell *self = PHOSH_SHELL (object);
  PhoshShellPrivate *priv = phosh_shell_get_instance_private(self);

  g_clear_handle_id (&priv->startup_finished_id, g_source_remove);

  panels_dispose (self);
  g_clear_pointer (&priv->faders, g_ptr_array_unref);

  g_clear_object (&priv->notification_banner);

  /* dispose managers in opposite order of declaration */
  g_clear_object (&priv->vpn_manager);
  g_clear_object (&priv->network_auth_manager);
  g_clear_object (&priv->run_command_manager);
  g_clear_object (&priv->splash_manager);
  g_clear_object (&priv->screenshot_manager);
  g_clear_object (&priv->calls_manager);
  g_clear_object (&priv->location_manager);
  g_clear_object (&priv->hks_manager);
  g_clear_object (&priv->gtk_mount_manager);
  g_clear_object (&priv->docked_manager);
  g_clear_object (&priv->mode_manager);
  g_clear_object (&priv->torch_manager);
  g_clear_object (&priv->gnome_shell_manager);
  g_clear_object (&priv->wwan);
  g_clear_object (&priv->mount_manager);
  g_clear_object (&priv->bt_manager);
  g_clear_object (&priv->feedback_manager);
  g_clear_object (&priv->notify_manager);
  g_clear_object (&priv->screen_saver_manager);
  g_clear_object (&priv->polkit_auth_agent);
  g_clear_object (&priv->wifi_manager);
  g_clear_object (&priv->toplevel_manager);
  g_clear_object (&priv->osk_manager);
  g_clear_object (&priv->idle_manager);
  g_clear_object (&priv->lockscreen_manager);
  g_clear_object (&priv->monitor_manager);
  g_clear_object (&priv->builtin_monitor);
  g_clear_object (&priv->primary_monitor);
  g_clear_object (&priv->background_manager);
  g_clear_object (&priv->keyboard_events);
  g_clear_object (&priv->app_tracker);

  /* sensors */
  g_clear_object (&priv->proximity);
  g_clear_object (&priv->rotation_manager);
  g_clear_object (&priv->sensor_proxy_manager);

  phosh_system_prompter_unregister ();
  g_clear_object (&priv->session_manager);

  g_clear_pointer (&priv->theme_name, g_free);
  g_clear_object (&priv->css_provider);

  G_OBJECT_CLASS (phosh_shell_parent_class)->dispose (object);
}


static void
on_num_toplevels_changed (PhoshShell *self, GParamSpec *pspec, PhoshToplevelManager *toplevel_manager)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (toplevel_manager));

  priv = phosh_shell_get_instance_private (self);
  /* all toplevels gone, show the overview */
  /* TODO: once we have unfoldable app-drawer unfold that too */
  if (!phosh_toplevel_manager_get_num_toplevels (toplevel_manager))
    phosh_home_set_state (PHOSH_HOME (priv->home), PHOSH_HOME_STATE_UNFOLDED);
}


static void
on_toplevel_added (PhoshShell *self, PhoshToplevel *unused, PhoshToplevelManager *toplevel_manager)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (toplevel_manager));

  priv = phosh_shell_get_instance_private (self);
  phosh_home_set_state (PHOSH_HOME (priv->home), PHOSH_HOME_STATE_FOLDED);
}


static void
on_new_notification (PhoshShell         *self,
                     PhoshNotification  *notification,
                     PhoshNotifyManager *manager)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_NOTIFICATION (notification));
  g_return_if_fail (PHOSH_IS_NOTIFY_MANAGER (manager));

  priv = phosh_shell_get_instance_private (self);

  /* Clear existing banner */
  if (priv->notification_banner && GTK_IS_WIDGET (priv->notification_banner)) {
    gtk_widget_destroy (priv->notification_banner);
  }

  if (phosh_notify_manager_get_show_notification_banner (manager, notification) &&
      phosh_top_panel_get_state (PHOSH_TOP_PANEL (priv->panel)) == PHOSH_TOP_PANEL_STATE_FOLDED &&
      !priv->locked) {
    g_set_weak_pointer (&priv->notification_banner,
                        phosh_notification_banner_new (notification));

    gtk_widget_show (GTK_WIDGET (priv->notification_banner));
  }
}


static gboolean
on_fade_out_timeout (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), G_SOURCE_REMOVE);

  priv = phosh_shell_get_instance_private (self);

  /* kill all faders if we time out */
  priv->faders = g_ptr_array_remove_range (priv->faders, 0, priv->faders->len);

  return G_SOURCE_REMOVE;
}


static void
notify_compositor_up_state (PhoshShell *self, enum phosh_private_shell_state state)
{
  struct phosh_private *phosh_private;

  g_debug ("Notify compositor state: %d", state);

  phosh_private = phosh_wayland_get_phosh_private (phosh_wayland_get_default ());
  if (phosh_private && phosh_private_get_version (phosh_private) >= PHOSH_PRIVATE_SHELL_READY_SINCE)
    phosh_private_set_shell_state (phosh_private, state);
}


static gboolean
on_startup_finished (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), G_SOURCE_REMOVE);
  priv = phosh_shell_get_instance_private (self);

  notify_compositor_up_state (self, PHOSH_PRIVATE_SHELL_STATE_UP);

  priv->startup_finished_id = 0;
  return G_SOURCE_REMOVE;
}


static gboolean
setup_idle_cb (PhoshShell *self)
{
  g_autoptr (GError) err = NULL;
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);

  priv->app_tracker = phosh_app_tracker_new ();
  priv->session_manager = phosh_session_manager_new ();
  priv->mode_manager = phosh_mode_manager_new ();

  priv->sensor_proxy_manager = phosh_sensor_proxy_manager_new (&err);
  if (!priv->sensor_proxy_manager)
    g_message ("Failed to connect to sensor-proxy: %s", err->message);

  panels_create (self);
  /* Create background after panel since it needs the panel's size */
  priv->background_manager = phosh_background_manager_new ();

  g_signal_connect_object (priv->toplevel_manager,
                           "notify::num-toplevels",
                           G_CALLBACK(on_num_toplevels_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_num_toplevels_changed (self, NULL, priv->toplevel_manager);

  g_signal_connect_object (priv->toplevel_manager,
                           "toplevel-added",
                           G_CALLBACK(on_toplevel_added),
                           self,
                           G_CONNECT_SWAPPED);

  /* Screen saver manager needs lock screen manager */
  priv->screen_saver_manager = phosh_screen_saver_manager_get_default (
    priv->lockscreen_manager);

  priv->notify_manager = phosh_notify_manager_get_default ();
  g_signal_connect_object (priv->notify_manager,
                           "new-notification",
                           G_CALLBACK (on_new_notification),
                           self,
                           G_CONNECT_SWAPPED);

  phosh_shell_get_location_manager (self);
  if (priv->sensor_proxy_manager) {
    priv->proximity = phosh_proximity_new (priv->sensor_proxy_manager,
                                           priv->calls_manager);
    phosh_monitor_manager_set_sensor_proxy_manager (priv->monitor_manager,
                                                    priv->sensor_proxy_manager);
  }

  priv->mount_manager = phosh_mount_manager_new ();
  priv->gtk_mount_manager = phosh_gtk_mount_manager_new ();

  phosh_session_manager_register (priv->session_manager,
                                  PHOSH_APP_ID,
                                  g_getenv ("DESKTOP_AUTOSTART_ID"));
  g_unsetenv ("DESKTOP_AUTOSTART_ID");

  priv->gnome_shell_manager = phosh_gnome_shell_manager_get_default ();
  priv->screenshot_manager = phosh_screenshot_manager_new ();
  priv->splash_manager = phosh_splash_manager_new (priv->app_tracker);
  priv->run_command_manager = phosh_run_command_manager_new();
  priv->network_auth_manager = phosh_network_auth_manager_new ();

  /* Delay signaling the compositor a bit so that idle handlers get a
   * chance to run and the user has can unlock right away. Ideally
   * we'd not need this */
  priv->startup_finished_id = g_timeout_add_seconds (1, (GSourceFunc)on_startup_finished, self);
  g_source_set_name_by_id (priv->startup_finished_id, "[phosh] startup finished");

  priv->startup_finished = TRUE;
  g_signal_emit (self, signals[READY], 0);

  return FALSE;
}


/* Load all types that might be used in UI files */
static void
type_setup (void)
{
  g_type_ensure (PHOSH_TYPE_BATTERY_INFO);
  g_type_ensure (PHOSH_TYPE_BT_INFO);
  g_type_ensure (PHOSH_TYPE_CONNECTIVITY_INFO);
  g_type_ensure (PHOSH_TYPE_DOCKED_INFO);
  g_type_ensure (PHOSH_TYPE_FEEDBACK_INFO);
  g_type_ensure (PHOSH_TYPE_HKS_INFO);
  g_type_ensure (PHOSH_TYPE_LOCATION_INFO);
  g_type_ensure (PHOSH_TYPE_MEDIA_PLAYER);
  g_type_ensure (PHOSH_TYPE_QUICK_SETTING);
  g_type_ensure (PHOSH_TYPE_ROTATE_INFO);
  g_type_ensure (PHOSH_TYPE_SETTINGS);
  g_type_ensure (PHOSH_TYPE_SYSTEM_MODAL);
  g_type_ensure (PHOSH_TYPE_SYSTEM_MODAL_DIALOG);
  g_type_ensure (PHOSH_TYPE_TORCH_INFO);
  g_type_ensure (PHOSH_TYPE_VPN_INFO);
  g_type_ensure (PHOSH_TYPE_WIFI_INFO);
  g_type_ensure (PHOSH_TYPE_WWAN_INFO);
}


static void
on_builtin_monitor_power_mode_changed (PhoshShell *self, GParamSpec *pspec, PhoshMonitor *monitor)
{
  PhoshMonitorPowerSaveMode mode;
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_MONITOR (monitor));
  priv = phosh_shell_get_instance_private (self);

  g_object_get (monitor, "power-mode", &mode, NULL);
  /* Might be emitted on startup before lockscreen_manager is up */
  if (mode == PHOSH_MONITOR_POWER_SAVE_MODE_OFF && priv->lockscreen_manager)
    phosh_shell_lock (self);

  phosh_shell_set_state (self, PHOSH_STATE_BLANKED, mode == PHOSH_MONITOR_POWER_SAVE_MODE_OFF);
}

static void
phosh_shell_set_builtin_monitor (PhoshShell *self, PhoshMonitor *monitor)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_MONITOR (monitor) || monitor == NULL);
  priv = phosh_shell_get_instance_private (self);

  if (priv->builtin_monitor == monitor)
    return;

  if (priv->builtin_monitor) {
    /* Power mode listener */
    g_signal_handlers_disconnect_by_data (priv->builtin_monitor, self);
    g_clear_object (&priv->builtin_monitor);

    if (priv->rotation_manager)
      phosh_rotation_manager_set_monitor (priv->rotation_manager, NULL);
  }

  g_debug ("New builtin monitor is %s", monitor ? monitor->name : "(none)");
  g_set_object (&priv->builtin_monitor, monitor);

  if (monitor) {
    g_signal_connect_swapped (priv->builtin_monitor,
                              "notify::power-mode",
                              G_CALLBACK (on_builtin_monitor_power_mode_changed),
                              self);

    if (priv->rotation_manager)
      phosh_rotation_manager_set_monitor (priv->rotation_manager, monitor);
  }

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BUILTIN_MONITOR]);
}


/* Find a new builtin monitor that differs from old, otherwise NULL */
static PhoshMonitor *
find_new_builtin_monitor (PhoshShell *self, PhoshMonitor *old)
{
  PhoshShellPrivate *priv;
  PhoshMonitor *monitor = NULL;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  for (int i = 0; i < phosh_monitor_manager_get_num_monitors (priv->monitor_manager); i++) {
    PhoshMonitor *tmp = phosh_monitor_manager_get_monitor (priv->monitor_manager, i);
    if (phosh_monitor_is_builtin (tmp) && tmp != old) {
      monitor = tmp;
      break;
    }
  }

  return monitor;
}


static void
on_monitor_added (PhoshShell *self, PhoshMonitor *monitor)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_MONITOR (monitor));
  priv = phosh_shell_get_instance_private (self);

  g_debug ("Monitor %p (%s) added", monitor, monitor->name);

  /* Set built-in monitor if not set already */
  if (!priv->builtin_monitor && phosh_monitor_is_builtin (monitor))
    phosh_shell_set_builtin_monitor (self, monitor);

  /*
   * on_monitor_added() gets connected in phosh_shell_constructed() but
   * we can't use phosh_shell_set_primary_monitor() yet since the
   * shell object is not yet up and we can't move panels, etc. so
   * ignore that case. This is not a problem since phosh_shell_constructed()
   * sets the primary monitor explicitly.
   */
  if (!priv->startup_finished)
    return;

  /* Set primary monitor if unset */
  if (priv->primary_monitor == NULL)
    phosh_shell_set_primary_monitor (self, monitor);
}


static void
on_monitor_removed (PhoshShell *self, PhoshMonitor *monitor)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  g_return_if_fail (PHOSH_IS_MONITOR (monitor));
  priv = phosh_shell_get_instance_private (self);

  if (priv->builtin_monitor == monitor) {
    PhoshMonitor *new_builtin;

    g_debug ("Builtin monitor %p (%s) removed", monitor, monitor->name);

    new_builtin = find_new_builtin_monitor (self, monitor);
    phosh_shell_set_builtin_monitor (self, new_builtin);
  }

  if (priv->primary_monitor == monitor) {
    g_debug ("Primary monitor %p (%s) removed", monitor, monitor->name);

    /* Prefer built in monitor when primary is gone... */
    if (priv->builtin_monitor) {
      phosh_shell_set_primary_monitor (self, priv->builtin_monitor);
      return;
    }

    /* ...just pick the first one available otherwise */
    for (int i = 0; i < phosh_monitor_manager_get_num_monitors (priv->monitor_manager); i++) {
      PhoshMonitor *new_primary = phosh_monitor_manager_get_monitor (priv->monitor_manager, i);
      if (new_primary != monitor) {
        phosh_shell_set_primary_monitor (self, new_primary);
        break;
      }
    }

    /* We did not find another monitor so all monitors are gone */
    if (priv->primary_monitor == monitor) {
      g_debug ("All monitors gone");
      phosh_shell_set_primary_monitor (self, NULL);
      return;
    }
  }
}


static void
phosh_shell_constructed (GObject *object)
{
  PhoshShell *self = PHOSH_SHELL (object);
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);

  G_OBJECT_CLASS (phosh_shell_parent_class)->constructed (object);

  /* We bind this early since a wl_display_roundtrip () would make us miss
     existing toplevels */
  priv->toplevel_manager = phosh_toplevel_manager_new ();

  priv->monitor_manager = phosh_monitor_manager_new (NULL);
  g_signal_connect_swapped (priv->monitor_manager,
                            "monitor-added",
                            G_CALLBACK (on_monitor_added),
                            self);
  g_signal_connect_swapped (priv->monitor_manager,
                            "monitor-removed",
                            G_CALLBACK (on_monitor_removed),
                            self);

  /* Make sure all outputs are up to date */
  phosh_wayland_roundtrip (phosh_wayland_get_default ());

  if (phosh_monitor_manager_get_num_monitors (priv->monitor_manager)) {
    PhoshMonitor *monitor = find_new_builtin_monitor (self, NULL);

    /* Setup builtin monitor if not set via 'monitor-added' */
    if (!priv->builtin_monitor && monitor) {
      phosh_shell_set_builtin_monitor (self, monitor);
      g_debug ("Builtin monitor %p, configured: %d",
               priv->builtin_monitor,
               phosh_monitor_is_configured (priv->builtin_monitor));
    }

    /* Setup primary monitor, prefer builtin */
    /* Can't invoke phosh_shell_set_primary_monitor () since this involves
       updating the panels as well and we need to init more of the shell first */
    if (priv->builtin_monitor)
      priv->primary_monitor = g_object_ref (priv->builtin_monitor);
    else
      priv->primary_monitor = g_object_ref (phosh_monitor_manager_get_monitor (priv->monitor_manager, 0));
  } else {
    g_error ("Need at least one monitor");
  }

  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_default (),
                                    "/sm/puri/phosh/icons");

  priv->calls_manager = phosh_calls_manager_new ();
  priv->lockscreen_manager = phosh_lockscreen_manager_new (priv->calls_manager);
  g_object_bind_property (priv->lockscreen_manager, "locked",
                          self, "locked",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  priv->idle_manager = phosh_idle_manager_get_default();

  priv->faders = g_ptr_array_new_with_free_func ((GDestroyNotify) (gtk_widget_destroy));

  phosh_system_prompter_register ();
  priv->polkit_auth_agent = phosh_polkit_auth_agent_new ();

  priv->feedback_manager = phosh_feedback_manager_new ();
  priv->keyboard_events = phosh_keyboard_events_new ();

  g_idle_add ((GSourceFunc) setup_idle_cb, self);
}


static void
phosh_shell_class_init (PhoshShellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = phosh_shell_constructed;
  object_class->dispose = phosh_shell_dispose;

  object_class->set_property = phosh_shell_set_property;
  object_class->get_property = phosh_shell_get_property;

  type_setup ();

  props[PROP_LOCKED] =
    g_param_spec_boolean ("locked",
                          "Locked",
                          "Whether the screen is locked",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * PhoshShell:builtin-monitor:
   *
   * The built in monitor. This is a hardware property and hence can
   * only be read. It can be %NULL when not present or disabled.
   */
  props[PROP_BUILTIN_MONITOR] =
    g_param_spec_object ("builtin-monitor",
                         "Built in monitor",
                         "The builtin monitor",
                         PHOSH_TYPE_MONITOR,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PhoshShell:primary-monitor:
   *
   * The primary monitor that has the panels, lock screen etc.
   */
  props[PROP_PRIMARY_MONITOR] =
    g_param_spec_object ("primary-monitor",
                         "Primary monitor",
                         "The primary monitor",
                         PHOSH_TYPE_MONITOR,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_SHELL_STATE] =
    g_param_spec_flags ("shell-state",
                        "Shell state",
                        "The state of the shell",
                        PHOSH_TYPE_SHELL_STATE_FLAGS,
                        PHOSH_STATE_NONE,
                        G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  signals[READY] = g_signal_new ("ready",
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST, 0,
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}


static GDebugKey debug_keys[] =
{
 { .key = "always-splash",
   .value = PHOSH_SHELL_DEBUG_FLAG_ALWAYS_SPLASH,
 },
};


static void
phosh_shell_init (PhoshShell *self)
{
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);
  GtkSettings *gtk_settings;

  priv->debug_flags = g_parse_debug_string(g_getenv ("PHOSH_DEBUG"),
                                           debug_keys,
                                           G_N_ELEMENTS (debug_keys));

  gtk_settings = gtk_settings_get_default ();
  g_object_set (G_OBJECT (gtk_settings), "gtk-application-prefer-dark-theme", TRUE, NULL);

  g_signal_connect_swapped (gtk_settings, "notify::gtk-theme-name", G_CALLBACK (on_gtk_theme_name_changed), self);
  on_gtk_theme_name_changed (self, NULL, gtk_settings);

  priv->shell_state = PHOSH_STATE_NONE;
}


static gboolean
select_fallback_monitor (gpointer data)
{
  PhoshShell *self = PHOSH_SHELL (data);
  PhoshShellPrivate *priv = phosh_shell_get_instance_private (self);

  g_return_val_if_fail (PHOSH_IS_MONITOR_MANAGER (priv->monitor_manager), FALSE);
  phosh_monitor_manager_enable_fallback (priv->monitor_manager);

  return G_SOURCE_REMOVE;
}


void
phosh_shell_set_primary_monitor (PhoshShell *self, PhoshMonitor *monitor)
{
  PhoshShellPrivate *priv;
  PhoshMonitor *m = NULL;
  gboolean needs_notify = FALSE;

  g_return_if_fail (PHOSH_IS_MONITOR (monitor) || monitor == NULL);
  g_return_if_fail (PHOSH_IS_SHELL (self));
  priv = phosh_shell_get_instance_private (self);

  if (monitor == priv->primary_monitor)
    return;

  if (monitor != NULL) {
    /* Make sure the new monitor exists */
    for (int i = 0; i < phosh_monitor_manager_get_num_monitors (priv->monitor_manager); i++) {
      m = phosh_monitor_manager_get_monitor (priv->monitor_manager, i);
      if (monitor == m)
        break;
    }
    g_return_if_fail (monitor == m);
  }

  needs_notify = priv->primary_monitor == NULL;
  g_set_object (&priv->primary_monitor, monitor);
  g_debug ("New primary monitor is %s", monitor ? monitor->name : "(none)");

  /* Move panels to the new monitor by recreating the layer-shell surfaces */
  panels_dispose (self);
  if (monitor)
    panels_create (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PRIMARY_MONITOR]);

  /* All monitors gone or disabled. See if monitor-manager finds a
   * fallback to enable. Do that in an idle callback so GTK can process
   * pending wayland events for the gone output */
  if (monitor == NULL) {
    /* No monitor we're not useful atm */
    notify_compositor_up_state (self, PHOSH_PRIVATE_SHELL_STATE_UNKNOWN);
    g_idle_add (select_fallback_monitor, self);
  } else {
    if (needs_notify)
      notify_compositor_up_state (self, PHOSH_PRIVATE_SHELL_STATE_UP);
  }
}


PhoshMonitor *
phosh_shell_get_builtin_monitor (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_MONITOR (priv->builtin_monitor) || priv->builtin_monitor == NULL, NULL);

  return priv->builtin_monitor;
}


/**
 * phosh_shell_get_primary_monitor:
 * @self: The shell
 *
 * Returns: the primary monitor or %NULL if there currently are no outputs
 */
PhoshMonitor *
phosh_shell_get_primary_monitor (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  return priv->primary_monitor;
}

/* Manager getters */

PhoshAppTracker *
phosh_shell_get_app_tracker (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_APP_TRACKER (priv->app_tracker), NULL);

  return priv->app_tracker;
}


PhoshBackgroundManager *
phosh_shell_get_background_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_BACKGROUND_MANAGER (priv->background_manager), NULL);

  return priv->background_manager;
}


PhoshCallsManager *
phosh_shell_get_calls_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_CALLS_MANAGER (priv->calls_manager), NULL);

  return priv->calls_manager;
}


PhoshFeedbackManager *
phosh_shell_get_feedback_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_FEEDBACK_MANAGER (priv->feedback_manager), NULL);

  return priv->feedback_manager;
}


PhoshGtkMountManager *
phosh_shell_get_gtk_mount_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_GTK_MOUNT_MANAGER (priv->gtk_mount_manager), NULL);

  return priv->gtk_mount_manager;
}


PhoshLockscreenManager *
phosh_shell_get_lockscreen_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  g_return_val_if_fail (PHOSH_IS_LOCKSCREEN_MANAGER (priv->lockscreen_manager), NULL);
  return priv->lockscreen_manager;
}


PhoshModeManager *
phosh_shell_get_mode_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  g_return_val_if_fail (PHOSH_IS_MODE_MANAGER (priv->mode_manager), NULL);
  return priv->mode_manager;
}


PhoshMonitorManager *
phosh_shell_get_monitor_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  g_return_val_if_fail (PHOSH_IS_MONITOR_MANAGER (priv->monitor_manager), NULL);
  return priv->monitor_manager;
}


PhoshToplevelManager *
phosh_shell_get_toplevel_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  g_return_val_if_fail (PHOSH_IS_TOPLEVEL_MANAGER (priv->toplevel_manager), NULL);
  return priv->toplevel_manager;
}


PhoshSessionManager *
phosh_shell_get_session_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_SESSION_MANAGER (priv->session_manager), NULL);

  return priv->session_manager;
}

/* Manager getters that create them as needed */

PhoshBtManager *
phosh_shell_get_bt_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->bt_manager)
      priv->bt_manager = phosh_bt_manager_new ();

  g_return_val_if_fail (PHOSH_IS_BT_MANAGER (priv->bt_manager), NULL);
  return priv->bt_manager;
}


PhoshDockedManager *
phosh_shell_get_docked_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->docked_manager)
    priv->docked_manager = phosh_docked_manager_new (priv->mode_manager);

  g_return_val_if_fail (PHOSH_IS_DOCKED_MANAGER (priv->docked_manager), NULL);
  return priv->docked_manager;
}


PhoshHksManager *
phosh_shell_get_hks_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->hks_manager)
    priv->hks_manager = phosh_hks_manager_new ();

  g_return_val_if_fail (PHOSH_IS_HKS_MANAGER (priv->hks_manager), NULL);
  return priv->hks_manager;
}


PhoshLocationManager *
phosh_shell_get_location_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->location_manager)
    priv->location_manager = phosh_location_manager_new ();

  g_return_val_if_fail (PHOSH_IS_LOCATION_MANAGER (priv->location_manager), NULL);
  return priv->location_manager;
}


PhoshOskManager *
phosh_shell_get_osk_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->osk_manager)
      priv->osk_manager = phosh_osk_manager_new ();

  g_return_val_if_fail (PHOSH_IS_OSK_MANAGER (priv->osk_manager), NULL);
  return priv->osk_manager;
}


PhoshRotationManager *
phosh_shell_get_rotation_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->rotation_manager)
    priv->rotation_manager = phosh_rotation_manager_new (priv->sensor_proxy_manager,
                                                         priv->lockscreen_manager,
                                                         priv->builtin_monitor);

  g_return_val_if_fail (PHOSH_IS_ROTATION_MANAGER (priv->rotation_manager), NULL);

  return priv->rotation_manager;
}


PhoshTorchManager *
phosh_shell_get_torch_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->torch_manager)
    priv->torch_manager = phosh_torch_manager_new ();

  g_return_val_if_fail (PHOSH_IS_TORCH_MANAGER (priv->torch_manager), NULL);
  return priv->torch_manager;
}


PhoshVpnManager *
phosh_shell_get_vpn_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->vpn_manager)
      priv->vpn_manager = phosh_vpn_manager_new ();

  g_return_val_if_fail (PHOSH_IS_VPN_MANAGER (priv->vpn_manager), NULL);
  return priv->vpn_manager;
}


PhoshWifiManager *
phosh_shell_get_wifi_manager (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->wifi_manager)
      priv->wifi_manager = phosh_wifi_manager_new ();

  g_return_val_if_fail (PHOSH_IS_WIFI_MANAGER (priv->wifi_manager), NULL);
  return priv->wifi_manager;
}


PhoshWWan *
phosh_shell_get_wwan (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  if (!priv->wwan) {
    g_autoptr (GSettings) settings = g_settings_new ("sm.puri.phosh");
    PhoshWWanBackend backend = g_settings_get_enum (settings, WWAN_BACKEND_KEY);

    switch (backend) {
      default:
      case PHOSH_WWAN_BACKEND_MM:
        priv->wwan = PHOSH_WWAN (phosh_wwan_mm_new());
        break;
      case PHOSH_WWAN_BACKEND_OFONO:
        priv->wwan = PHOSH_WWAN (phosh_wwan_ofono_new());
        break;
    }
  }

  g_return_val_if_fail (PHOSH_IS_WWAN (priv->wwan), NULL);
  return priv->wwan;
}

/**
 * phosh_shell_get_usable_area:
 * @self: The shell
 * @x:(out)(nullable): The x coordinate where client usable area starts
 * @y:(out)(nullable): The y coordinate where client usable area starts
 * @width:(out)(nullable): The width of the client usable area
 * @height:(out)(nullable): The height of the client usable area
 *
 * Returns the usable area in pixels usable by a client on the phone
 * display.
 */
void
phosh_shell_get_usable_area (PhoshShell *self, int *x, int *y, int *width, int *height)
{
  PhoshMonitor *monitor;
  PhoshMonitorMode *mode;
  int w, h;
  float scale;

  g_return_if_fail (PHOSH_IS_SHELL (self));

  monitor = phosh_shell_get_primary_monitor (self);
  g_return_if_fail(monitor);
  mode = phosh_monitor_get_current_mode (monitor);
  g_return_if_fail (mode != NULL);

  scale = MAX(1.0, phosh_monitor_get_fractional_scale (monitor));

  g_debug ("Primary monitor %p scale is %f, mode: %dx%d, transform is %d",
           monitor,
           scale,
           mode->width,
           mode->height,
           monitor->transform);

  switch (phosh_monitor_get_transform(monitor)) {
  case PHOSH_MONITOR_TRANSFORM_NORMAL:
  case PHOSH_MONITOR_TRANSFORM_180:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED:
  case PHOSH_MONITOR_TRANSFORM_FLIPPED_180:
    w = mode->width / scale;
    h = mode->height / scale - PHOSH_TOP_PANEL_HEIGHT - PHOSH_HOME_BUTTON_HEIGHT;
    break;
  default:
    w = mode->height / scale;
    h = mode->width / scale - PHOSH_TOP_PANEL_HEIGHT - PHOSH_HOME_BUTTON_HEIGHT;
    break;
  }

  if (x)
    *x = 0;
  if (y)
    *y = PHOSH_TOP_PANEL_HEIGHT;
  if (width)
    *width = w;
  if (height)
    *height = h;
}


PhoshShell *
phosh_shell_get_default (void)
{
  static PhoshShell *instance;

  if (instance == NULL) {
    g_debug("Creating shell");
    instance = g_object_new (PHOSH_TYPE_SHELL, NULL);
    g_object_add_weak_pointer (G_OBJECT (instance), (gpointer *)&instance);
  }
  return instance;
}

void
phosh_shell_fade_out (PhoshShell *self, guint timeout)
{
  PhoshShellPrivate *priv;
  PhoshMonitorManager *monitor_manager;

  g_debug ("Fading out...");
  g_return_if_fail (PHOSH_IS_SHELL (self));
  monitor_manager = phosh_shell_get_monitor_manager (self);
  g_return_if_fail (PHOSH_IS_MONITOR_MANAGER (monitor_manager));
  priv = phosh_shell_get_instance_private (self);

  for (int i = 0; i < phosh_monitor_manager_get_num_monitors (monitor_manager); i++) {
    PhoshFader *fader;
    PhoshMonitor *monitor = phosh_monitor_manager_get_monitor (monitor_manager, i);

    fader = phosh_fader_new (monitor);
    g_ptr_array_add (priv->faders, fader);
    gtk_widget_show (GTK_WIDGET (fader));
    if (timeout > 0)
      g_timeout_add_seconds (timeout, (GSourceFunc) on_fade_out_timeout, self);
  }
}

/**
 * phosh_shell_set_power_save:
 * @self: The shell
 * @enable: Wether power save mode should be enabled
 *
 * Enter power saving mode. This currently blanks all monitors.
 */
void
phosh_shell_enable_power_save (PhoshShell *self, gboolean enable)
{
  g_debug ("Entering power save mode");
  g_return_if_fail (PHOSH_IS_SHELL (self));

  /*
   * Locking the outputs instructs g-s-d to tell us to put
   * outputs into power save mode via org.gnome.Mutter.DisplayConfig
   */
  phosh_shell_set_locked(self, enable);

  /* TODO: other means of power saving */
}

/**
 * phosh_shell_started_by_display_manager:
 * @self: The shell
 *
 * Returns: %TRUE if we were started from a display manager. %FALSE otherwise.
 */
gboolean
phosh_shell_started_by_display_manager(PhoshShell *self)
{
  g_return_val_if_fail (PHOSH_IS_SHELL (self), FALSE);

  if (!g_strcmp0 (g_getenv ("GDMSESSION"), "phosh"))
    return TRUE;

  return FALSE;
}

/**
 * phosh_shell_is_startup_finished:
 * @self: The shell
 *
 * Returns: %TRUE if the shell finished startup. %FALSE otherwise.
 */
gboolean
phosh_shell_is_startup_finished(PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), FALSE);
  priv = phosh_shell_get_instance_private (self);

  return priv->startup_finished;
}


void
phosh_shell_add_global_keyboard_action_entries (PhoshShell *self,
                                                const GActionEntry *entries,
                                                gint n_entries,
                                                gpointer user_data)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  priv = phosh_shell_get_instance_private (self);
  g_return_if_fail (priv->keyboard_events);

  g_action_map_add_action_entries (G_ACTION_MAP (priv->keyboard_events),
                                   entries,
                                   n_entries,
                                   user_data);
}


void
phosh_shell_remove_global_keyboard_action_entries (PhoshShell *self,
                                                   GStrv       action_names)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  priv = phosh_shell_get_instance_private (self);
  g_return_if_fail (priv->keyboard_events);

  for (int i = 0; i < g_strv_length (action_names); i++) {
    g_action_map_remove_action (G_ACTION_MAP (priv->keyboard_events),
                                action_names[i]);
  }
}


/**
 * phosh_shell_is_session_active
 * @self: The shell
 *
 * Whether this shell is part of the active session
 */
gboolean
phosh_shell_is_session_active (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), FALSE);
  priv = phosh_shell_get_instance_private (self);

  return phosh_session_manager_is_active (priv->session_manager);
}


/**
 * phosh_get_app_launch_context:
 * @self: The shell
 *
 * Returns: an app launch context for the primary display
 */
GdkAppLaunchContext*
phosh_shell_get_app_launch_context (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), NULL);
  priv = phosh_shell_get_instance_private (self);

  return gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (priv->panel)));
}

/**
 * phosh_shell_get_state
 * @self: The shell
 *
 * Returns: The current #PhoshShellStateFlags
 */
PhoshShellStateFlags
phosh_shell_get_state (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), PHOSH_STATE_NONE);
  priv = phosh_shell_get_instance_private (self);

  return priv->shell_state;
}

/**
 * phosh_shell_set_state:
 * @self: The shell
 * @state: The #PhoshShellStateFlags to set
 * @enabled: %TRUE to set a shell state, %FALSE to reset
 *
 * Set the shells state.
 */
void
phosh_shell_set_state (PhoshShell          *self,
                       PhoshShellStateFlags state,
                       gboolean             enabled)
{
  PhoshShellPrivate *priv;
  PhoshShellStateFlags old_state;
  g_autofree gchar *str_state = NULL;
  g_autofree gchar *str_new_flags = NULL;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  priv = phosh_shell_get_instance_private (self);

  old_state = priv->shell_state;

  if (enabled)
    priv->shell_state = priv->shell_state | state;
  else
    priv->shell_state = priv->shell_state & ~state;

  if (old_state == priv->shell_state)
    return;

  str_state = g_flags_to_string (PHOSH_TYPE_SHELL_STATE_FLAGS,
                                 state);
  str_new_flags = g_flags_to_string (PHOSH_TYPE_SHELL_STATE_FLAGS,
                                     priv->shell_state);

  g_debug ("%s %s shells state. New state: %s",
           enabled ? "Adding to" : "Removing from",
           str_state, str_new_flags);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHELL_STATE]);
}

void
phosh_shell_lock (PhoshShell *self)
{
  g_return_if_fail (PHOSH_IS_SHELL (self));

  phosh_shell_set_locked (self, TRUE);
}


void
phosh_shell_unlock (PhoshShell *self)
{
  g_return_if_fail (PHOSH_IS_SHELL (self));

  phosh_shell_set_locked (self, FALSE);
}

/**
 * phosh_shell_get_locked:
 * @self: The #PhoshShell singleton
 *
 * Returns: %TRUE if the shell is currently locked, otherwise %FALSE.
 */
gboolean
phosh_shell_get_locked (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), FALSE);
  priv = phosh_shell_get_instance_private (self);

  return priv->locked;
}

/**
 * phosh_shell_set_locked:
 * @self: The #PhoshShell singleton
 * @locked: %TRUE to lock the shell
 *
 * Lock the shell. We proxy to lockscreen-manager to avoid
 * that other parts of the shell need to care about this
 * abstraction.
 */
void
phosh_shell_set_locked (PhoshShell *self, gboolean locked)
{
  PhoshShellPrivate *priv;

  g_return_if_fail (PHOSH_IS_SHELL (self));
  priv = phosh_shell_get_instance_private (self);

  if (locked == priv->locked)
    return;

  phosh_lockscreen_manager_set_locked (priv->lockscreen_manager, locked);
}


/**
 * phosh_shell_get_show_splash:
 * @self: The #PhoshShell singleton
 *
 * Whether splash screens should be used when apps start
 * Returns: %TRUE when splash should be used, otherwise %FALSE
 */
gboolean
phosh_shell_get_show_splash (PhoshShell *self)
{
  PhoshShellPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_SHELL (self), TRUE);
  priv = phosh_shell_get_instance_private (self);
  g_return_val_if_fail (PHOSH_IS_DOCKED_MANAGER (priv->docked_manager), TRUE);

  if (priv->debug_flags & PHOSH_SHELL_DEBUG_FLAG_ALWAYS_SPLASH)
    return TRUE;

  if (phosh_docked_manager_get_enabled (priv->docked_manager))
    return FALSE;

  return TRUE;
}
