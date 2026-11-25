#include "filemanager/configuration_notice.h"

#include "core/log.h"

#include <cstdlib>
#include <gio/gio.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace filemanager {

  namespace {
    constexpr Logger kLog("files-config");
    constexpr std::string_view kMonitorStatusQuery = "config-monitor-status files";

    std::string shellSocketPath() {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const char* display = std::getenv("WAYLAND_DISPLAY");
      return std::string(runtime && *runtime ? runtime : "/tmp")
          + "/nythera-"
          + (display && *display ? display : "wayland-0")
          + ".sock";
    }

    constexpr auto kNotificationService = "org.freedesktop.Notifications";
    constexpr auto kNotificationPath = "/org/freedesktop/Notifications";

    void postDesktopNotification(
        GDBusConnection* bus, const nythera::config::ConfigurationNotice& notice, const char* body
    ) {
      g_dbus_connection_call(
          bus, kNotificationService, kNotificationPath, kNotificationService, "Notify",
          g_variant_new(
              "(susss@as@a{sv}i)", "Nythera Files", 0U, notice.desktopIcon.c_str(), notice.title.c_str(), body,
              g_variant_new_strv(nullptr, 0), g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0), -1
          ),
          G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
          +[](GObject* source, GAsyncResult* reply, gpointer) {
            GError* error = nullptr;
            if (auto* response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), reply, &error)) {
              g_variant_unref(response);
            } else {
              kLog.warn("Cannot show the desktop notification: {}", error->message);
              g_clear_error(&error);
            }
          },
          nullptr
      );
    }

    void sendDesktopNotification(nythera::config::ConfigurationNotice notification) {
      g_bus_get(
          G_BUS_TYPE_SESSION, nullptr,
          +[](GObject*, GAsyncResult* result, gpointer data) {
            std::unique_ptr<nythera::config::ConfigurationNotice> notice(
                static_cast<nythera::config::ConfigurationNotice*>(data)
            );
            GError* error = nullptr;
            auto* bus = g_bus_get_finish(result, &error);
            if (bus == nullptr) {
              kLog.warn("Cannot connect to the desktop notification service: {}", error->message);
              g_clear_error(&error);
              return;
            }
            if (notice->body.empty()) {
              postDesktopNotification(bus, *notice, "");
              g_object_unref(bus);
              return;
            }
            g_dbus_connection_call(
                bus, kNotificationService, kNotificationPath, kNotificationService, "GetCapabilities", nullptr,
                G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
                +[](GObject* source, GAsyncResult* reply, gpointer payload) {
                  const std::unique_ptr<nythera::config::ConfigurationNotice> pending(
                      static_cast<nythera::config::ConfigurationNotice*>(payload)
                  );
                  auto* connection = G_DBUS_CONNECTION(source);
                  auto* response = g_dbus_connection_call_finish(connection, reply, nullptr);
                  gchar* escaped = nullptr;
                  if (response != nullptr) {
                    const gchar** capabilities = nullptr;
                    g_variant_get(response, "(^a&s)", &capabilities);
                    if (g_strv_contains(capabilities, "body-markup")) {
                      escaped = g_markup_escape_text(pending->body.data(), static_cast<gssize>(pending->body.size()));
                    }
                    g_free(capabilities);
                    g_variant_unref(response);
                  }
                  postDesktopNotification(connection, *pending, escaped ? escaped : pending->body.c_str());
                  g_free(escaped);
                },
                notice.release()
            );
            g_object_unref(bus);
          },
          new nythera::config::ConfigurationNotice(std::move(notification))
      );
    }

    void sendOsd(GSocketConnection* connection, const nythera::config::ConfigurationNotice& notice) {
      std::string value = notice.title;
      if (!notice.body.empty()) {
        value += '\n' + notice.body;
      }
      const auto payload = nlohmann::json{
          {"icon", notice.osdIcon},
          {"value", value},
          {"hide_delay_ms", notice.osdHideDelayMs},
      };
      const auto command = "osd-show " + payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      GError* error = nullptr;
      auto* output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
      if (!g_output_stream_write_all(output, command.data(), command.size(), nullptr, nullptr, &error)) {
        kLog.warn("Cannot send the configuration notice to Nythera Shell: {}", error->message);
        g_clear_error(&error);
      }
      // Shell processes a command once the client closes the write side.
      g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
    }

    void dispatchOsdToShell(nythera::config::ConfigurationNotice notification) {
      auto* client = g_socket_client_new();
      g_socket_client_set_timeout(client, 1);
      auto* address = g_unix_socket_address_new(shellSocketPath().c_str());
      g_socket_client_connect_async(
          client, G_SOCKET_CONNECTABLE(address), nullptr,
          +[](GObject* source, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<nythera::config::ConfigurationNotice> notice(
                static_cast<nythera::config::ConfigurationNotice*>(data)
            );
            GError* error = nullptr;
            auto* connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
            if (connection != nullptr) {
              sendOsd(connection, *notice);
              g_object_unref(connection);
            } else if (
                g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED)
            ) {
              sendDesktopNotification(std::move(*notice));
            } else {
              kLog.warn("Cannot contact the Nythera Shell OSD: {}", error->message);
            }
            g_clear_error(&error);
          },
          new nythera::config::ConfigurationNotice(std::move(notification))
      );
      g_object_unref(address);
      g_object_unref(client);
    }

    // A hung or half-dead Shell must not swallow the alert, so the probe is bounded.
    constexpr guint kMonitorStatusTimeoutMs = 1000;

    gboolean cancelProbe(gpointer data) {
      g_cancellable_cancel(G_CANCELLABLE(data));
      return G_SOURCE_REMOVE;
    }

    struct StatusProbeState {
      nythera::config::ConfigurationNotice notice;
      GSocketConnection* connection = nullptr;
      GCancellable* cancellable = nullptr;
      GSource* timeout = nullptr;
      char buffer[256]{};
      std::string reply;

      StatusProbeState() = default;
      StatusProbeState(const StatusProbeState&) = delete;
      StatusProbeState& operator=(const StatusProbeState&) = delete;

      void startTimeout() {
        cancellable = g_cancellable_new();
        timeout = g_timeout_source_new(kMonitorStatusTimeoutMs);
        g_source_set_callback(timeout, cancelProbe, g_object_ref(cancellable), g_object_unref);
        g_source_attach(timeout, nullptr);
      }

      ~StatusProbeState() {
        if (timeout != nullptr) {
          g_source_destroy(timeout);
          g_source_unref(timeout);
        }
        if (cancellable != nullptr)
          g_object_unref(cancellable);
        if (connection != nullptr) {
          g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
          g_object_unref(connection);
        }
      }
    };

    void readMonitorStatusChunk(std::unique_ptr<StatusProbeState> state) {
      auto* raw = state.release();
      auto* input = g_io_stream_get_input_stream(G_IO_STREAM(raw->connection));
      g_input_stream_read_async(
          input, raw->buffer, sizeof(raw->buffer), G_PRIORITY_DEFAULT, raw->cancellable,
          +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<StatusProbeState> pending(static_cast<StatusProbeState*>(data));
            GError* error = nullptr;
            const gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
            if (n > 0) {
              pending->reply.append(pending->buffer, static_cast<std::size_t>(n));
              if (pending->reply.size() < 256) {
                readMonitorStatusChunk(std::move(pending));
                return;
              }
            } else if (n < 0) {
              kLog.warn("Failed reading Shell config monitor status: {}", error->message);
              g_clear_error(&error);
              dispatchOsdToShell(std::move(pending->notice));
              return;
            }

            std::string_view reply(pending->reply);
            while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r' || reply.back() == ' ')) {
              reply.remove_suffix(1);
            }
            if (reply == "monitored") {
              // Shell owns configuration alerts while monitoring "files".
              return;
            }
            dispatchOsdToShell(std::move(pending->notice));
          },
          raw
      );
    }

    void sendNotice(nythera::config::ConfigurationNotice notification) {
      auto* client = g_socket_client_new();
      g_socket_client_set_timeout(client, 1);
      auto* address = g_unix_socket_address_new(shellSocketPath().c_str());
      g_socket_client_connect_async(
          client, G_SOCKET_CONNECTABLE(address), nullptr,
          +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<nythera::config::ConfigurationNotice> notice(
                static_cast<nythera::config::ConfigurationNotice*>(data)
            );
            GError* error = nullptr;
            auto* connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
            if (connection == nullptr) {
              if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                  || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED)) {
                sendDesktopNotification(std::move(*notice));
              } else {
                kLog.warn("Cannot contact Nythera Shell: {}", error->message);
              }
              g_clear_error(&error);
              return;
            }

            auto state = std::make_unique<StatusProbeState>();
            state->notice = std::move(*notice);
            state->connection = connection;
            state->startTimeout();
            auto* raw = state.release();
            auto* output = g_io_stream_get_output_stream(G_IO_STREAM(raw->connection));
            g_output_stream_write_all_async(
                output, kMonitorStatusQuery.data(), kMonitorStatusQuery.size(), G_PRIORITY_DEFAULT, raw->cancellable,
                +[](GObject* stream, GAsyncResult* writeResult, gpointer payload) {
                  std::unique_ptr<StatusProbeState> pending(static_cast<StatusProbeState*>(payload));
                  GError* writeError = nullptr;
                  gsize written = 0;
                  if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), writeResult, &written, &writeError)) {
                    kLog.warn("Failed querying Shell config monitor status: {}", writeError->message);
                    g_clear_error(&writeError);
                    dispatchOsdToShell(std::move(pending->notice));
                    return;
                  }
                  if (!g_socket_shutdown(
                          g_socket_connection_get_socket(pending->connection), FALSE, TRUE, &writeError
                      )) {
                    kLog.warn("Failed shutting down Shell query stream: {}", writeError->message);
                    g_clear_error(&writeError);
                    dispatchOsdToShell(std::move(pending->notice));
                    return;
                  }
                  readMonitorStatusChunk(std::move(pending));
                },
                raw
            );
          },
          new nythera::config::ConfigurationNotice(std::move(notification))
      );
      g_object_unref(address);
      g_object_unref(client);
    }
  } // namespace

  void notifyInvalidConfiguration(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory) {
    sendNotice(nythera::config::invalidConfigurationNotice(problem, configDirectory));
  }

  void notifyConfigurationReloaded() { sendNotice(nythera::config::reloadedConfigurationNotice()); }

} // namespace filemanager
