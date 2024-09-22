/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-activity"

#include "phosh-config.h"
#include "activity.h"
#include "shell.h"
#include "swipe-away-bin.h"
#include "thumbnail.h"
#include "util.h"
#include "app-grid-button.h"

#include <gio/gdesktopappinfo.h>

/**
 * PhoshActivity:
 *
 * An app in the favorites overview
 *
 * The #PhoshActivity is used to select a running application in the overview.
 */

/* Icons actually sized according to the pixel-size set in the template */
#define ACTIVITY_ICON_SIZE -1

enum {
  CLICKED,
  CLOSED,
  RESIZED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

enum {
  PROP_0,
  PROP_APP_ID,
  PROP_PARENT_APP_ID,
  PROP_MAXIMIZED,
  PROP_FULLSCREEN,
  PROP_WIN_WIDTH,
  PROP_WIN_HEIGHT,
  LAST_PROP,
};
static GParamSpec *props[LAST_PROP];

typedef struct
{
  GtkWidget *swipe_bin;
  GtkWidget *icon;
  GtkWidget *box;
  GtkWidget *revealer;
  GtkWidget *btn_close;
  GtkWidget *preview;
  GtkWidget *button;

  gboolean maximized;
  gboolean fullscreen;
  int win_width;
  int win_height;

  char *app_id;
  char *parent_app_id;

  cairo_surface_t *surface;
  PhoshThumbnail *thumbnail;

  gboolean hovering;
  guint remove_timeout_id;
  GtkAllocation allocation;
} PhoshActivityPrivate;


struct _PhoshActivity
{
  GtkEventBox parent;
};

G_DEFINE_TYPE_WITH_PRIVATE(PhoshActivity, phosh_activity, GTK_TYPE_EVENT_BOX)


static void
phosh_activity_set_property (GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
  PhoshActivity *self = PHOSH_ACTIVITY (object);
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private(self);
  int height, width;

  switch (property_id) {
    case PROP_APP_ID:
      g_free (priv->app_id);
      priv->app_id = g_value_dup_string (value);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_APP_ID]);
      break;
    case PROP_PARENT_APP_ID:
      g_free (priv->parent_app_id);
      priv->parent_app_id = g_value_dup_string (value);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PARENT_APP_ID]);
      break;
    case PROP_MAXIMIZED:
      priv->maximized = g_value_get_boolean (value);
      phosh_util_toggle_style_class (GTK_WIDGET (self), "phosh-maximized", priv->maximized);
      break;
    case PROP_FULLSCREEN:
      priv->fullscreen = g_value_get_boolean (value);
      phosh_util_toggle_style_class (GTK_WIDGET (self), "phosh-fullscreen", priv->fullscreen);
      break;
    case PROP_WIN_WIDTH:
      width = g_value_get_int (value);
      if (width != priv->win_width) {
        priv->win_width = width;
        gtk_widget_queue_resize (GTK_WIDGET (self));
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WIN_WIDTH]);
      }
      break;
    case PROP_WIN_HEIGHT:
      height = g_value_get_int (value);
      if (height != priv->win_height) {
        priv->win_height = height;
        gtk_widget_queue_resize (GTK_WIDGET (self));
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WIN_HEIGHT]);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
phosh_activity_get_property (GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  PhoshActivity *self = PHOSH_ACTIVITY (object);
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private(self);

  switch (property_id) {
    case PROP_APP_ID:
      g_value_set_string (value, priv->app_id);
      break;
    case PROP_PARENT_APP_ID:
      g_value_set_string (value, priv->parent_app_id);
      break;
    case PROP_MAXIMIZED:
      g_value_set_boolean (value, priv->maximized);
      break;
    case PROP_FULLSCREEN:
      g_value_set_boolean (value, priv->fullscreen);
      break;
    case PROP_WIN_WIDTH:
      g_value_set_int (value, priv->win_width);
      break;
    case PROP_WIN_HEIGHT:
      g_value_set_int (value, priv->win_height);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
clicked_cb (PhoshActivity *self)
{
  g_signal_emit (self, signals[CLICKED], 0);
}


static void
closed_cb (PhoshActivity *self)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  phosh_swipe_away_bin_remove (PHOSH_SWIPE_AWAY_BIN (priv->swipe_bin));
}


static gboolean
remove_timeout_cb (PhoshActivity *self)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  phosh_swipe_away_bin_undo (PHOSH_SWIPE_AWAY_BIN (priv->swipe_bin));

  priv->remove_timeout_id = 0;

  return G_SOURCE_REMOVE;
}


