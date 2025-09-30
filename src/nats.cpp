#include "nats.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#include <random>
#include <sstream>

namespace natspp {

static constexpr const char* CRLF = "\r\n";

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i=0, n=s.size();
  while (i<n) {
    while (i<n && std::isspace((unsigned char)s[i])) ++i;
    size_t j=i; while (j<n && !std::isspace((unsigned char)s[j])) ++j;
    if (j>i) out.emplace_back(s.substr(i, j-i));
    i=j;
  }
  return out;
}
static std::optional<std::string> json_get_string(const std::string& j, const char* key) {
  std::regex re("\"" + std::string(key) + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch m; if (std::regex_search(j, m, re)) return m[1].str();
  return std::nullopt;
}
static std::optional<bool> json_get_bool(const std::string& j, const char* key) {
  std::regex re("\"" + std::string(key) + "\"\\s*:\\s*(true|false)");
  std::smatch m; if (std::regex_search(j, m, re)) return m[1] == "true";
  return std::nullopt;
}
static std::string json_escape(const std::string& s) {
  std::string o; o.reserve(s.size()+8);
  for (char c: s) o += (c=='"'? "\\\"" : std::string(1,c));
  return o;
}

static std::string make_inbox() 
{
  // NATS commonly uses base32; hex is fine for uniqueness.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist;
  std::ostringstream oss;
  oss << "_INBOX.";
  for (int i = 0; i < 3; ++i) oss << std::hex << dist(rng); // ~48 hex chars
  return oss.str();
}
struct client::impl {
  options opts;

  int fd{-1};
  std::atomic<bool> running{false};
  std::thread reader;

  // handshake sync
  std::mutex ready_mu;
  std::condition_variable ready_cv;
  bool ready{false};

  // subs & write serialization
  std::mutex sub_mu;
  std::unordered_map<int, message_handler> subs;
  int next_sid{0};

  std::mutex write_mu;

  // server info (minimal)
  bool srv_headers{false};
  bool srv_tls_required{false};
  bool srv_auth_required{false};
  std::string srv_nonce;

  // --- stop/wait support (for run_forever) ---
  std::mutex stop_mu;
  std::condition_variable stop_cv;

  explicit impl(options o) : opts(std::move(o)) {}

  ~impl() { stop(); }

  void start() {
    fd = dial(opts.host.c_str(), opts.port.c_str());
    running = true;
    reader = std::thread([this]{ reader_loop(); });
  }

  void stop() {
    running = false;
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); fd = -1; }
    if (reader.joinable()) reader.join();
    // notify anyone waiting in run_forever()
    std::lock_guard<std::mutex> lk(stop_mu);
    stop_cv.notify_all();
  }
  
  static int dial(const char* host, const char* port) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, port, &hints, &res) != 0) throw error("getaddrinfo failed");
    int sock = -1;
    for (auto* p = res; p; p = p->ai_next) {
      sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (sock < 0) continue;
      if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) { freeaddrinfo(res); return sock; }
      ::close(sock); sock = -1;
    }
    freeaddrinfo(res);
    throw error("connect failed");
  }

  // -- simple blocking helpers (socket is in blocking mode) --
  bool read_line(std::string& out) {
    out.clear(); char c;
    while (true) {
      ssize_t k = ::recv(fd, &c, 1, 0);
      if (k <= 0) return false;
      if (c == '\r') {
        ssize_t k2 = ::recv(fd, &c, 1, 0);
        if (k2 <= 0 || c != '\n') return false;
        return true;
      }
      out.push_back(c);
    }
  }
  bool read_exact(std::string& out, size_t n) {
    out.resize(n);
    size_t got = 0;
    while (got < n) {
      ssize_t k = ::recv(fd, &out[got], n - got, 0);
      if (k <= 0) return false;
      got += (size_t)k;
    }
    return true;
  }
  void write_all(const std::string& s) {
    const char* p = s.data(); size_t n = s.size();
    while (n) {
      ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
      if (k <= 0) throw error("send failed");
      p += k; n -= (size_t)k;
    }
  }

  void mark_ready() {
    std::lock_guard<std::mutex> lk(ready_mu);
    ready = true;
    ready_cv.notify_all();
  }

  void reader_loop() {
    // Expect INFO
    std::string line;
    if (!read_line(line)) { running = false; return; }
    if (line.rfind("INFO ", 0) != 0) { running = false; return; }
    auto info = line.substr(5);
    if (auto v = json_get_bool(info, "headers")) srv_headers = *v;
    if (auto v = json_get_bool(info, "tls_required")) srv_tls_required = *v;
    if (auto v = json_get_bool(info, "auth_required")) srv_auth_required = *v;
    if (auto v = json_get_string(info, "nonce")) srv_nonce = *v;

    // Send CONNECT
    std::string j = "CONNECT {";
    j += "\"lang\":\"cpp\",\"version\":\"0.1\",\"name\":\""+json_escape(opts.name)+"\",";
    j += "\"verbose\":" + std::string(opts.verbose?"true":"false") + ",";
    j += "\"pedantic\":" + std::string(opts.pedantic?"true":"false");
    if (opts.headers) j += ",\"headers\":true";
    j += "}";
    j += CRLF;

    try { write_all(j); } catch (...) { running = false; return; }

    mark_ready();

    // Main loop
    while (running) {
      if (!read_line(line)) break;

      if (line == "PING") {
        try { write_all(std::string("PONG\r\n")); } catch (...) { break; }
        continue;
      }
      if (line == "+OK") continue;
      if (line.rfind("-ERR", 0) == 0) { std::cerr << line << "\n"; continue; }
      if (line.rfind("INFO ", 0) == 0) continue; // ignore async INFO (MVP)

      if (line.rfind("MSG ", 0) == 0) {
        // MSG <subject> <sid> [reply] <#bytes>
        auto parts = split_ws(line);
        if (parts.size() < 4) continue;
        std::string subject = parts[1];
        int sid = std::stoi(parts[2]);
        bool has_reply = (parts.size() == 5);
        std::string reply = has_reply ? parts[3] : "";
        size_t nbytes = std::stoul(parts[has_reply?4:3]);

        std::string payload;
        if (!read_exact(payload, nbytes)) break;
        std::string crlf;
        if (!read_exact(crlf, 2) || crlf != "\r\n") break;

        message_handler cb;
        { std::lock_guard<std::mutex> lk(sub_mu);
          auto it = subs.find(sid);
          if (it != subs.end()) cb = it->second;
        }
        if (cb) cb(subject, reply, payload);
        continue;
      }
      // Unknown line -> ignore
    }
    running = false;
    // wake run_forever()
    {
      std::lock_guard<std::mutex> lk(stop_mu);
      stop_cv.notify_all();
    }
  }

  // API called by facade (thread-safe)
  void api_publish(const std::string& subject,
                        const void* data, std::size_t size,
                        const std::string& reply) {
    std::lock_guard<std::mutex> lk(write_mu);

    // header: PUB <subject> [reply] <#bytes>\r\n
    std::string hdr = "PUB " + subject + (reply.empty() ? "" : " " + reply)
                    + " " + std::to_string(size) + "\r\n";
    write_all(hdr);

    // raw bytes
    const char* p = static_cast<const char*>(data);
    std::size_t n = size;
    while (n) {
      ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
      if (k <= 0) throw error("send failed");
      p += k; n -= static_cast<std::size_t>(k);
    }

    // trailing CRLF
    write_all(std::string("\r\n"));
  }

  int api_subscribe(const std::string& subject, message_handler cb, const std::string& queue) {
    int sid = ++next_sid;
    {
      std::lock_guard<std::mutex> lk(sub_mu);
      subs[sid] = std::move(cb);
    }
    std::lock_guard<std::mutex> lk(write_mu);
    std::string cmd = "SUB " + subject + (queue.empty() ? "" : " " + queue) + " " + std::to_string(sid) + "\r\n";
    write_all(cmd);
    return sid;
  }
  void api_unsubscribe(int sid, std::optional<int> max_msgs) {
    std::lock_guard<std::mutex> lk(write_mu);
    std::string cmd = "UNSUB " + std::to_string(sid);
    if (max_msgs) cmd += " " + std::to_string(*max_msgs);
    cmd += "\r\n";
    write_all(cmd);
  }
};

