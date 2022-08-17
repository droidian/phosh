/*
 * Copyright (C) 2020 Alexander Mikhaylenko <alexm@gnome.org>
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Based on hdy-carousel-indicator-dots.c from Libhandy 1.6 published
 * as LGPL-2.1-or-later.
 */

#include "app-carousel-indicator.h"

#include <math.h>

#define DOTS_RADIUS 3
#define DOTS_RADIUS_SELECTED 4
#define FAVORITES_RADIUS 5
#define FAVORITES_RADIUS_SELECTED 6
#define DOTS_OPACITY 0.3
#define DOTS_OPACITY_SELECTED 0.9
#define DOTS_SPACING 7
#define DOTS_MARGIN 6
#define FAVORITES_ICON_NAME "emblem-favorite-symbolic"

/**
 * PhoshAppCarouselIndicator:
 *
 * A fork of #HdyCarouselIndicatorDots which can display a favorites icon for the first pages.
 */

struct _PhoshAppCarouselIndicator
{
  GtkDrawingArea parent_instance;

  HdyCarousel *carousel;
  guint favorites;
  GtkOrientation orientation;

  cairo_surface_t *favorites_surface;
  cairo_surface_t *favorites_selected_surface;

  guint tick_cb_id;
  guint64 end_time;
};

G_DEFINE_TYPE_WITH_CODE (PhoshAppCarouselIndicator, phosh_app_carousel_indicator, GTK_TYPE_DRAWING_AREA,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_ORIENTABLE, NULL))

enum {
  PROP_0,
  PROP_CAROUSEL,
  PROP_FAVORITES,

  /* GtkOrientable */
  PROP_ORIENTATION,
  LAST_PROP = PROP_FAVORITES + 1,
};

static GParamSpec *props[LAST_PROP];

static void
favorites_surface_update (PhoshAppCarouselIndicator *self)
{
  GtkIconTheme *icon_theme;
  gint scale_factor;
  GdkWindow *parent_window;
  GtkIconLookupFlags flags;
  GError *error = NULL;

  icon_theme = gtk_icon_theme_get_default ();
  scale_factor = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  parent_window = gtk_widget_get_parent_window (GTK_WIDGET (self));
  flags = GTK_ICON_LOOKUP_FORCE_SVG | GTK_ICON_LOOKUP_FORCE_SYMBOLIC | GTK_ICON_LOOKUP_FORCE_SIZE;

  if (self->favorites_surface)
    cairo_surface_destroy (self->favorites_surface);

  if (self->favorites_selected_surface)
    cairo_surface_destroy (self->favorites_selected_surface);

  self->favorites_surface =
    gtk_icon_theme_load_surface (icon_theme, FAVORITES_ICON_NAME,
                                 FAVORITES_RADIUS * 2, scale_factor,
                                 parent_window, flags, &error);

  self->favorites_selected_surface =
    gtk_icon_theme_load_surface (icon_theme, FAVORITES_ICON_NAME,
                                 FAVORITES_RADIUS_SELECTED * 2, scale_factor,
                                 parent_window, flags, &error);
}

static gdouble
lerp (gdouble a, gdouble b, gdouble t)
{
  return a * (1.0 - t) + b * t;
}

static gboolean
animation_cb (GtkWidget     *widget,
              GdkFrameClock *frame_clock,
              gpointer       user_data)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (widget);
  gint64 frame_time;

  g_assert (self->tick_cb_id > 0);

  gtk_widget_queue_resize (GTK_WIDGET (self));

  frame_time = gdk_frame_clock_get_frame_time (frame_clock) / 1000;

  if (frame_time >= self->end_time ||
      !hdy_get_enable_animations (GTK_WIDGET (self))) {
    self->tick_cb_id = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void
stop_animation (PhoshAppCarouselIndicator *self)
{
  if (self->tick_cb_id == 0)
    return;

  gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_cb_id);
  self->tick_cb_id = 0;
}

static void
animate (PhoshAppCarouselIndicator *self,
         gint64                     duration)
{
  GdkFrameClock *frame_clock;
  gint64 frame_time;

  if (duration <= 0 || !hdy_get_enable_animations (GTK_WIDGET (self))) {
    gtk_widget_queue_resize (GTK_WIDGET (self));
    return;
  }

  frame_clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  if (!frame_clock) {
    gtk_widget_queue_resize (GTK_WIDGET (self));
    return;
  }

  frame_time = gdk_frame_clock_get_frame_time (frame_clock);

  self->end_time = MAX (self->end_time, frame_time / 1000 + duration);
  if (self->tick_cb_id == 0)
    self->tick_cb_id = gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                                     animation_cb,
                                                     NULL, NULL);
}