static void
removed_cb (PhoshActivity *self)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  if (priv->remove_timeout_id)
    g_source_remove (priv->remove_timeout_id);

  priv->remove_timeout_id =
    g_timeout_add_seconds (1, (GSourceFunc) remove_timeout_cb, self);
  g_source_set_name_by_id (priv->remove_timeout_id, "[phosh] remove_timeout_id");

  g_signal_emit (self, signals[CLOSED], 0);
}


static float
get_scale (PhoshActivity *self)
{
  float scale;
  int width, height, image_width, image_height;
  PhoshActivityPrivate *priv;

  priv = phosh_activity_get_instance_private (self);
  width = gtk_widget_get_allocated_width (priv->preview);
  height = gtk_widget_get_allocated_height (priv->preview);

  if (!priv->surface)
    return 1.0;

  image_width = cairo_image_surface_get_width (priv->surface);
  image_height = cairo_image_surface_get_height (priv->surface);

  scale = width / (float)image_width;

  if (height / (float)image_height < scale)
    scale = height / (float)image_height;

  return scale;
}

static gboolean
draw_cb (PhoshActivity *self, cairo_t *cairo, GtkDrawingArea *area)
{
  int width, height, image_width, image_height, x, y = 0;
  float scale;
  PhoshActivityPrivate *priv;
  GtkStyleContext *context;

  g_return_val_if_fail (PHOSH_IS_ACTIVITY (self), FALSE);
  g_return_val_if_fail (GTK_IS_DRAWING_AREA (area), FALSE);
  width = gtk_widget_get_allocated_width (GTK_WIDGET (area));
  height = gtk_widget_get_allocated_height (GTK_WIDGET (area));
  priv = phosh_activity_get_instance_private (self);
  context = gtk_widget_get_style_context (GTK_WIDGET (area));

  if (!priv->surface)
    return FALSE;

  image_width = cairo_image_surface_get_width (priv->surface);
  image_height = cairo_image_surface_get_height (priv->surface);


  gtk_render_background(context, cairo, 0, 0, width, height);

  scale = get_scale (self);
  cairo_scale (cairo, scale, scale);

  x = (width - image_width * scale) / 2.0 / scale;

  cairo_rectangle (cairo, x, y, image_width, image_height);
  cairo_set_source_surface (cairo, priv->surface, x, y);
  cairo_fill (cairo);

  return FALSE;
}


static void
size_allocate_cb (PhoshActivity *self, GtkAllocation *alloc, GtkDrawingArea *area)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);
  gboolean changed = alloc->width != priv->allocation.width ||
                     alloc->height != priv->allocation.height;

  priv->allocation = *alloc;

  if (changed)
    g_signal_emit (self, signals[RESIZED], 0, alloc);
}


static void
phosh_activity_constructed (GObject *object)
{
  PhoshActivity *self = PHOSH_ACTIVITY (object);
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);
  g_autoptr (GDesktopAppInfo) app_info = NULL;
  GIcon *icon = NULL;

  app_info = phosh_get_desktop_app_info_for_app_id (priv->app_id);
  if (app_info)
    icon = g_app_info_get_icon (G_APP_INFO (app_info));

  if (!icon && priv->parent_app_id) {
    app_info = phosh_get_desktop_app_info_for_app_id (priv->parent_app_id);
    if (app_info)
      icon = g_app_info_get_icon (G_APP_INFO (app_info));
  }

  if (icon) {
    gtk_image_set_from_gicon (GTK_IMAGE (priv->icon), icon, ACTIVITY_ICON_SIZE);
  } else {
    gtk_image_set_from_icon_name (GTK_IMAGE (priv->icon), PHOSH_APP_UNKNOWN_ICON, ACTIVITY_ICON_SIZE);
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (self)), "phosh-empty");
  }

  G_OBJECT_CLASS (phosh_activity_parent_class)->constructed (object);
}


