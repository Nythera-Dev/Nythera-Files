#include "filemanager/configuration_notice.h"

#include "core/log.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <gio/gio.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace filemanager {

  namespace {
    constexpr Logger kLog("files-config");

    struct Notice {
      const char* title;
      const char* osdIcon;
      const char* desktopIcon;
      int osdHideDelayMs;
      std::string body;
    };

    std::string displayDiagnostic(std::string_view message) {
      constexpr std::string_view prefix = "Error while parsing ";
      constexpr std::string_view separator = ": ";
      if (!message.starts_with(prefix))
        return std::string(message);
      const auto scopeEnd = message.find(separator, prefix.size());
      if (scopeEnd == std::string_view::npos || scopeEnd + separator.size() == message.size())
        return std::string(message);
      const auto scope = message.substr(prefix.size(), scopeEnd - prefix.size());
      const auto reason = message.substr(scopeEnd + separator.size());

      constexpr std::string_view expectedPrefix = "expected ";
      constexpr std::string_view sawSeparator = ", saw ";
      if (reason.starts_with(expectedPrefix)) {
        const auto saw = reason.find(sawSeparator, expectedPrefix.size());
        if (saw != std::string_view::npos && saw + sawSeparator.size() < reason.size()) {
          auto expected = reason.substr(expectedPrefix.size(), saw - expectedPrefix.size());
          const auto token = reason.substr(saw + sawSeparator.size());
          if (expected.starts_with("a "))
            expected.remove_prefix(2);
          else if (expected.starts_with("an "))
            expected.remove_prefix(3);
          return "Unexpected "
              + std::string(token == "end-of-file" ? "end of file" : token)
              + "; expected "
              + std::string(expected);
        }
      }
      if (reason == "encountered end-of-file") {
        if (scope == "string" || scope == "literal string")
          return "Unterminated " + std::string(scope);
        return "Unexpected end of file in " + std::string(scope);
      }
      if (reason.starts_with("exceeds length limit"))
        return "Value " + std::string(reason) + " for " + std::string(scope);

      std::string concise(reason);
      if (concise.front() >= 'a' && concise.front() <= 'z')
        concise.front() = static_cast<char>(concise.front() - 'a' + 'A');
      return concise;
    }

    std::string formatBody(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory) {
      const std::filesystem::path file(problem.origin.file);
      const auto parent = file.empty() ? configDirectory : file.parent_path();
      std::string body = FileUtils::xdgPathForDisplay(parent.string());
      if (!body.empty()) {
        if (!body.ends_with('/'))
          body += '/';
        body += '\n';
      }
      if (!file.empty()) {
        body += file.filename().string();
        if (problem.origin.line != 0) {
          body += " · Line " + std::to_string(problem.origin.line);
          if (problem.origin.column != 0)
            body += ", column " + std::to_string(problem.origin.column);
        }
        body += '\n';
      }
      return body + "> " + displayDiagnostic(problem.message);
    }

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

    void postDesktopNotification(GDBusConnection* bus, const Notice& notice, const char* body) {
      g_dbus_connection_call(
          bus, kNotificationService, kNotificationPath, kNotificationService, "Notify",
          g_variant_new(
              "(susss@as@a{sv}i)", "Nythera Files", 0U, notice.desktopIcon, notice.title, body,
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

    void sendDesktopNotification(Notice notification) {
      g_bus_get(
          G_BUS_TYPE_SESSION, nullptr,
          +[](GObject*, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Notice> notice(static_cast<Notice*>(data));
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
                  const std::unique_ptr<Notice> pending(static_cast<Notice*>(payload));
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
          new Notice(std::move(notification))
      );
    }

    void sendOsd(GSocketConnection* connection, const Notice& notice) {
      std::string value = notice.title;
      if (!notice.body.empty())
        value += '\n' + notice.body;
      const auto payload =
          nlohmann::json{{"icon", notice.osdIcon}, {"value", value}, {"hide_delay_ms", notice.osdHideDelayMs}};
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

    void sendNotice(Notice notification) {
      auto* client = g_socket_client_new();
      g_socket_client_set_timeout(client, 1);
      auto* address = g_unix_socket_address_new(shellSocketPath().c_str());
      g_socket_client_connect_async(
          client, G_SOCKET_CONNECTABLE(address), nullptr,
          +[](GObject* source, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<Notice> notice(static_cast<Notice*>(data));
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
          new Notice(std::move(notification))
      );
      g_object_unref(address);
      g_object_unref(client);
    }
  } // namespace

  void notifyInvalidConfiguration(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory) {
    const auto formatted = formatBody(problem, configDirectory);
    auto* valid = g_utf8_make_valid(formatted.data(), static_cast<gssize>(formatted.size()));
    sendNotice({"Invalid configuration", "alert-triangle", "dialog-warning", 5000, valid});
    g_free(valid);
  }

  void notifyConfigurationReloaded() {
    sendNotice({"Config reloaded", "circle-check", "dialog-information", 1000, {}});
  }

} // namespace filemanager
