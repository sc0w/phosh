/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-settings"

#include <glib/gi18n.h>

#include "media-player.h"
#include "mode-manager.h"
#include "shell.h"
#include "settings.h"
#include "quick-setting.h"
#include "settings/brightness.h"
#include "settings/gvc-channel-bar.h"
#include "torch-info.h"
#include "torch-manager.h"
#include "vpn-manager.h"
#include "wwan/wwan-manager.h"
#include "notifications/notify-manager.h"
#include "notifications/notification-frame.h"
#include "rotateinfo.h"

#include <pulse/pulseaudio.h>
#include "gvc-mixer-control.h"
#include "gvc-mixer-stream.h"
#include <gio/gdesktopappinfo.h>
#include <xkbcommon/xkbcommon.h>

#include <math.h>

/**
 * SECTION:settings
 * @short_description: The settings menu
 * @Title: PhoshSettings
 */

enum {
  SETTING_DONE,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

typedef struct _PhoshSettings
{
  GtkBin parent;

  GtkWidget *box_settings;
  GtkWidget *quick_settings;
  GtkWidget *scale_brightness;
  GtkWidget *output_vol_bar;
  GtkWidget *media_player;

  /* Output volume control */
  GvcMixerControl *mixer_control;
  GvcMixerStream *output_stream;
  gboolean allow_volume_above_100_percent;
  gboolean setting_volume;
  gboolean is_headphone;

  /* Notifications */
  GtkWidget *list_notifications;
  GtkWidget *box_notifications;

  /* Torch */
  PhoshTorchManager *torch_manager;
  GtkWidget *scale_torch;
  gboolean setting_torch;
} PhoshSettings;


G_DEFINE_TYPE (PhoshSettings, phosh_settings, GTK_TYPE_BIN)


static void
close_settings_menu (PhoshSettings *self)
{
  g_signal_emit (self, signals[SETTING_DONE], 0);
  phosh_trigger_feedback ("button-pressed");
}

static void
brightness_value_changed_cb (GtkScale *scale_brightness, gpointer unused)
{
  int brightness;

  brightness = (int)gtk_range_get_value (GTK_RANGE (scale_brightness));
  brightness_set (brightness);
}

static void
rotation_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshRotationManager *rotation_manager;
  PhoshRotationManagerMode mode;
  PhoshMonitorTransform transform;
  gboolean locked;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));

  rotation_manager = phosh_shell_get_rotation_manager (shell);
  g_return_if_fail (rotation_manager);
  mode = phosh_rotation_manager_get_mode (PHOSH_ROTATION_MANAGER (rotation_manager));

  switch (mode) {
  case PHOSH_ROTATION_MANAGER_MODE_OFF:
    transform = phosh_rotation_manager_get_transform (rotation_manager) ?
      PHOSH_MONITOR_TRANSFORM_NORMAL : PHOSH_MONITOR_TRANSFORM_270;
    phosh_rotation_manager_set_transform (rotation_manager, transform);
    g_signal_emit (self, signals[SETTING_DONE], 0);
    break;
  case PHOSH_ROTATION_MANAGER_MODE_SENSOR:
    locked = phosh_rotation_manager_get_orientation_locked (rotation_manager);
    phosh_rotation_manager_set_orientation_locked (rotation_manager, !locked);
    break;
  default:
    g_assert_not_reached ();
  }
}

static void
rotation_setting_long_pressed_cb (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshRotateInfoMode mode;
  PhoshRotationManager *rotation_manager;

  rotation_manager = phosh_shell_get_rotation_manager (shell);
  g_return_if_fail (rotation_manager);

  mode = phosh_rotation_manager_get_mode (rotation_manager);
  switch (mode) {
  case PHOSH_ROTATION_MANAGER_MODE_OFF:
    mode = PHOSH_ROTATION_MANAGER_MODE_SENSOR;
    break;
  case PHOSH_ROTATION_MANAGER_MODE_SENSOR:
    mode = PHOSH_ROTATION_MANAGER_MODE_OFF;
    break;
  default:
    g_assert_not_reached ();
  }
  g_debug ("Rotation manager mode: %d", mode);
  phosh_rotation_manager_set_mode (rotation_manager, mode);
}