static void
phosh_activity_dispose (GObject *object)
{
  PhoshActivity *self = PHOSH_ACTIVITY (object);
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  g_clear_pointer (&priv->surface, cairo_surface_destroy);
  g_clear_object (&priv->thumbnail);

  if (priv->remove_timeout_id) {
    g_source_remove (priv->remove_timeout_id);
    priv->remove_timeout_id = 0;
  }

  G_OBJECT_CLASS (phosh_activity_parent_class)->dispose (object);
}


static void
phosh_activity_finalize (GObject *object)
{
  PhoshActivity *self = PHOSH_ACTIVITY (object);
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  g_free (priv->parent_app_id);
  g_free (priv->app_id);

  G_OBJECT_CLASS (phosh_activity_parent_class)->finalize (object);
}


static GtkSizeRequestMode
phosh_activity_get_request_mode (GtkWidget *widgte)
{
  return GTK_SIZE_REQUEST_WIDTH_FOR_HEIGHT;
}


static void
phosh_activity_get_preferred_height (GtkWidget *widget,
                                     int       *min,
                                     int       *nat)
{
  PhoshActivityPrivate *priv;
  int smallest = 0;
  int box_smallest = 0;
  int parent_nat;

  g_return_if_fail (PHOSH_IS_ACTIVITY (widget));
  priv = phosh_activity_get_instance_private (PHOSH_ACTIVITY (widget));

  GTK_WIDGET_CLASS (phosh_activity_parent_class)->get_preferred_height (widget,
                                                                        &smallest,
                                                                        &parent_nat);
  gtk_widget_get_preferred_width (priv->box, &box_smallest, NULL);

  smallest = MAX (smallest, box_smallest);

  if (min)
    *min = smallest;

  if (nat)
    *nat = smallest;
}


static void
phosh_activity_get_preferred_width_for_height (GtkWidget *widget,
                                               int        height,
                                               int       *min,
                                               int       *nat)
{
  PhoshActivityPrivate *priv;
  int smallest = 0;
  int box_smallest = 0;
  int size;
  int parent_nat;
  int margin_start, margin_end, margin_top, margin_bottom;
  double aspect_ratio;

  g_return_if_fail (PHOSH_IS_ACTIVITY (widget));
  priv = phosh_activity_get_instance_private (PHOSH_ACTIVITY (widget));

  GTK_WIDGET_CLASS (phosh_activity_parent_class)->get_preferred_width_for_height (widget,
                                                                                  height,
                                                                                  &smallest,
                                                                                  &parent_nat);
  gtk_widget_get_preferred_width_for_height (priv->box, height, &box_smallest, NULL);

  smallest = MAX (smallest, box_smallest);

  margin_start = gtk_widget_get_margin_start (priv->preview);
  margin_end = gtk_widget_get_margin_end (priv->preview);
  margin_top = gtk_widget_get_margin_top (priv->preview);
  margin_bottom = gtk_widget_get_margin_bottom (priv->preview);

  aspect_ratio = (double) priv->win_width / priv->win_height;
  size = MAX (smallest, (height - margin_top - margin_bottom) * aspect_ratio) + margin_start + margin_end;

  if (min)
    *min = size;

  if (nat)
    *nat = size;
}


static void
phosh_activity_get_preferred_height_for_width (GtkWidget *widget,
                                               int        width,
                                               int       *min,
                                               int       *nat)
{
  phosh_activity_get_preferred_height (widget, min, nat);
}


static void
set_hovering (PhoshActivity *self,
              gboolean       hovering)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private(self);

  if (hovering == priv->hovering)
    return;

  priv->hovering = hovering;

  // Revealer won't animate if not mapped, show it preemptively
  if (hovering)
    gtk_widget_show (priv->revealer);

  gtk_revealer_set_reveal_child (GTK_REVEALER (priv->revealer), hovering);
}


