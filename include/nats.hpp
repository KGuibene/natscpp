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
using request_handler = std::function<std::string(const std::string& subject,
                                                  const std::string& payload)>;

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

  // Request/Reply (binary payload). Throws natspp::error on timeout/not connected.
  std::string request(const std::string& subject,
                      const void* data, std::size_t size,
                      std::chrono::milliseconds timeout);

  // Respond to requests on `subject`. The handler returns the response payload.
  // If the incoming message has an empty reply subject, it's ignored.


  int respond(const std::string& subject,
              request_handler handler,
              const std::string& queue = "");

private:
  struct impl;
  std::unique_ptr<impl> p_;
};

} // namespace natspp
