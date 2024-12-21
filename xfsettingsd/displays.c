/*
 *  Copyright (c) 2008 Nick Schermer <nick@xfce.org>
 *  Copyright (C) 2010-2012 Lionel Le Folgoc <lionel@lefolgoc.net>
 *  Copyright (C) 2023 Gaël Bonithon <gael@xfce.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "displays.h"

#ifdef HAVE_UPOWERGLIB
#include "displays-upower.h"
#endif

#ifdef HAVE_XRANDR
#include "displays-x11.h"

#include <gdk/gdkx.h>
#endif

#ifdef ENABLE_WAYLAND
#include "displays-wayland.h"

#include <gdk/gdkwayland.h>
#endif

#include "common/debug.h"
#include "common/display-profiles.h"

#include <libxfce4ui/libxfce4ui.h>


#define get_instance_private(instance) \
    ((XfceDisplaysHelperPrivate *) xfce_displays_helper_get_instance_private (XFCE_DISPLAYS_HELPER (instance)))

static void
xfce_displays_helper_constructed (GObject *object);
static void
xfce_displays_helper_finalize (GObject *object);



typedef struct _XfceDisplaysHelperPrivate
{
    XfconfChannel *channel;
#ifdef HAVE_UPOWERGLIB
    XfceDisplaysUPower *power;
#endif
    guint iio_watch_id;
    GDBusProxy *iio_proxy;
} XfceDisplaysHelperPrivate;



G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (XfceDisplaysHelper, xfce_displays_helper, G_TYPE_OBJECT);



static void
xfce_displays_helper_class_init (XfceDisplaysHelperClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->constructed = xfce_displays_helper_constructed;
    gobject_class->finalize = xfce_displays_helper_finalize;
}



static void
xfce_displays_helper_init (XfceDisplaysHelper *helper)
{
}



static void
xfce_displays_helper_channel_property_changed (XfconfChannel *channel,
                                               const gchar *property_name,
                                               const GValue *value,
                                               XfceDisplaysHelper *helper)
{
    if (G_UNLIKELY (G_VALUE_HOLDS_STRING (value) && g_strcmp0 (property_name, APPLY_SCHEME_PROP) == 0))
    {
        /* apply */
        XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->channel_apply (helper, g_value_get_string (value));
        /* remove the apply property */
        xfconf_channel_reset_property (channel, APPLY_SCHEME_PROP, FALSE);
    }
}

static void
xfce_displays_helper_iio_sensor_apply_rotation (XfceDisplaysHelper *helper,
                                                gint                rotation)
{
    XfconfChannel *channel = xfce_displays_helper_get_channel (helper);
    GHashTable *props;
    GHashTableIter iter;
    void *key, *value;
    gboolean need_apply = FALSE;

    props = xfconf_channel_get_properties (channel, NULL);
    if (G_LIKELY (props != NULL))
    {
        g_hash_table_iter_init (&iter, props);
        while (g_hash_table_iter_next (&iter, &key, &value))
        {
            if (g_str_has_suffix (key, "Rotation"))
            {
                if (rotation != g_value_get_int(value))
                {
                    xfconf_channel_set_int (channel, key, rotation);
                    need_apply = TRUE;
                }
            }
        }
        g_hash_table_destroy (props);
    }

    /* Apply orientation changes */
    if (need_apply)
        xfconf_channel_set_string (channel, APPLY_SCHEME_PROP, DEFAULT_SCHEME_NAME);
}