static GdkRGBA
get_color (GtkWidget *widget)
{
  GtkStyleContext *context;
  GtkStateFlags flags;
  GdkRGBA color;

  context = gtk_widget_get_style_context (widget);
  flags = gtk_widget_get_state_flags (widget);
  gtk_style_context_get_color (context, flags, &color);

  return color;
}

static void
draw_dots (GtkWidget      *widget,
           cairo_t        *cr,
           GtkOrientation  orientation,
           gdouble         position,
           gdouble        *sizes,
           guint           n_pages)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (widget);
  GdkRGBA color;
  gint i, widget_length, widget_thickness;
  gdouble x, y, indicator_length, dot_size, full_size;
  gdouble current_position, remaining_progress;

  color = get_color (widget);
  dot_size = 2 * DOTS_RADIUS_SELECTED + DOTS_SPACING;

  indicator_length = 0;
  for (i = 0; i < n_pages; i++)
    indicator_length += dot_size * sizes[i];

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    widget_length = gtk_widget_get_allocated_width (widget);
    widget_thickness = gtk_widget_get_allocated_height (widget);
  } else {
    widget_length = gtk_widget_get_allocated_height (widget);
    widget_thickness = gtk_widget_get_allocated_width (widget);
  }

  /* Ensure the indicators are aligned to pixel grid when not animating */
  full_size = round (indicator_length / dot_size) * dot_size;
  if ((widget_length - (gint) full_size) % 2 == 0)
    widget_length--;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    cairo_translate (cr, (widget_length - indicator_length) / 2.0, widget_thickness / 2);
  else
    cairo_translate (cr, widget_thickness / 2, (widget_length - indicator_length) / 2.0);

  x = 0;
  y = 0;

  current_position = 0;
  remaining_progress = 1;

  for (i = 0; i < n_pages; i++) {
    gdouble progress, radius, opacity;

    if (orientation == GTK_ORIENTATION_HORIZONTAL)
      x += dot_size * sizes[i] / 2.0;
    else
      y += dot_size * sizes[i] / 2.0;

    current_position += sizes[i];

    progress = CLAMP (current_position - position, 0, remaining_progress);
    remaining_progress -= progress;

    opacity = lerp (DOTS_OPACITY, DOTS_OPACITY_SELECTED, progress) * sizes[i];

    cairo_set_source_rgba (cr, color.red, color.green, color.blue,
                           color.alpha * opacity);
    if (i >= self->favorites || !self->favorites_surface || !self->favorites_selected_surface) {
      radius = lerp (DOTS_RADIUS, DOTS_RADIUS_SELECTED, progress) * sizes[i];

      cairo_arc (cr, x, y, radius, 0, 2 * G_PI);
      cairo_fill (cr);
    } else {
      if (progress <= 0.0) {
        cairo_mask_surface (cr, self->favorites_surface,
                            x - FAVORITES_RADIUS,
                            y - FAVORITES_RADIUS);
      } else if (progress >= 1.0) {
        cairo_mask_surface (cr, self->favorites_selected_surface,
                            x - FAVORITES_RADIUS_SELECTED,
                            y - FAVORITES_RADIUS_SELECTED);
      } else {
        gdouble scale;

        radius = lerp (FAVORITES_RADIUS, FAVORITES_RADIUS_SELECTED, progress) * sizes[i];
        scale = radius / FAVORITES_RADIUS_SELECTED;

        cairo_save (cr);
        cairo_scale (cr, scale, scale);
        cairo_mask_surface (cr, self->favorites_selected_surface,
                            (x - radius) / scale,
                            (y - radius) / scale);
        cairo_restore (cr);
      }
    }

    if (orientation == GTK_ORIENTATION_HORIZONTAL)
      x += dot_size * sizes[i] / 2.0;
    else
      y += dot_size * sizes[i] / 2.0;
  }
}

static void
n_pages_changed_cb (PhoshAppCarouselIndicator *self)
{
  animate (self, hdy_carousel_get_reveal_duration (self->carousel));
}

