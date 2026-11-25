#include "config/config_service.h"
#include "filemanager/file_manager_settings.h"
#include "test_isolation.h"

#include <chrono>
#include <fstream>
#include <gio/gio.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  void write(const fs::path& path, std::string_view content) {
    std::ofstream file(path);
    TEST_CHECK(file << content);
  }

  void drainFor(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    do {
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      g_usleep(1000);
    } while (std::chrono::steady_clock::now() < until);
  }

  void waitFor(const auto& ready) {
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (!ready() && std::chrono::steady_clock::now() < until)
      drainFor(2ms);
    TEST_CHECK(ready());
  }

  struct Request {
    std::string command;
    int fd = -1;
  };

  Request acceptRequest(int server) {
    Request request;
    waitFor([&] {
      return request.fd >= 0 || (request.fd = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0;
    });
    waitFor([&] {
      char bytes[4096];
      const auto count = ::read(request.fd, bytes, sizeof(bytes));
      if (count > 0)
        request.command.append(bytes, static_cast<std::size_t>(count));
      return count == 0;
    });
    return request;
  }

  bool isProbe(const Request& request) { return request.command == "config-monitor-status files"; }

  bool hasPendingConnection(int server) {
    drainFor(150ms);
    const int connection = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (connection >= 0)
      ::close(connection);
    return connection >= 0;
  }

  // Files asks Shell who owns the alert first. `probeReply` models Shell's answer; nullopt never answers.
  nlohmann::json receiveOsd(int server, std::optional<std::string_view> probeReply = "unmonitored\n") {
    auto probe = acceptRequest(server);
    const auto releaseProbe = nythera::test::makeScopeExit([&] { ::close(probe.fd); });
    TEST_CHECK(isProbe(probe));
    if (probeReply && !probeReply->empty())
      TEST_CHECK(::write(probe.fd, probeReply->data(), probeReply->size()) == static_cast<ssize_t>(probeReply->size()));
    if (probeReply) {
      ::close(probe.fd);
      probe.fd = -1;
    }
    auto request = acceptRequest(server);
    const auto releaseRequest = nythera::test::makeScopeExit([&] { ::close(request.fd); });
    TEST_CHECK(request.command.starts_with("osd-show "));
    return nlohmann::json::parse(request.command.substr(9));
  }

  struct Notifications {
    bool markup = false;
    std::vector<std::pair<std::string, std::string>> messages;
  };

  void notificationCall(
      GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* args,
      GDBusMethodInvocation* reply, gpointer data
  ) {
    auto& notifications = *static_cast<Notifications*>(data);
    if (std::string_view(method) == "GetCapabilities") {
      const char* caps[] = {"body", "body-markup", nullptr};
      g_dbus_method_invocation_return_value(
          reply, g_variant_new("(@as)", g_variant_new_strv(caps, notifications.markup ? 2 : 1))
      );
      return;
    }
    const char* title = nullptr;
    const char* body = nullptr;
    g_variant_get_child(args, 3, "&s", &title);
    g_variant_get_child(args, 4, "&s", &body);
    notifications.messages.emplace_back(title, body);
    g_dbus_method_invocation_return_value(reply, g_variant_new("(u)", 1U));
  }

  void checkDeliveryAndRecovery(const nythera::test::ScopedTestEnv& env) {
    const auto socketPath = env.runtime() / "nythera-test-display.sock";
    const nythera::test::ScopedEnvVar display("WAYLAND_DISPLAY", "test-display");
    const auto configDir = env.config() / "nythera/files";
    TEST_CHECK(fs::create_directories(configDir));
    const auto included = env.work() / "extra.toml";
    write(configDir / "config.toml", "[include]\nfiles = ['$HOME/../work/extra.toml']\n");
    write(included, "[files]\nshow_hidden = true\n");

    Notifications notifications;
    auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    TEST_CHECK(bus != nullptr);
    const auto releaseBus = nythera::test::makeScopeExit([&] { g_object_unref(bus); });
    constexpr auto interface = "org.freedesktop.Notifications";
    auto* name = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", interface, 4U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr
    );
    TEST_CHECK(name != nullptr);
    guint32 acquired = 0;
    g_variant_get(name, "(u)", &acquired);
    g_variant_unref(name);
    TEST_CHECK(acquired == 1);
    auto* xml = g_dbus_node_info_new_for_xml(
        R"(<node><interface name="org.freedesktop.Notifications">
        <method name="GetCapabilities"><arg type="as" direction="out"/></method>
        <method name="Notify">
        <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
        <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
        <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/>
        </method></interface></node>)",
        nullptr
    );
    TEST_CHECK(xml != nullptr);
    const auto releaseXml = nythera::test::makeScopeExit([&] { g_dbus_node_info_unref(xml); });
    const GDBusInterfaceVTable vtable{.method_call = notificationCall};
    const auto registration = g_dbus_connection_register_object(
        bus, "/org/freedesktop/Notifications", xml->interfaces[0], &vtable, &notifications, nullptr, nullptr
    );
    TEST_CHECK(registration);
    const auto unregister =
        nythera::test::makeScopeExit([&] { g_dbus_connection_unregister_object(bus, registration); });

    int server = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    TEST_CHECK(server >= 0);
    const auto releaseServer = nythera::test::makeScopeExit([&] {
      if (server >= 0)
        ::close(server);
    });
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    TEST_CHECK(socketPath.native().size() < sizeof(address.sun_path));
    socketPath.native().copy(address.sun_path, sizeof(address.sun_path) - 1);
    TEST_CHECK(::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    TEST_CHECK(::listen(server, 4) == 0);

    ConfigService shell(ConfigService::SettingsAccess::InheritanceOnly);
    filemanager::FileManagerSettings settings(shell);
    TEST_CHECK(settings.loadFileView().showHidden);
    int reports = 0;
    std::string rawDiagnostic;
    settings.setProblemCallback([&](const auto& problem, const auto& directory) {
      ++reports;
      rawDiagnostic = problem.message;
      filemanager::notifyInvalidConfiguration(problem, directory);
    });
    int reloads = 0;
    settings.setManualReloadCallback([&] {
      ++reloads;
      filemanager::notifyConfigurationReloaded();
    });
    const auto dispatch = [&] {
      std::vector<pollfd> fds;
      const auto idx = settings.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 1000) == 1);
      settings.dispatch(fds, idx);
    };
    constexpr auto invalid = "[files]\nshow_hidden = true g\n";
    write(included, invalid);
    dispatch();
    TEST_CHECK(settings.loadFileView().showHidden && reports == 1);
    TEST_CHECK(!settings.reload() && reports == 1 && reloads == 0);
    TEST_CHECK(rawDiagnostic == "Error while parsing key-value pair: expected a comment or line break, saw 'g'");
    const auto payload = receiveOsd(server);
    TEST_CHECK(payload.at("hide_delay_ms") == 5000);
    const auto text = payload.at("value").get<std::string>();
    TEST_CHECK(text.starts_with(
        "Invalid configuration\n" + included.parent_path().string() + "/\nextra.toml · Line 2, column "
    ));
    TEST_CHECK(text.ends_with("\n> Unexpected 'g'; expected comment or line break"));
    drainFor(50ms);
    TEST_CHECK(notifications.messages.empty());

    // Recovery can resolve to the same effective preferences. The same error
    // after recovery should alert again, through the standalone service this time.
    write(included, "[files]\nshow_hidden = true\n");
    dispatch();
    TEST_CHECK(settings.loadFileView().showHidden && reports == 1 && reloads == 1);
    const auto recovered = receiveOsd(server);
    TEST_CHECK(recovered.at("value") == "Config reloaded");
    TEST_CHECK(recovered.at("icon") == "circle-check");
    TEST_CHECK(recovered.at("hide_delay_ms") == 1000);
    drainFor(50ms);
    TEST_CHECK(notifications.messages.empty());

    // Shell owns the alert while it monitors Files: Files must stay silent (no OSD, no desktop notice).
    filemanager::notifyConfigurationReloaded();
    {
      auto probe = acceptRequest(server);
      const auto releaseProbe = nythera::test::makeScopeExit([&] { ::close(probe.fd); });
      TEST_CHECK(isProbe(probe));
      TEST_CHECK(::write(probe.fd, "monitored\n", 10) == 10);
    }
    TEST_CHECK(!hasPendingConnection(server));
    TEST_CHECK(notifications.messages.empty());

    // An unresponsive or non-answering Shell must not swallow the alert, and must not duplicate it.
    filemanager::notifyConfigurationReloaded();
    TEST_CHECK(receiveOsd(server, std::nullopt).at("hide_delay_ms") == 1000);
    TEST_CHECK(!hasPendingConnection(server));
    filemanager::notifyConfigurationReloaded();
    TEST_CHECK(receiveOsd(server, "").at("value") == "Config reloaded");
    filemanager::notifyConfigurationReloaded();
    TEST_CHECK(receiveOsd(server, "error: unknown command\n").at("value") == "Config reloaded");
    TEST_CHECK(!hasPendingConnection(server));
    TEST_CHECK(notifications.messages.empty());

    ::close(server);
    server = -1; // Leave the stale socket path present.
    write(included, invalid);
    TEST_CHECK(!settings.reload() && settings.loadFileView().showHidden);
    TEST_CHECK(!settings.reload() && reports == 2);
    waitFor([&] { return !notifications.messages.empty(); });
    TEST_CHECK(notifications.messages.size() == 1);
    TEST_CHECK(notifications.messages[0].first == "Invalid configuration");
    TEST_CHECK(text == notifications.messages[0].first + "\n" + notifications.messages[0].second);
    dispatch();
    TEST_CHECK(reloads == 1 && reports == 2);

    write(included, "[files]\nshow_hidden = false\n");
    dispatch();
    waitFor([&] { return notifications.messages.size() == 2; });
    TEST_CHECK(notifications.messages[1].first == "Config reloaded" && notifications.messages[1].second.empty());
    TEST_CHECK(!settings.loadFileView().showHidden && reloads == 2);
    const auto previous = settings.loadFileView();
    auto next = previous;
    next.showHidden = true;
    TEST_CHECK(settings.saveFileView(next, previous));
    dispatch();
    TEST_CHECK(reloads == 2);
    drainFor(50ms);
    TEST_CHECK(notifications.messages.size() == 2);

    // A new settings instance must also report a startup error after UI setup.
    notifications.markup = true;
    write(included, "[files]\nshow_hidden = \"oops");
    filemanager::FileManagerSettings startup(shell);
    startup.setProblemCallback(filemanager::notifyInvalidConfiguration);
    waitFor([&] { return notifications.messages.size() == 3; });
    TEST_CHECK(notifications.messages[2].second.ends_with("\n&gt; Unterminated string"));

    notifications.markup = false;
    write(included, "[files]\nshow_hidden : true\n");
    TEST_CHECK(!startup.reload());
    waitFor([&] { return notifications.messages.size() == 4; });
    TEST_CHECK(notifications.messages[3].second.ends_with("\n> Unexpected ':'; expected '='"));

    write(included, "[files]\nshow_hidden = [true false]\n");
    TEST_CHECK(!startup.reload());
    waitFor([&] { return notifications.messages.size() == 5; });
    TEST_CHECK(notifications.messages[4].second.ends_with("\n> Unexpected 'f'; expected comma or closing ']'"));

    write(included, "[files]\nshow_hidden = false\n");
    TEST_CHECK(startup.reload() && !startup.loadFileView().showHidden);

    write(included, "[files.saved_places.example]\npath = 'relative'\n");
    write(
        configDir / "config.toml",
        "[include]\nfiles = ['$HOME/../work/extra.toml']\n[files.saved_places.example]\nlabel = 'Example'\n"
    );
    TEST_CHECK(!startup.reload() && !startup.loadFileView().showHidden);
    waitFor([&] { return notifications.messages.size() == 6; });
    TEST_CHECK(notifications.messages[5].second.contains("extra.toml · Line 2, column "));
    TEST_CHECK(notifications.messages[5].second.contains("files.saved_places.example.path"));
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env(
        {.prefix = "files-config-notice", .isolateDbusSession = false, .requirePrivateDbusSession = true}
    );
    checkDeliveryAndRecovery(env);
  });
}
