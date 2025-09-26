#include <gtest/gtest.h>
#include "nats.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace std::chrono_literals;

static std::string env_or(const char* key, const char* defv) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::string(defv);
}

TEST(Natspp, ConnectPublishSubscribe) {
  // Read host/port from env if provided by CI/script
  std::string host = env_or("NATS_HOST", "127.0.0.1");
  std::string port = env_or("NATS_PORT", "4222");

  natspp::options opts;
  opts.host = host;
  opts.port = port;
  opts.name = "gtest-natspp";
  opts.handshake_timeout_ms = 3000;

  natspp::client c(opts);

  // If connect fails (e.g. no server), skip test instead of failing the build locally
  try {
    c.connect();
  } catch (const std::exception& e) {
    GTEST_SKIP() << "Skipping: could not connect to NATS at " << host << ":" << port
                 << " (" << e.what() << ")";
  }

  // Use a unique subject to avoid interference
  std::string subject = "gtest.demo." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

  std::mutex mu;
  std::condition_variable cv;
  std::atomic<int> seen{0};
  std::string last_data;

  int sid = c.subscribe(subject, [&](auto subj, auto reply, auto data) {
    (void)subj; (void)reply;
    {
      std::lock_guard<std::mutex> lk(mu);
      last_data = data;
      seen.fetch_add(1, std::memory_order_relaxed);
    }
    cv.notify_all();
  });

  // Publish one message
  const char* msg = "hello-from-gtest";
  c.publish(subject, msg, strlen(msg));

  // Wait for it
  {
    std::unique_lock<std::mutex> lk(mu);
    bool ok = cv.wait_for(lk, 2s, [&]{ return seen.load() >= 1; });
    EXPECT_TRUE(ok) << "Did not receive message within timeout";
    EXPECT_EQ(last_data, "hello-from-gtest");
  }

  // Ensure unsubscribe prevents further deliveries
  c.unsubscribe(sid);
  const char* msg2 = "second";
  c.publish(subject, msg2, strlen(msg2));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(seen.load(), 1) << "Received message after unsubscribe";

  c.close();
}

TEST(Natspp, UnsubscribeBeforeConnectIsError) {
  natspp::client c;
  EXPECT_THROW(c.unsubscribe(1), natspp::error);
}
