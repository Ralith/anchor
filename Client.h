#ifndef ANCHOR_CLIENT_H_
#define ANCHOR_CLIENT_H_

#include <string>
#include <vector>
#include <deque>
#include <cassert>

#include <arpa/inet.h>

#include <ares.h>
#include <uv.h>

#include "Connection.h"

struct Client {
  class Ares {
  public:
    ~Ares() { if(started_) ares_library_cleanup(); }

    int start() {
      int result = ares_library_init(ARES_LIB_INIT_ALL);
      assert(!started_);
      started_ = result == 0;
      return result;
    }

    class Channel {
    public:
      ares_channel channel;

      ~Channel() { if(started_) ares_destroy(channel); }

      int start() {
        assert(!started_);
        int result = ares_init(&channel);
        started_ = result == 0;
        return result;
      }

    private:
      bool started_ = false;
    };

  private:
    bool started_ = false;
  };

  struct AresPoll {
    uv_poll_t handle;
    int fd;
    ares_channel channel;
  };

  struct Resolution {
    const std::string host;
    const in_port_t port;
    const std::string path;
    Client &client;
  };

  Client() {
    uv_loop_init(&loop);
    uv_timer_init(&loop, &ares_timer);
    ares_timer.data = this;
  }

  ~Client();

  void ares_stage();

  void init_file(uint64_t size);

  void open(std::string host, in_port_t port, std::string path);

  void progress(uint64_t bytes);

  void balance_chunks();
  void schedule_work();

  uv_loop_t loop;

  Ares ares;
  Ares::Channel dns;
  uv_timer_t ares_timer;
  std::vector<AresPoll> ares_polls;

  std::deque<Resolution> resolutions;
  std::deque<Connection> connections;
  std::vector<Chunk> chunks;

  std::string file_name;
  uint64_t file_size = ~0;
  int fd = -1;
  uint8_t *file_data = nullptr;
  Stats stats;
};

#endif