static void
phosh_app_carousel_indicator_measure (GtkWidget      *widget,
                                       GtkOrientation  orientation,
                                       gint            for_size,
                                       gint           *minimum,
                                       gint           *natural,
                                       gint           *minimum_baseline,
                                       gint           *natural_baseline)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (widget);
  gint size = 0;

  if (orientation == self->orientation) {
    gint i, n_points = 0;
    gdouble indicator_length, dot_size;
    g_autofree gdouble *points = NULL;
    g_autofree gdouble *sizes = NULL;

    if (self->carousel)
      points = hdy_swipeable_get_snap_points (HDY_SWIPEABLE (self->carousel), &n_points);

    sizes = g_new0 (gdouble, n_points);

    if (n_points > 0)
      sizes[0] = points[0] + 1;
    for (i = 1; i < n_points; i++)
      sizes[i] = points[i] - points[i - 1];

    dot_size = 2 * DOTS_RADIUS_SELECTED + DOTS_SPACING;
    indicator_length = 0;
    for (i = 0; i < n_points; i++)
      indicator_length += dot_size * sizes[i];

    size = ceil (indicator_length);
  } else {
    size = 2 * DOTS_RADIUS_SELECTED;
  }

  size += 2 * DOTS_MARGIN;

  if (minimum)
    *minimum = size;

  if (natural)
    *natural = size;

  if (minimum_baseline)
    *minimum_baseline = -1;

  if (natural_baseline)
    *natural_baseline = -1;
}

static void
phosh_app_carousel_indicator_get_preferred_width (GtkWidget *widget,
                                                   gint      *minimum_width,
                                                   gint      *natural_width)
{
  phosh_app_carousel_indicator_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1,
                                  minimum_width, natural_width, NULL, NULL);
}

static void
phosh_app_carousel_indicator_get_preferred_height (GtkWidget *widget,
                                                    gint      *minimum_height,
                                                    gint      *natural_height)
{
  phosh_app_carousel_indicator_measure (widget, GTK_ORIENTATION_VERTICAL, -1,
                                  minimum_height, natural_height, NULL, NULL);
}

static gboolean
phosh_app_carousel_indicator_draw (GtkWidget *widget,
                                    cairo_t   *cr)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (widget);
  gint i, n_points;
  gdouble position;
  g_autofree gdouble *points = NULL;
  g_autofree gdouble *sizes = NULL;

  if (!self->carousel)
    return GDK_EVENT_PROPAGATE;

  points = hdy_swipeable_get_snap_points (HDY_SWIPEABLE (self->carousel), &n_points);
  position = hdy_carousel_get_position (self->carousel);

  if (n_points < 2)
    return GDK_EVENT_PROPAGATE;

  if (self->orientation == GTK_ORIENTATION_HORIZONTAL &&
      gtk_widget_get_direction (widget) == GTK_TEXT_DIR_RTL)
    position = points[n_points - 1] - position;

  sizes = g_new0 (gdouble, n_points);

  sizes[0] = points[0] + 1;
  for (i = 1; i < n_points; i++)
    sizes[i] = points[i] - points[i - 1];

  draw_dots (widget, cr, self->orientation, position, sizes, n_points);

  return GDK_EVENT_PROPAGATE;
}

static void
hdy_carousel_dispose (GObject *object)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (object);

  phosh_app_carousel_indicator_set_carousel (self, NULL);
  g_clear_pointer (&self->favorites_surface, cairo_surface_destroy);
  g_clear_pointer (&self->favorites_selected_surface, cairo_surface_destroy);

  G_OBJECT_CLASS (phosh_app_carousel_indicator_parent_class)->dispose (object);
}