static void
feedback_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell;
  PhoshFeedbackManager *manager;

  shell = phosh_shell_get_default ();
  g_return_if_fail (PHOSH_IS_SHELL (shell));
  manager = phosh_shell_get_feedback_manager (shell);
  g_return_if_fail (PHOSH_IS_FEEDBACK_MANAGER (manager));
  phosh_feedback_manager_toggle (manager);
}

static void
wifi_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshWifiManager *manager;
  gboolean enabled;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));

  manager = phosh_shell_get_wifi_manager (shell);
  g_return_if_fail (PHOSH_IS_WIFI_MANAGER (manager));

  enabled = phosh_wifi_manager_get_enabled (manager);
  phosh_wifi_manager_set_enabled (manager, !enabled);
}

static void
wifi_setting_long_pressed_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("wifi");
  close_settings_menu (self);
}

static void
wwan_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshWWan *wwan;
  gboolean enabled;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));

  wwan = phosh_shell_get_wwan (shell);
  g_return_if_fail (PHOSH_IS_WWAN (wwan));

  enabled = phosh_wwan_is_enabled (wwan);
  phosh_wwan_set_enabled (wwan, !enabled);
}

static void
wwan_setting_long_pressed_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("wwan");
  close_settings_menu (self);
}

static void
bt_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshBtManager *manager;
  gboolean enabled;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));

  manager = phosh_shell_get_bt_manager (shell);
  g_return_if_fail (PHOSH_IS_BT_MANAGER (manager));

  enabled = phosh_bt_manager_get_enabled (manager);
  phosh_bt_manager_set_enabled (manager, !enabled);
}

static void
bt_setting_long_pressed_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("bluetooth");
  close_settings_menu (self);
}

static void
feedback_setting_long_pressed_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("notifications");
  close_settings_menu (self);
}

static void
battery_setting_clicked_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("power");
  close_settings_menu (self);
}


static void
torch_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell;
  PhoshTorchManager *manager;

  shell = phosh_shell_get_default ();
  g_return_if_fail (PHOSH_IS_SHELL (shell));
  manager = phosh_shell_get_torch_manager (shell);
  g_return_if_fail (PHOSH_IS_TORCH_MANAGER (manager));
  phosh_torch_manager_toggle (manager);
}


static void
docked_setting_clicked_cb (PhoshSettings *self)
{
  PhoshShell *shell;
  PhoshDockedManager *manager;
  gboolean enabled;

  shell = phosh_shell_get_default ();
  g_return_if_fail (PHOSH_IS_SHELL (shell));
  manager = phosh_shell_get_docked_manager (shell);
  g_return_if_fail (PHOSH_IS_DOCKED_MANAGER (manager));

  enabled = phosh_docked_manager_get_enabled (manager);
  phosh_docked_manager_set_enabled (manager, !enabled);
}

static void
docked_setting_long_pressed_cb (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("display");
  close_settings_menu (self);
}


static void
update_output_vol_bar (PhoshSettings *self)
{
  GtkAdjustment *adj;

  self->setting_volume = TRUE;
  gvc_channel_bar_set_base_volume (GVC_CHANNEL_BAR (self->output_vol_bar),
                                   gvc_mixer_stream_get_base_volume (self->output_stream));
  gvc_channel_bar_set_is_amplified (GVC_CHANNEL_BAR (self->output_vol_bar),
                                    self->allow_volume_above_100_percent &&
                                    gvc_mixer_stream_get_can_decibel (self->output_stream));
  adj = GTK_ADJUSTMENT (gvc_channel_bar_get_adjustment (GVC_CHANNEL_BAR (self->output_vol_bar)));
  g_debug ("Adjusting volume to %d", gvc_mixer_stream_get_volume (self->output_stream));
  gtk_adjustment_set_value (adj, gvc_mixer_stream_get_volume (self->output_stream));
  self->setting_volume = FALSE;
}


static void
on_vpn_setting_clicked (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();
  PhoshVpnManager *vpn_manager;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));
  g_return_if_fail (PHOSH_IS_SHELL (shell));

  g_debug ("Toggling VPN connection");

  vpn_manager = phosh_shell_get_vpn_manager (shell);
  phosh_vpn_manager_toggle_last_connection (vpn_manager);
}


static void
on_vpn_setting_long_pressed (PhoshSettings *self)
{
  phosh_quick_setting_open_settings_panel ("network");
  close_settings_menu (self);
}