static void
xfce_displays_helper_iio_sensor_properties_changed (GDBusProxy *proxy,
                                                    GVariant   *changed_properties,
                                                    GStrv       invalidated_properties,
                                                    XfceDisplaysHelper *helper)
{
    GVariant *v;
    GVariantDict dict;
    const gchar *orientation = NULL;
    gint rotation = 0;

    g_variant_dict_init (&dict, changed_properties);

    if (g_variant_dict_contains (&dict, "AccelerometerOrientation"))
    {
        v = g_dbus_proxy_get_cached_property (helper->iio_proxy, "AccelerometerOrientation");
        orientation = g_variant_get_string (v, NULL);
        g_variant_unref (v);

        if (g_strcmp0 (orientation, "normal") == 0){
            rotation = 0;
            sprintf(command, "xinput set-prop \"%s\" \"Coordinate Transformation Matrix\" %s", "pointer:Goodix Capacitive TouchScreen", "1 0 0 0 1 0 0 0 1");
            system(command);
            }
        else if (g_strcmp0 (orientation, "bottom-up") == 0){
            rotation = 180;
            sprintf(command, "xinput set-prop \"%s\" \"Coordinate Transformation Matrix\" %s", "pointer:Goodix Capacitive TouchScreen", "-1 0 1 0 -1 1 0 0 1");
            system(command);
            }
        else if (g_strcmp0 (orientation, "left-up") == 0){
            rotation = 90;
            sprintf(command, "xinput set-prop \"%s\" \"Coordinate Transformation Matrix\" %s", "pointer:Goodix Capacitive TouchScreen", "0 -1 1 1 0 0 0 0 1");
            system(command);
            }
        else if (g_strcmp0 (orientation, "right-up") == 0){
            rotation = 270;
            sprintf(command, "xinput set-prop \"%s\" \"Coordinate Transformation Matrix\" %s", "pointer:Goodix Capacitive TouchScreen", "0 1 0 -1 0 1 0 0 1");
            system(command);
            }
        else
            return; // Handle "undefined" case.

        xfce_displays_helper_iio_sensor_apply_rotation (helper, rotation);
    }
}

static void
xfce_displays_helper_iio_sensor_appeared_cb (GDBusConnection *connection,
                                             const gchar     *name,
                                             const gchar     *name_owner,
                                             XfceDisplaysHelper *helper)
{
    GError *error = NULL;

    helper->iio_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       NULL,
                                                       "net.hadess.SensorProxy",
                                                       "/net/hadess/SensorProxy",
                                                       "net.hadess.SensorProxy",
                                                       NULL, NULL);

    g_signal_connect (G_OBJECT (helper->iio_proxy), "g-properties-changed",
                      G_CALLBACK (xfce_displays_helper_iio_sensor_properties_changed), helper);

    /* Accelerometer */
    g_dbus_proxy_call_sync (helper->iio_proxy,
                            "ClaimAccelerometer", NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL, &error);
    g_assert_no_error (error);
}

static void
xfce_displays_helper_iio_sensor_vanished_cb (GDBusConnection *connection,
                                             const gchar     *name,
                                             XfceDisplaysHelper *helper)
{
    if (helper->iio_proxy) {
        g_clear_object (&helper->iio_proxy);
    }
}

