#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace natspp {

struct error : std::runtime_error { using std::runtime_error::runtime_error; };

struct options {
  std::string host = "127.0.0.1";
  std::string port = "4222";
  std::string name = "natspp";
  int handshake_timeout_ms = 5000;
  bool verbose = false;
  bool pedantic = false;
  bool headers = false; // not used in this minimal version
};

using message_handler = std::function<void(const std::string& subject,
                                           const std::string& reply,
                                           const std::string& data)>;

class client {
public:
  explicit client(options opts = {});
  ~client();

  client(const client&) = delete; client& operator=(const client&) = delete;

  // Connects to server, waits for INFO and sends CONNECT (blocks until ready or timeout).
  void connect();

  // Stops reader thread and closes socket.
  void close();

  // Block until the client stops (close() called or connection ends)
  void run_forever();

  // Publish data to a subject (optionally with reply subject). Thread-safe.
  void publish(const std::string& subject,
              const void* data, std::size_t size,
              const std::string& reply = "");
      
  // Subscribe. Returns SID; handler runs on reader thread. Thread-safe.
  int subscribe(const std::string& subject, message_handler cb,
                const std::string& queue = "");

  // Unsubscribe. Thread-safe.
  void unsubscribe(int sid, std::optional<int> max_msgs = std::nullopt);

private:
  struct impl;
  std::unique_ptr<impl> p_;
};

} // namespace natspp