static void
output_stream_notify_is_muted_cb (GvcMixerStream *stream, GParamSpec *pspec, gpointer data)
{
  PhoshSettings *self = PHOSH_SETTINGS (data);
  gboolean muted;

  muted = gvc_mixer_stream_get_is_muted (stream);
  if (!self->setting_volume) {
    gvc_channel_bar_set_is_muted (GVC_CHANNEL_BAR (self->output_vol_bar), muted);
    if (!muted)
      update_output_vol_bar (self);
  };

}


static void
output_stream_notify_volume_cb (GvcMixerStream *stream, GParamSpec *pspec, gpointer data)
{
  PhoshSettings *self = PHOSH_SETTINGS (data);

  if (!self->setting_volume)
    update_output_vol_bar (self);
}


static void
on_output_stream_port_changed (GvcMixerStream *stream, GParamSpec *pspec, gpointer data)
{
  PhoshSettings *self = PHOSH_SETTINGS (data);
  const char *form_factor;
  gboolean is_headphone = FALSE;
  const char *icon = "audio-speakers-symbolic";
  const GvcMixerStreamPort *port;
  PhoshMediaPlayer *media_player = PHOSH_MEDIA_PLAYER (self->media_player);

  port = gvc_mixer_stream_get_port (stream);
  g_return_if_fail (port);
  g_debug ("Port changed: %s (%s)", port->human_port ?: port->port, port->port);

  form_factor = gvc_mixer_stream_get_form_factor (stream);
  if (g_strcmp0 (form_factor, "headset") == 0 ||
      g_strcmp0 (form_factor, "headphone") == 0) {
    is_headphone = TRUE;
  }

  if (g_strcmp0 (port->port, "[Out] Headphones") == 0 ||
      g_strcmp0 (port->port, "analog-output-headphones") == 0) {
    is_headphone = TRUE;
  }

  if (is_headphone == self->is_headphone)
    return;

  self->is_headphone = is_headphone;
  if (is_headphone)
    icon = "audio-headphones-symbolic";
  else if (phosh_media_player_get_is_playable (media_player) &&
           phosh_media_player_get_status (media_player) == PHOSH_MEDIA_PLAYER_STATUS_PLAYING) {
    phosh_media_player_toggle_play_pause (media_player);
  }

  gvc_channel_bar_set_icon_name (GVC_CHANNEL_BAR (self->output_vol_bar), icon);
}


static void
mixer_control_output_update_cb (GvcMixerControl *mixer, guint id, gpointer data)
{
  PhoshSettings *self = PHOSH_SETTINGS (data);

  g_debug ("Audio output updated: %d", id);

  g_return_if_fail (PHOSH_IS_SETTINGS (self));

  if (self->output_stream)
    g_signal_handlers_disconnect_by_data (self->output_stream, self);

  g_set_object (&self->output_stream,
                gvc_mixer_control_get_default_sink (self->mixer_control));
  g_return_if_fail (self->output_stream);

  g_signal_connect_object (self->output_stream,
                           "notify::volume",
                           G_CALLBACK (output_stream_notify_volume_cb),
                           self, 0);

  g_signal_connect_object (self->output_stream,
                           "notify::is-muted",
                           G_CALLBACK (output_stream_notify_is_muted_cb),
                           self, 0);

  g_signal_connect_object (self->output_stream,
                           "notify::port",
                           G_CALLBACK (on_output_stream_port_changed),
                           self, 0);
  on_output_stream_port_changed (self->output_stream, NULL, self);

  update_output_vol_bar (self);
}


static void
vol_bar_value_changed_cb (GvcChannelBar *bar, PhoshSettings *self)
{
  double volume, rounded;
  g_autofree char *name = NULL;

  if (!self->output_stream)
    self->output_stream = g_object_ref (gvc_mixer_control_get_default_sink (self->mixer_control));

  volume = gvc_channel_bar_get_volume (bar);
  rounded = round (volume);

  g_object_get (self->output_vol_bar, "name", &name, NULL);
  g_debug ("Setting stream volume %lf (rounded: %lf) for bar '%s'", volume, rounded, name);

  g_return_if_fail (self->output_stream);
  if (gvc_mixer_stream_set_volume (self->output_stream, (pa_volume_t) rounded) != FALSE)
    gvc_mixer_stream_push_volume (self->output_stream);

  gvc_mixer_stream_change_is_muted (self->output_stream, (int) rounded == 0);
}