// ===== facade ================================================================

client::client(options opts) : p_(std::make_unique<impl>(std::move(opts))) {}
client::~client() { try { close(); } catch (...) {} }

void client::connect() {
  if (!p_) throw error("invalid state");
  if (p_->running) return;

  p_->start();
  // wait for INFO->CONNECT handshake
  std::unique_lock<std::mutex> lk(p_->ready_mu);
  if (!p_->ready_cv.wait_for(
          lk,
          std::chrono::milliseconds(p_->opts.handshake_timeout_ms),
          [this]{ return p_->ready; })) {
    throw error("timeout: handshake not ready (no INFO from server?)");
  }
}

void client::close() {
  if (!p_) return;
  p_->stop();
}

void client::run_forever() {
  if (!p_) throw error("invalid state");
  // If not connected yet, this would return immediately; guard that:
  if (!p_->running) throw error("not connected");

  std::unique_lock<std::mutex> lk(p_->stop_mu);
  p_->stop_cv.wait(lk, [this]{ return !p_->running; });
}


void client::publish(const std::string& subject,
                     const void* data, std::size_t size,
                     const std::string& reply) {
  if (!p_ || !p_->running) throw error("not connected");
  p_->api_publish(subject, data, size, reply);
}

int client::subscribe(const std::string& subject, message_handler cb, const std::string& queue) {
  if (!p_ || !p_->running) throw error("not connected");
  return p_->api_subscribe(subject, std::move(cb), queue);
}

