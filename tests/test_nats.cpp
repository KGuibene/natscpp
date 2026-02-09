#include <gtest/gtest.h>
#include "nats.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct FakeNatsServer {
  explicit FakeNatsServer(std::string info = "INFO {\"headers\":true}\r\n")
      : info_line(std::move(info)) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) throw std::runtime_error("socket failed");
    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("bind failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      throw std::runtime_error("getsockname failed");
    }
    port = ntohs(addr.sin_port);
    if (::listen(listen_fd, 1) != 0) throw std::runtime_error("listen failed");
    accept_thread = std::thread([this] {
      sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
      {
        std::lock_guard<std::mutex> lk(mu);
        client_fd = fd;
        accepted = true;
      }
      cv.notify_all();
      if (fd >= 0) {
        write_all(info_line);
      }
    });
  }

  ~FakeNatsServer() {
    if (client_fd >= 0) {
      ::shutdown(client_fd, SHUT_RDWR);
      ::close(client_fd);
    }
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    if (accept_thread.joinable()) {
      accept_thread.join();
    }
  }

  int get_port() const { return port; }

  void wait_for_client() {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, 2s, [&]{ return accepted; });
  }

  bool read_line(std::string& out, std::chrono::milliseconds timeout = 1s) {
    out.clear();
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!poll_in(deadline)) return false;
      char c{};
      ssize_t n = ::recv(client_fd, &c, 1, 0);
      if (n <= 0) return false;
      if (c == '\r') {
        char lf{};
        if (::recv(client_fd, &lf, 1, 0) != 1 || lf != '\n') return false;
        return true;
      }
      out.push_back(c);
    }
    return false;
  }

  bool read_exact(std::string& out, std::size_t bytes, std::chrono::milliseconds timeout = 1s) {
    out.assign(bytes, '\0');
    std::size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (got < bytes && std::chrono::steady_clock::now() < deadline) {
      if (!poll_in(deadline)) return false;
      ssize_t n = ::recv(client_fd, &out[got], bytes - got, 0);
      if (n <= 0) return false;
      got += static_cast<std::size_t>(n);
    }
    return got == bytes;
  }

  void write_all(const std::string& s) {
    const char* p = s.data();
    std::size_t n = s.size();
    while (n > 0) {
      ssize_t k = ::send(client_fd, p, n, 0);
      if (k <= 0) throw std::runtime_error("send failed");
      p += k;
      n -= static_cast<std::size_t>(k);
    }
  }

  void send_line(const std::string& line) { write_all(line + "\r\n"); }

  void send_msg(const std::string& subject, int sid, const std::string& payload,
                const std::string& reply = "") {
    std::ostringstream oss;
    oss << "MSG " << subject << " " << sid;
    if (!reply.empty()) oss << " " << reply;
    oss << " " << payload.size() << "\r\n" << payload << "\r\n";
    write_all(oss.str());
  }

  void send_hmsg(const std::string& subject, int sid, const std::string& headers,
                 const std::string& payload, const std::string& reply = "") {
    std::string hdr = headers;
    std::ostringstream oss;
    std::size_t total = hdr.size() + payload.size();
    oss << "HMSG " << subject << " " << sid;
    if (!reply.empty()) oss << " " << reply;
    oss << " " << total << " " << hdr.size() << "\r\n" << hdr << payload << "\r\n";
    write_all(oss.str());
  }

private:
  bool poll_in(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (ms.count() < 0) return false;
    pollfd pfd{};
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, static_cast<int>(ms.count())) > 0;
  }

  std::string info_line;
  int listen_fd{-1};
  int client_fd{-1};
  int port{0};
  std::thread accept_thread;
  std::mutex mu;
  std::condition_variable cv;
  bool accepted{false};
};

std::vector<std::string> split_ws(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> parts;
  std::string tok;
  while (iss >> tok) parts.push_back(tok);
  return parts;
}

}  // namespace

TEST(Natspp, ConnectSendsConnectAndRespondsToPing) {
  FakeNatsServer server;
  natspp::options opts;
  opts.host = "127.0.0.1";
  opts.port = std::to_string(server.get_port());
  opts.name = "gtest-natspp";
  opts.headers = true;

  natspp::client c(opts);
  c.connect();
  server.wait_for_client();

  std::string line;
  ASSERT_TRUE(server.read_line(line));
  EXPECT_TRUE(line.rfind("CONNECT ", 0) == 0);
  EXPECT_NE(line.find("\"headers\":true"), std::string::npos);

  server.send_line("PING");
  ASSERT_TRUE(server.read_line(line));
  EXPECT_EQ(line, "PONG");

  c.close();
}