static void
on_quicksetting_activated (PhoshSettings   *self,
                           GtkFlowBoxChild *child,
                           GtkFlowBox      *box)

{
  GtkWidget *quick_setting;

  quick_setting = gtk_bin_get_child (GTK_BIN (child));
  gtk_button_clicked (GTK_BUTTON (quick_setting));
}

static void
on_media_player_raised (PhoshSettings *self,
                        gpointer       unused)
{
  g_return_if_fail (PHOSH_IS_SETTINGS (self));
  g_signal_emit (self, signals[SETTING_DONE], 0);
}


static void
on_notifications_clear_all_clicked (PhoshSettings *self)
{
  PhoshNotifyManager *manager;

  manager = phosh_notify_manager_get_default ();
  phosh_notify_manager_close_all_notifications (manager, PHOSH_NOTIFICATION_REASON_DISMISSED);
  g_signal_emit (self, signals[SETTING_DONE], 0);
}


static GtkWidget *
create_notification_row (gpointer item, gpointer data)
{
  GtkWidget *row = NULL;
  GtkWidget *frame = NULL;

  row = g_object_new (GTK_TYPE_LIST_BOX_ROW,
                      "activatable", FALSE,
                      "visible", TRUE,
                      NULL);

  frame = phosh_notification_frame_new (TRUE);
  phosh_notification_frame_bind_model (PHOSH_NOTIFICATION_FRAME (frame), item);

  gtk_widget_show (frame);

  gtk_container_add (GTK_CONTAINER (row), frame);

  return row;
}


static void
on_torch_scale_value_changed (PhoshSettings *self, GtkScale *scale_torch)
{
  double value;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));
  g_return_if_fail (PHOSH_IS_TORCH_MANAGER (self->torch_manager));

  /* Only react to scale changes when torch is enabled */
  if (!phosh_torch_manager_get_enabled (self->torch_manager))
      return;

  self->setting_torch = TRUE;
  value = gtk_range_get_value (GTK_RANGE (self->scale_torch));
  g_debug ("Setting torch brightness to %.2f", value);
  phosh_torch_manager_set_scaled_brightness (self->torch_manager, value / 100.0);
}


static void
on_torch_brightness_changed (PhoshSettings *self, GParamSpec *pspec, PhoshTorchManager *manager)
{
  g_return_if_fail (PHOSH_IS_SETTINGS (self));
  g_return_if_fail (PHOSH_IS_TORCH_MANAGER (manager));

  if (self->setting_torch) {
    self->setting_torch = FALSE;
    return;
  }

  gtk_range_set_value (GTK_RANGE (self->scale_torch),
                       100.0 * phosh_torch_manager_get_scaled_brightness (self->torch_manager));
}


static void
on_notifcation_frames_items_changed (PhoshSettings *self,
                                     guint          position,
                                     guint          removed,
                                     guint          added,
                                     GListModel    *list)
{
  gboolean is_empty;

  g_return_if_fail (PHOSH_IS_SETTINGS (self));
  g_return_if_fail (G_IS_LIST_MODEL (list));

  is_empty = !g_list_model_get_n_items (list);
  g_debug("Notification list empty: %d", is_empty);

  gtk_widget_set_visible (GTK_WIDGET (self->box_notifications), !is_empty);
  if (is_empty)
    g_signal_emit (self, signals[SETTING_DONE], 0);
}


static void
setup_brightness_range (PhoshSettings *self)
{
  gtk_range_set_range (GTK_RANGE (self->scale_brightness), 0, 100);
  gtk_range_set_round_digits (GTK_RANGE (self->scale_brightness), 0);
  gtk_range_set_increments (GTK_RANGE (self->scale_brightness), 1, 10);
  brightness_init (GTK_SCALE (self->scale_brightness));
  g_signal_connect (self->scale_brightness,
                    "value-changed",
                    G_CALLBACK(brightness_value_changed_cb),
                    NULL);
}