void client::unsubscribe(int sid, std::optional<int> max_msgs) {
  if (!p_ || !p_->running) throw error("not connected");
  p_->api_unsubscribe(sid, max_msgs);
}


std::string client::request(const std::string& subject,
                            const void* data, std::size_t size,
                            std::chrono::milliseconds timeout) {
  if (!p_ || !p_->running) throw error("not connected");

  const std::string inbox = make_inbox();

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  std::string response;

  // 1) Subscribe to the unique reply subject first
  int sid = subscribe(inbox, [&](const std::string&, const std::string&, const std::string& payload){
    std::lock_guard<std::mutex> lk(mu);
    if (got) return;           // ignore extra replies, take the first
    response = payload;
    got = true;
    cv.notify_all();
  });

  // 2) Publish the request with the reply subject
  try {
    publish(subject, data, size, inbox);   // uses your binary publish overload
  } catch (...) {
    // ensure we don't leak the subscription if publish throws
    try { unsubscribe(sid); } catch (...) {}
    throw;
  }

  // 3) Wait for the first reply or timeout
  std::unique_lock<std::mutex> lk(mu);
  bool ok = cv.wait_for(lk, timeout, [&]{ return got; });

  // 4) Cleanup sub (best-effort)
  try { unsubscribe(sid); } catch (...) {}

  if (!ok) throw error("request timeout");
  return response;
}

int client::respond(const std::string& subject,
                    request_handler handler,
                    const std::string& queue) {
  if (!p_ || !p_->running) throw error("not connected");

  // Reuse existing subscribe; handler runs on reader thread
  return subscribe(subject,
    [this, subject, handler = std::move(handler)](const std::string& subj,
                                         const std::string& reply,
                                         const std::string& data) {
      if (reply.empty()) return;                   // no place to respond
      std::string resp;
      try {
        resp = handler(subj, data);                // user logic
      } catch (const std::exception& e) {
        // Optional: send error text back, or just drop
        resp = std::string("error: ") + e.what();
      }
      try {
        publish(subject, &reply, sizeof(reply), resp);                      // send response
      } catch (...) {
        // best-effort; ignore publish errors here
      }
    }, queue);
}



} // namespace natspp