static void
xfce_displays_helper_constructed (GObject *object)
{
    XfceDisplaysHelper *helper = XFCE_DISPLAYS_HELPER (object);
    XfceDisplaysHelperPrivate *priv = get_instance_private (object);

    /* if X11/Wayland impl init suceeded */
    if (XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->get_outputs (helper) != NULL)
    {
        gchar *matching_profile;
        gint mode;

#ifdef HAVE_UPOWERGLIB
        priv->power = g_object_new (XFCE_TYPE_DISPLAYS_UPOWER, NULL);
        g_signal_connect (G_OBJECT (priv->power),
                          "lid-changed",
                          G_CALLBACK (XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->toggle_internal),
                          helper);
#endif


        priv->iio_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                                 "net.hadess.SensorProxy",
                                                 G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                 xfce_displays_helper_iio_sensor_appeared_cb,
                                                 xfce_displays_helper_iio_sensor_vanished_cb,
                                                 helper, NULL);

        /* open the channel */
        priv->channel = xfconf_channel_get ("displays");

        /* remove any leftover apply property before setting the monitor */
        xfconf_channel_reset_property (priv->channel, APPLY_SCHEME_PROP, FALSE);

        /* monitor channel changes */
        g_signal_connect_object (G_OBJECT (priv->channel),
                                 "property-changed",
                                 G_CALLBACK (xfce_displays_helper_channel_property_changed),
                                 helper, 0);

        /*  check if we can auto-enable a profile */
        matching_profile = xfce_displays_helper_get_matching_profile (helper);
        mode = xfconf_channel_get_int (priv->channel, AUTO_ENABLE_PROFILES, AUTO_ENABLE_PROFILES_DEFAULT);
        if (matching_profile != NULL && (mode == AUTO_ENABLE_PROFILES_ON_CONNECT || mode == AUTO_ENABLE_PROFILES_ALWAYS))
        {
            XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->channel_apply (helper, matching_profile);
        }
        else
        {
            XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->channel_apply (helper, DEFAULT_SCHEME_NAME);
        }
        g_free (matching_profile);
    }

    G_OBJECT_CLASS (xfce_displays_helper_parent_class)->constructed (object);
}



static void
xfce_displays_helper_finalize (GObject *object)
{
#ifdef HAVE_UPOWERGLIB
    XfceDisplaysHelperPrivate *priv = get_instance_private (object);
    if (priv->power != NULL)
        g_object_unref (priv->power);
#endif

    G_OBJECT_CLASS (xfce_displays_helper_parent_class)->finalize (object);
}



GObject *
xfce_displays_helper_new (void)
{
#ifdef HAVE_XRANDR
    if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
        return g_object_new (XFCE_TYPE_DISPLAYS_HELPER_X11, NULL);
#endif
#ifdef ENABLE_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY (gdk_display_get_default ()))
        return g_object_new (XFCE_TYPE_DISPLAYS_HELPER_WAYLAND, NULL);
#endif

    g_critical ("Display settings are not supported on this windowing environment");

    return NULL;
}



gchar *
xfce_displays_helper_get_matching_profile (XfceDisplaysHelper *helper)
{
    XfceDisplaysHelperPrivate *priv = get_instance_private (helper);
    GList *profiles = NULL;
    gchar **display_infos = XFCE_DISPLAYS_HELPER_GET_CLASS (helper)->get_display_infos (helper);
    gchar *profile = NULL;
    gboolean default_matches = FALSE;

    if (display_infos != NULL)
    {
        profiles = display_settings_get_profiles (display_infos, priv->channel, TRUE);
        default_matches = display_settings_profile_matches ("Default", display_infos, priv->channel);
        if (profiles == NULL && default_matches)
        {
            /* if user profile matching failed, use Default if possible */
            profiles = g_list_prepend (profiles, g_strdup ("Default"));
        }
        g_strfreev (display_infos);
    }

    if (profiles == NULL)
    {
        xfsettings_dbg (XFSD_DEBUG_DISPLAYS, "No matching display profiles found.");
    }
    else if (g_list_length (profiles) == 1)
    {
        profile = g_strdup (profiles->data);
        xfsettings_dbg (XFSD_DEBUG_DISPLAYS, "Applied the only matching display profile: %s", profile);
    }
    else
    {
        if (default_matches)
        {
            profile = g_strdup ("Default");
            xfsettings_dbg (XFSD_DEBUG_DISPLAYS, "Found %d matching display profiles, applying %s",
                            g_list_length (profiles) + 1, profile);
        }
        else
        {
            xfsettings_dbg (XFSD_DEBUG_DISPLAYS, "Found %d matching display profiles, unable to choose",
                            g_list_length (profiles));
        }
    }

    g_list_free_full (profiles, g_free);

    return profile;
}



XfconfChannel *
xfce_displays_helper_get_channel (XfceDisplaysHelper *helper)
{
    return get_instance_private (helper)->channel;
}
