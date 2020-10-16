/*
 * Copyright (C) 2020 Jolla Ltd.
 * Copyright (C) 2020 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2020 UBports Foundation
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#define GLOG_MODULE_NAME repowerd_plugin_log

#include <nfc_plugin_impl.h>
#include <nfc_manager.h>
#include <gio/gio.h>
#include <gutil_log.h>

GLOG_MODULE_DEFINE("repowerd-plugin");

enum display_events {
    DISPLAY_VALID,
    DISPLAY_STATE,
    DISPLAY_EVENT_COUNT
};

enum manager_events {
    MANAGER_ENABLED,
    MANAGER_EVENT_COUNT
};

typedef NfcPluginClass RepowerdPluginClass;
typedef struct repowerd_plugin {
    NfcPlugin parent;
    NfcManager* manager;
    GDBusConnection* connection;
    gulong manager_event_id[MANAGER_EVENT_COUNT];
    /*gulong display_event_id[DISPLAY_EVENT_COUNT];*/
    guint subscription_id;
    gboolean screen_on;
    gboolean always_on;
} RepowerdPlugin;

G_DEFINE_TYPE(RepowerdPlugin, repowerd_plugin, NFC_TYPE_PLUGIN)
#define THIS_TYPE (repowerd_plugin_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, RepowerdPlugin))

/* These need to be synchronized with the settings plugin */
#define SETTINGS_STORAGE_PATH   "/var/lib/nfcd/settings"
#define SETTINGS_GROUP          "Settings"
#define SETTINGS_KEY_ALWAYS_ON  "AlwaysOn"

static
void
repowerd_plugin_update_power(
    RepowerdPlugin* self)
{
    nfc_manager_request_power(self->manager, self->manager->enabled &&
        (self->always_on || self->screen_on));
}

static
void
repowerd_plugin_manager_state_handler(
    NfcManager* manager,
    void* plugin)
{
    repowerd_plugin_update_power(THIS(plugin));
}

static
void
repowerd_display_signal_cb(
    GDBusConnection *connection,
    const gchar *sender_name, const gchar *object_path,
    const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data)
{
    RepowerdPlugin* self = THIS(user_data);
    printf("%s: %s.%s %s\n",object_path,interface_name,signal_name,
        g_variant_print(parameters,TRUE));
    
    gint state = 0;
    gint reason = 0;
    g_variant_get(parameters, "(ii)", &state, &reason);
    self->screen_on = (state == 1);
    repowerd_plugin_update_power(self);
}

static
gboolean
repowerd_plugin_start(
    NfcPlugin* plugin,
    NfcManager* manager)
{
    RepowerdPlugin* self = THIS(plugin);

    GVERBOSE("Starting");
    GASSERT(!self->manager);
    self->manager = nfc_manager_ref(manager);
    self->manager_event_id[MANAGER_ENABLED] =
        nfc_manager_add_enabled_changed_handler(manager,
            repowerd_plugin_manager_state_handler, self);

    /* No need to track the display state if we are always on */
    if (!self->always_on) {
        GError *err = NULL;
        self->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
        if (err != NULL) {
            GVERBOSE("Failed to connect to system bus");
        }
        self->subscription_id = g_dbus_connection_signal_subscribe(
            self->connection,
            "com.canonical.Unity.Screen", 
            "com.canonical.Unity.Screen", 
            "DisplayPowerStateChange",
            "/com/canonical/Unity/Screen",
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            repowerd_display_signal_cb,
            self,
            NULL);
        self->screen_on = TRUE;
        repowerd_plugin_update_power(self);
    }
    repowerd_plugin_update_power(self);
    return TRUE;
}

static
void
repowerd_plugin_stop(
    NfcPlugin* plugin)
{
    RepowerdPlugin* self = THIS(plugin);

    GVERBOSE("Stopping");
    if (self->connection) {
        g_dbus_connection_signal_unsubscribe(self->connection,
            self->subscription_id);
        g_dbus_connection_close_sync(self->connection, NULL, NULL);
        g_object_unref(self->connection);
        self->connection = NULL;
    }
    nfc_manager_remove_all_handlers(self->manager, self->manager_event_id);
    nfc_manager_unref(self->manager);
    self->manager = NULL;
}

static
void
repowerd_plugin_init(
    RepowerdPlugin* self)
{
    GKeyFile* config = g_key_file_new();

    if (g_key_file_load_from_file(config, SETTINGS_STORAGE_PATH, 0, NULL)) {
        self->always_on = g_key_file_get_boolean(config, SETTINGS_GROUP,
            SETTINGS_KEY_ALWAYS_ON, NULL);
    }
    g_key_file_unref(config);
}

static
void
repowerd_plugin_class_init(
    RepowerdPluginClass* klass)
{
    klass->start = repowerd_plugin_start;
    klass->stop = repowerd_plugin_stop;
}

static
NfcPlugin*
repowerd_plugin_create(
    void)
{
    GDEBUG("Plugin loaded");
    return g_object_new(THIS_TYPE, NULL);
}

static GLogModule* const repowerd_plugin_logs[] = {
    &GLOG_MODULE_NAME,
    NULL
};

NFC_PLUGIN_DEFINE2(repowerd, "repowerd-based screen state tracking",
    repowerd_plugin_create, repowerd_plugin_logs, 0)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