TEST(Natspp, PublishSubscribeUnsubscribeAndHeaders) {
  FakeNatsServer server;
  natspp::options opts;
  opts.host = "127.0.0.1";
  opts.port = std::to_string(server.get_port());
  opts.name = "gtest-natspp";

  natspp::client c(opts);
  c.connect();
  server.wait_for_client();

  std::mutex mu;
  std::condition_variable cv;
  std::string got_payload;
  std::string got_hpayload;
  int sid = c.subscribe("demo.subject",
                        [&](const std::string&, const std::string&, const std::string& data) {
                          std::lock_guard<std::mutex> lk(mu);
                          if (got_payload.empty()) {
                            got_payload = data;
                          } else {
                            got_hpayload = data;
                          }
                          cv.notify_all();
                        },
                        "queue");

  std::string line;
  ASSERT_TRUE(server.read_line(line));
  auto parts = split_ws(line);
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "SUB");
  EXPECT_EQ(parts[1], "demo.subject");
  EXPECT_EQ(parts[2], "queue");
  EXPECT_EQ(parts[3], std::to_string(sid));

  server.send_msg("demo.subject", sid, "payload");
  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 1s, [&]{ return got_payload == "payload"; }));
  }

  std::string headers = "NATS/1.0\r\nFoo: Bar\r\n\r\n";
  server.send_hmsg("demo.subject", sid, headers, "body");
  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 1s, [&]{ return got_hpayload == "body"; }));
  }

  const char* msg = "hi";
  c.publish("demo.subject", msg, std::strlen(msg));
  ASSERT_TRUE(server.read_line(line));
  parts = split_ws(line);
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "PUB");
  EXPECT_EQ(parts[1], "demo.subject");
  EXPECT_EQ(parts[2], "2");
  std::string payload;
  ASSERT_TRUE(server.read_exact(payload, 2));
  EXPECT_EQ(payload, "hi");
  ASSERT_TRUE(server.read_exact(payload, 2));
  EXPECT_EQ(payload, "\r\n");

  c.unsubscribe(sid, 1);
  ASSERT_TRUE(server.read_line(line));
  EXPECT_EQ(line, "UNSUB " + std::to_string(sid) + " 1");

  c.close();
}

TEST(Natspp, RequestReplyAndTimeout) {
  FakeNatsServer server;
  natspp::options opts;
  opts.host = "127.0.0.1";
  opts.port = std::to_string(server.get_port());

  natspp::client c(opts);
  c.connect();
  server.wait_for_client();

  std::thread responder([&] {
    std::string line;
    ASSERT_TRUE(server.read_line(line));
    auto sub_parts = split_ws(line);
    ASSERT_GE(sub_parts.size(), 3u);
    int sid = std::stoi(sub_parts.back());

    ASSERT_TRUE(server.read_line(line));
    auto pub_parts = split_ws(line);
    ASSERT_EQ(pub_parts.size(), 4u);
    std::string reply = pub_parts[2];
    std::size_t nbytes = static_cast<std::size_t>(std::stoul(pub_parts[3]));
    std::string payload;
    ASSERT_TRUE(server.read_exact(payload, nbytes));
    ASSERT_TRUE(server.read_exact(payload, 2));
    server.send_msg(reply, sid, "response");
  });

  std::string out = c.request("demo.req", "ping", 4, 1s);
  EXPECT_EQ(out, "response");

  responder.join();

  EXPECT_THROW(c.request("demo.req", "noop", 4, 50ms), natspp::error);

  c.close();
}

TEST(Natspp, RespondPublishesToReplySubject) {
  FakeNatsServer server;
  natspp::options opts;
  opts.host = "127.0.0.1";
  opts.port = std::to_string(server.get_port());

  natspp::client c(opts);
  c.connect();
  server.wait_for_client();

  int sid = c.respond("svc.echo", [](const std::string&, const std::string& payload) {
    return std::string("echo:") + payload;
  });

  std::string line;
  ASSERT_TRUE(server.read_line(line));
  auto parts = split_ws(line);
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "SUB");
  EXPECT_EQ(parts[1], "svc.echo");
  EXPECT_EQ(parts[2], std::to_string(sid));

  server.send_msg("svc.echo", sid, "hello", "inbox.1");

  ASSERT_TRUE(server.read_line(line));
  parts = split_ws(line);
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "PUB");
  EXPECT_EQ(parts[1], "inbox.1");
  std::size_t nbytes = static_cast<std::size_t>(std::stoul(parts[2]));
  std::string payload;
  ASSERT_TRUE(server.read_exact(payload, nbytes));
  EXPECT_EQ(payload, "echo:hello");
  ASSERT_TRUE(server.read_exact(payload, 2));

  c.unsubscribe(sid);
  ASSERT_TRUE(server.read_line(line));
  EXPECT_EQ(line, "UNSUB " + std::to_string(sid));

  c.close();
}

TEST(Natspp, RunForeverUnblocksOnClose) {
  FakeNatsServer server;
  natspp::options opts;
  opts.host = "127.0.0.1";
  opts.port = std::to_string(server.get_port());

  natspp::client c(opts);
  c.connect();
  server.wait_for_client();

  std::thread t([&] { c.run_forever(); });
  std::this_thread::sleep_for(50ms);
  c.close();
  t.join();
}

TEST(Natspp, UnsubscribeBeforeConnectIsError) {
  natspp::client c;
  EXPECT_THROW(c.unsubscribe(1), natspp::error);
}

TEST(Natspp, PublishBeforeConnectIsError) {
  natspp::client c;
  const char* payload = "data";
  EXPECT_THROW(c.publish("subject", payload, std::strlen(payload)), natspp::error);
}
