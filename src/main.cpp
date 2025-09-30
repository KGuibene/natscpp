#include "nats.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "message.pb.h" 

static natspp::client* g_client = nullptr;

void on_sigint(int) {
  if (g_client) {
    std::cerr << "\nSIGINT caught, closing...\n";
    g_client->close();
  }
}

int main() {
  try {
    std::signal(SIGINT, on_sigint);

    natspp::options opts;
    opts.host = "127.0.0.1";
    opts.port = "4222";
    opts.name = "natspp-min";
    opts.handshake_timeout_ms = 5000;

    natspp::client c(opts);
    g_client = &c;

    c.connect();

    int sid = c.subscribe("demo.>", [](auto subj, auto reply, auto data){
      std::cout << "[MSG] " << subj << " [" << reply << "] " << data << std::endl;
    });

    demo::Message msg;
    msg.set_content("Hello, NATS!");
    std::string buf;
    if (!msg.SerializeToString(&buf)) {
      std::cerr << "serialize failed\n";
      return 1;
    }
    c.publish("demo.hello",  buf.data(), buf.size());


  int rsid = c.respond("service.greeting",
    [](const std::string&, const std::string& data) -> std::string {
      std::string req;
      resp = "Hello " + req;
      std::string buf;
      resp.SerializeToString(&buf);
      return buf;
    });

    // Send a request and wait for the response (with timeout)
    /*
    std::string reqbuf = "Requester 1";
    try {
      auto reply = c.request("service.greeting", &reqbuf, sizeof(&reqbuf), std::chrono::milliseconds(1000));
      std::cout << "got reply bytes, size=" << reply.size() << "\n";
      std::string reply_str = reply; // raw bytes
      std::cout << "reply as string: " << reply_str << "\n";
    } catch (const natspp::error& e) {
      std::cerr << "request failed: " << e.what() << "\n";
    }*/
    // Block here until close() is called or connection ends:
    c.run_forever();
    // optional: cleanup
    c.unsubscribe(sid);
    c.close();
    return 0;



  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << std::endl;
    return 1;
  }
}