static void
setup_torch (PhoshSettings *self)
{
  PhoshShell *shell = phosh_shell_get_default ();

  self->torch_manager = g_object_ref(phosh_shell_get_torch_manager (shell));

  gtk_range_set_range (GTK_RANGE (self->scale_torch), 40, 100);
  gtk_range_set_value (GTK_RANGE (self->scale_torch),
                       phosh_torch_manager_get_scaled_brightness (self->torch_manager) * 100.0);
  g_signal_connect_object (self->torch_manager,
                           "notify::brightness",
                           G_CALLBACK(on_torch_brightness_changed),
                           self,
                           G_CONNECT_SWAPPED);
}


static void
setup_volume_bar (PhoshSettings *self)
{
  self->output_vol_bar = gvc_channel_bar_new ();
  gtk_widget_set_sensitive (self->output_vol_bar, TRUE);
  gtk_widget_show (self->output_vol_bar);

  gtk_box_pack_start (GTK_BOX (self->box_settings), self->output_vol_bar, FALSE, FALSE, 0);
  gtk_box_reorder_child (GTK_BOX (self->box_settings), self->output_vol_bar, 1);

  self->mixer_control = gvc_mixer_control_new ("Phone Shell Volume Control");
  g_return_if_fail (self->mixer_control);

  gvc_mixer_control_open (self->mixer_control);
  g_signal_connect (self->mixer_control,
                    "active-output-update",
                    G_CALLBACK (mixer_control_output_update_cb),
                    self);
  g_signal_connect (self->output_vol_bar,
                    "value-changed",
                    G_CALLBACK (vol_bar_value_changed_cb),
                    self);
}


static void
phosh_settings_constructed (GObject *object)
{
  PhoshSettings *self = PHOSH_SETTINGS (object);
  PhoshNotifyManager *manager;

  setup_brightness_range (self);
  setup_volume_bar (self);
  setup_torch (self);

  g_signal_connect (self->quick_settings,
                    "child-activated",
                    G_CALLBACK (on_quicksetting_activated),
                    self);

  manager = phosh_notify_manager_get_default ();
  gtk_list_box_bind_model (GTK_LIST_BOX (self->list_notifications),
                           G_LIST_MODEL (phosh_notify_manager_get_list (manager)),
                           create_notification_row,
                           NULL,
                           NULL);
  g_signal_connect_object (phosh_notify_manager_get_list (manager),
                           "items-changed",
                           G_CALLBACK (on_notifcation_frames_items_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_notifcation_frames_items_changed (self, -1, -1, -1,
                                       G_LIST_MODEL (phosh_notify_manager_get_list (manager)));

  G_OBJECT_CLASS (phosh_settings_parent_class)->constructed (object);
}


static void
phosh_settings_dispose (GObject *object)
{
  PhoshSettings *self = PHOSH_SETTINGS (object);

  brightness_dispose ();

  g_clear_object (&self->output_stream);

  g_clear_object (&self->torch_manager);

  G_OBJECT_CLASS (phosh_settings_parent_class)->dispose (object);
}


static void
phosh_settings_finalize (GObject *object)
{
  PhoshSettings *self = PHOSH_SETTINGS (object);

  g_clear_object (&self->mixer_control);

  G_OBJECT_CLASS (phosh_settings_parent_class)->finalize (object);
}



static void
phosh_settings_class_init (PhoshSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = phosh_settings_dispose;
  object_class->finalize = phosh_settings_finalize;
  object_class->constructed = phosh_settings_constructed;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/sm/puri/phosh/ui/settings-menu.ui");

  signals[SETTING_DONE] = g_signal_new ("setting-done",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);

  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, box_settings);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, list_notifications);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, media_player);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, quick_settings);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, scale_brightness);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, scale_torch);
  gtk_widget_class_bind_template_child (widget_class, PhoshSettings, box_notifications);

  gtk_widget_class_bind_template_callback (widget_class, battery_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, bt_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, bt_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, docked_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, docked_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, feedback_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, feedback_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_media_player_raised);
  gtk_widget_class_bind_template_callback (widget_class, rotation_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, rotation_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, torch_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, wifi_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, wifi_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, wwan_setting_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, wwan_setting_long_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_notifications_clear_all_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_torch_scale_value_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_vpn_setting_long_pressed);
  gtk_widget_class_bind_template_callback (widget_class, on_vpn_setting_clicked);

}


static void
phosh_settings_init (PhoshSettings *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}


GtkWidget *
phosh_settings_new (void)
{
  return g_object_new (PHOSH_TYPE_SETTINGS, NULL);
}