static void
phosh_app_carousel_indicator_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (object);

  switch (prop_id) {
  case PROP_CAROUSEL:
    g_value_set_object (value, phosh_app_carousel_indicator_get_carousel (self));
    break;

  case PROP_FAVORITES:
    g_value_set_uint (value, phosh_app_carousel_indicator_get_favorites (self));
    break;

  case PROP_ORIENTATION:
    g_value_set_enum (value, self->orientation);
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
phosh_app_carousel_indicator_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  PhoshAppCarouselIndicator *self = PHOSH_APP_CAROUSEL_INDICATOR (object);

  switch (prop_id) {
  case PROP_CAROUSEL:
    phosh_app_carousel_indicator_set_carousel (self, g_value_get_object (value));
    break;

  case PROP_FAVORITES:
    phosh_app_carousel_indicator_set_favorites (self, g_value_get_uint (value));
    break;

  case PROP_ORIENTATION:
    {
      GtkOrientation orientation = g_value_get_enum (value);
      if (orientation != self->orientation) {
        self->orientation = orientation;
        gtk_widget_queue_resize (GTK_WIDGET (self));
        g_object_notify (G_OBJECT (self), "orientation");
      }
    }
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
phosh_app_carousel_indicator_class_init (PhoshAppCarouselIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = hdy_carousel_dispose;
  object_class->get_property = phosh_app_carousel_indicator_get_property;
  object_class->set_property = phosh_app_carousel_indicator_set_property;

  widget_class->get_preferred_width = phosh_app_carousel_indicator_get_preferred_width;
  widget_class->get_preferred_height = phosh_app_carousel_indicator_get_preferred_height;
  widget_class->draw = phosh_app_carousel_indicator_draw;

  /**
   * PhoshAppCarouselIndicator:carousel: (attributes org.gtk.Property.get=phosh_app_carousel_indicator_get_carousel org.gtk.Property.set=phosh_app_carousel_indicator_set_carousel)
   *
   * The [class@Carousel] the indicator uses.
   *
   * Since: 1.0
   */
  props[PROP_CAROUSEL] =
    g_param_spec_object ("carousel",
                         "Carousel",
                         "Carousel",
                         HDY_TYPE_CAROUSEL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_FAVORITES] =
    g_param_spec_uint ("favorites",
                       "Favorites",
                       "Favorites",
                       0, G_MAXUINT, 0,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_override_property (object_class,
                                    PROP_ORIENTATION,
                                    "orientation");

  g_object_class_install_properties (object_class, LAST_PROP, props);

  gtk_widget_class_set_css_name (widget_class, "appcarouselindicator");
}

static void
phosh_app_carousel_indicator_init (PhoshAppCarouselIndicator *self)
{
  g_signal_connect (self, "notify::scale-factor", G_CALLBACK (favorites_surface_update), self);
  favorites_surface_update (self);
}

GtkWidget *
phosh_app_carousel_indicator_new (void)
{
  return g_object_new (PHOSH_TYPE_APP_CAROUSEL_INDICATOR, NULL);
}

HdyCarousel *
phosh_app_carousel_indicator_get_carousel (PhoshAppCarouselIndicator *self)
{
  g_return_val_if_fail (PHOSH_IS_APP_CAROUSEL_INDICATOR (self), NULL);

  return self->carousel;
}

void
phosh_app_carousel_indicator_set_carousel (PhoshAppCarouselIndicator *self,
                                            HdyCarousel               *carousel)
{
  g_return_if_fail (PHOSH_IS_APP_CAROUSEL_INDICATOR (self));
  g_return_if_fail (HDY_IS_CAROUSEL (carousel) || carousel == NULL);

  if (self->carousel == carousel)
    return;

  if (self->carousel) {
    stop_animation (self);
    g_signal_handlers_disconnect_by_func (self->carousel, gtk_widget_queue_draw, self);
    g_signal_handlers_disconnect_by_func (self->carousel, n_pages_changed_cb, self);
  }

  g_set_object (&self->carousel, carousel);

  if (self->carousel) {
    g_signal_connect_object (self->carousel, "notify::position",
                             G_CALLBACK (gtk_widget_queue_draw), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (self->carousel, "notify::n-pages",
                             G_CALLBACK (n_pages_changed_cb), self,
                             G_CONNECT_SWAPPED);
  }

  gtk_widget_queue_resize (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CAROUSEL]);
}

guint
phosh_app_carousel_indicator_get_favorites (PhoshAppCarouselIndicator *self)
{
  g_return_val_if_fail (PHOSH_IS_APP_CAROUSEL_INDICATOR (self), 0);

  return self->favorites;
}

void
phosh_app_carousel_indicator_set_favorites (PhoshAppCarouselIndicator *self,
                                            guint                      favorites)
{
  g_return_if_fail (PHOSH_IS_APP_CAROUSEL_INDICATOR (self));

  if (self->favorites == favorites)
    return;

  self->favorites = favorites;
  gtk_widget_queue_draw (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FAVORITES]);
}