static gboolean
phosh_activity_enter_notify_event (GtkWidget        *widget,
                                   GdkEventCrossing *event)
{
  if (event->window != gtk_widget_get_window (widget) ||
      event->detail == GDK_NOTIFY_INFERIOR)
    return GDK_EVENT_PROPAGATE;

  /* enter-notify never happens on touch, so we don't need to check it */
  set_hovering (PHOSH_ACTIVITY (widget), TRUE);

  return GDK_EVENT_PROPAGATE;
}


static gboolean
phosh_activity_leave_notify_event (GtkWidget        *widget,
                                   GdkEventCrossing *event)
{
  if (event->window != gtk_widget_get_window (widget) ||
      event->detail == GDK_NOTIFY_INFERIOR)
    return GDK_EVENT_PROPAGATE;

  set_hovering (PHOSH_ACTIVITY (widget), FALSE);

  return GDK_EVENT_PROPAGATE;
}


static gboolean
phosh_activity_motion_notify_event (GtkWidget      *widget,
                                    GdkEventMotion *event)
{
  GdkDevice *source_device = gdk_event_get_source_device ((GdkEvent *) event);
  GdkInputSource input_source = gdk_device_get_source (source_device);

  if (input_source != GDK_SOURCE_TOUCHSCREEN)
    set_hovering (PHOSH_ACTIVITY (widget), TRUE);

  return GDK_EVENT_PROPAGATE;
}


static gboolean
phosh_activity_key_press_event (GtkWidget *self, GdkEventKey *event)
{
  gboolean handled = FALSE;
  PhoshActivityPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_ACTIVITY (self), FALSE);
  priv = phosh_activity_get_instance_private (PHOSH_ACTIVITY (self));

  switch (event->keyval) {
  case GDK_KEY_Return:
    gtk_button_clicked (GTK_BUTTON (priv->button));
    handled = TRUE;
    break;
  default:
    /* nothing to do */
    break;
  }

  return handled;
}


static void
phosh_activity_unmap (GtkWidget *widget)
{
  set_hovering (PHOSH_ACTIVITY (widget), FALSE);

  GTK_WIDGET_CLASS (phosh_activity_parent_class)->unmap (widget);
}


static void
phosh_activity_class_init (PhoshActivityClass *klass)
{
  GObjectClass *object_class = (GObjectClass *)klass;
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = phosh_activity_constructed;
  object_class->dispose = phosh_activity_dispose;
  object_class->finalize = phosh_activity_finalize;

  object_class->set_property = phosh_activity_set_property;
  object_class->get_property = phosh_activity_get_property;

  widget_class->get_request_mode = phosh_activity_get_request_mode;
  widget_class->get_preferred_height = phosh_activity_get_preferred_height;
  widget_class->get_preferred_height_for_width = phosh_activity_get_preferred_height_for_width;
  widget_class->get_preferred_width_for_height = phosh_activity_get_preferred_width_for_height;
  widget_class->enter_notify_event = phosh_activity_enter_notify_event;
  widget_class->leave_notify_event = phosh_activity_leave_notify_event;
  widget_class->motion_notify_event = phosh_activity_motion_notify_event;
  widget_class->key_press_event = phosh_activity_key_press_event;
  widget_class->unmap = phosh_activity_unmap;

  props[PROP_APP_ID] =
    g_param_spec_string (
      "app-id",
      "app-id",
      "The application id",
      "",
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * PhoshActivity:parent-app-id:
   *
   * The app-id of the parent activity (if any)
   */
  props[PROP_PARENT_APP_ID] =
    g_param_spec_string ("parent-app-id", "", "",
                         NULL,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_MAXIMIZED] =
    g_param_spec_boolean (
      "maximized",
      "maximized",
      "Whether the window is maximized",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  props[PROP_FULLSCREEN] =
    g_param_spec_boolean (
      "fullscreen",
      "fullscreen",
      "Whether the window is presented fullscreen",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  props[PROP_WIN_WIDTH] =
    g_param_spec_int (
      "win-width",
      "Window Width",
      "The window's width",
      0,
      G_MAXINT,
      300,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_WIN_HEIGHT] =
    g_param_spec_int (
      "win-height",
      "Window Height",
      "The window's height",
      0,
      G_MAXINT,
      300,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, props);

  signals[CLICKED] = g_signal_new ("clicked",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);

  signals[CLOSED] = g_signal_new ("closed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);

  signals[RESIZED] = g_signal_new ("resized",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 1, GDK_TYPE_RECTANGLE | G_SIGNAL_TYPE_STATIC_SCOPE);

  g_type_ensure (PHOSH_TYPE_SWIPE_AWAY_BIN);

  gtk_widget_class_set_template_from_resource (widget_class, "/sm/puri/phosh/ui/activity.ui");

  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, btn_close);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, button);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, preview);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, swipe_bin);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, icon);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, box);
  gtk_widget_class_bind_template_child_private (widget_class, PhoshActivity, revealer);
  gtk_widget_class_bind_template_callback (widget_class, clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, draw_cb);
  gtk_widget_class_bind_template_callback (widget_class, size_allocate_cb);
  gtk_widget_class_bind_template_callback (widget_class, closed_cb);
  gtk_widget_class_bind_template_callback (widget_class, removed_cb);

  gtk_widget_class_set_css_name (widget_class, "phosh-activity");
}


static void
phosh_activity_init (PhoshActivity *self)
{
  PhoshActivityPrivate *priv = phosh_activity_get_instance_private (self);

  gtk_widget_init_template (GTK_WIDGET (self));

  priv->win_height = 300;
  priv->win_width = 300;
}


GtkWidget *
phosh_activity_new (const char *app_id)
{
  return g_object_new (PHOSH_TYPE_ACTIVITY,
                       "app-id", app_id,
                       NULL);
}


const char *
phosh_activity_get_app_id (PhoshActivity *self)
{
  PhoshActivityPrivate *priv;

  g_return_val_if_fail (PHOSH_IS_ACTIVITY (self), NULL);
  priv = phosh_activity_get_instance_private (self);

  return priv->app_id;
}

void
phosh_activity_set_thumbnail (PhoshActivity *self, PhoshThumbnail *thumbnail)
{
  PhoshActivityPrivate *priv;
  void *data;
  guint w, width, height, stride, margin;
  enum wl_shm_format format;
  float scale;

  g_return_if_fail (PHOSH_IS_ACTIVITY (self));
  priv = phosh_activity_get_instance_private (self);

  g_clear_pointer (&priv->surface, cairo_surface_destroy);
  g_clear_object (&priv->thumbnail);

  data = phosh_thumbnail_get_image (thumbnail);
  phosh_thumbnail_get_size (thumbnail, &width, &height, &stride);
  phosh_thumbnail_get_format (thumbnail, &format);

  phosh_convert_buffer (data, format, width, height, stride);

  priv->surface = cairo_image_surface_create_for_data (
      data, CAIRO_FORMAT_ARGB32, width, height, stride);
  priv->thumbnail = thumbnail;

  gtk_style_context_remove_class (gtk_widget_get_style_context (GTK_WIDGET (self)), "phosh-empty");

  /* Make sure the close button is over the thumbnail */
  w = gtk_widget_get_allocated_width (GTK_WIDGET (self));
  scale = get_scale (self);
  margin = w ? (w - (width * scale)) / 2 : 0;
  gtk_widget_set_margin_end (priv->btn_close, margin);

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
phosh_activity_get_thumbnail_allocation (PhoshActivity *self, GtkAllocation *allocation)
{
  PhoshActivityPrivate *priv;

  g_return_if_fail (allocation);
  g_return_if_fail (PHOSH_IS_ACTIVITY (self));
  priv = phosh_activity_get_instance_private (self);

  *allocation = priv->allocation;
}
