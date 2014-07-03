#include <vector>
#include <deque>
#include <sstream>
#include <string>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

#include <unistd.h>

#include <arpa/nameser.h>

#include <uv.h>

#include <ares.h>

#include "http-parser/http_parser.h"

#if UV_VERSION_MAJOR != 0 || UV_VERSION_MINOR != 11
#error unsupported libuv version
#endif

#define elementsof(array) (sizeof(array) / sizeof((array)[0]))

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

struct State;

struct Connection {
  Connection(State &s, std::string h, std::string p) : state(s), host_port(std::move(h)), path(std::move(p))
  {
    connect_req.data = this;
    write_req.data = this;
  }
  uv_tcp_t handle;
  uv_connect_t connect_req;
  uv_write_t write_req;
  State &state;
  const std::string host_port;
  const std::string path;
};

struct Resolution {
  const std::string host;
  const in_port_t port;
  const std::string path;
  State &state;
};

struct State {
  State() {
    uv_loop_init(&loop);
    uv_timer_init(&loop, &ares_timer);
    ares_timer.data = this;
  }

  ~State() {
    uv_loop_close(&loop);
  }

  void ares_stage();

  uv_loop_t loop;

  Ares ares;
  Ares::Channel dns;
  uv_timer_t ares_timer;
  std::vector<AresPoll> ares_polls;

  std::deque<Resolution> resolutions;
  std::deque<Connection> connections;
};

void alloc_cb(uv_handle_t* handle,
              size_t suggested_size,
              uv_buf_t* buf) {
  (void) handle;
  buf->base = reinterpret_cast<char *>(malloc(suggested_size));
  buf->len = suggested_size;
}

void read_cb(uv_stream_t* stream,
             ssize_t nread,
             const uv_buf_t* buf) {
  auto &connection = *reinterpret_cast<Connection *>(stream);
  if(nread == UV__EOF) {
    return;
  }
  if(nread < 0) {
    // TODO: nontfatal
    fprintf(stderr, "FATAL: Read error from %s: %s\n", connection.host_port.c_str(), uv_strerror(nread));
    abort();
  }
  write(STDOUT_FILENO, buf->base, nread);
  free(buf->base);
}

void write_cb(uv_write_t* req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);
  if(status < 0) {
    // TODO: nonfatal
    fprintf(stderr, "FATAL: Failed to send HTTP request to %s: %s\n", connection.host_port.c_str(), uv_strerror(status));
    abort();
  }

  uv_read_start(reinterpret_cast<uv_stream_t *>(&connection.handle), alloc_cb, read_cb);
}

void connect_cb(uv_connect_t *req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);

  if(status < 0) {
    fprintf(stderr, "WARN: Connection to %s failed: %s\n", connection.host_port.c_str(), uv_strerror(status));
  }

  printf("connected to %s\n", connection.host_port.c_str());

  uv_buf_t bufs[5];
  bufs[0].base = const_cast<char *>("GET ");
  bufs[0].len = strlen(bufs[0].base);
  bufs[1].base = const_cast<char *>(connection.path.data());
  bufs[1].len = connection.path.size();
  bufs[2].base = const_cast<char *>(" HTTP/1.1\r\nHost: ");
  bufs[2].len = strlen(bufs[2].base);
  bufs[3].base = const_cast<char *>(connection.host_port.data());
  bufs[3].len = connection.host_port.size();
  bufs[4].base = const_cast<char *>("\r\nConnection: close\r\n\r\n");
  bufs[4].len = strlen(bufs[4].base);

  uv_write(&connection.write_req, reinterpret_cast<uv_stream_t *>(&connection.handle), bufs, elementsof(bufs), write_cb);
}

void query4_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  auto &res = *reinterpret_cast<Resolution *>(arg);
  if(status != ARES_SUCCESS) {
    fprintf(stderr, "WARN: DNS resolution failed: %s\n", ares_strerror(status));
    return;
  }
  assert(abuf != nullptr && alen != 0);
  struct ares_addrttl addrs[1];
  int naddrs = elementsof(addrs);
  auto result = ares_parse_a_reply(abuf, alen, nullptr, addrs, &naddrs);
  if(result != ARES_SUCCESS) {
    fprintf(stderr, "WARN: Couldn't parse reply from DNS server: %s\n", ares_strerror(status));
    return;
  }

  {
    char addrstr[INET_ADDRSTRLEN+1];
    uv_inet_ntop(AF_INET, &addrs[0].ipaddr, addrstr, sizeof(addrstr));
    printf("resolved %s to %s\n", res.host.c_str(), addrstr);
  }

  res.state.connections.emplace_back(res.state, res.host + ":" + std::to_string(res.port), res.path);
  uv_tcp_init(&res.state.loop, &res.state.connections.back().handle);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(res.port);
  addr.sin_addr = addrs[0].ipaddr;
  uv_tcp_connect(&res.state.connections.back().connect_req, &res.state.connections.back().handle,
                 reinterpret_cast<struct sockaddr *>(&addr), connect_cb);
}

void ares_process_cb(uv_poll_t *handle, int status, int events) {
  auto &ares_poll = *reinterpret_cast<AresPoll *>(handle);
  if(status < 0) {
    // FIXME
    fprintf(stderr, "FATAL: %s\n", uv_strerror(status));
    abort();
  }
  ares_process_fd(ares_poll.channel,
                  events & UV_READABLE ? ares_poll.fd : ARES_SOCKET_BAD,
                  events & UV_WRITABLE ? ares_poll.fd : ARES_SOCKET_BAD);
  auto &state = *reinterpret_cast<State *>(handle->data);
  state.ares_stage();
}

void ares_timer_cb(uv_timer_t *timer) {
  auto &state = *reinterpret_cast<State *>(timer->data);
  ares_process_fd(state.dns.channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  state.ares_stage();
}

void State::ares_stage() {
  uv_timer_stop(&ares_timer);
  for(auto &poll : ares_polls) {
    uv_poll_stop(&poll.handle);
  }
  ares_polls.clear();

  struct timeval tv;
  if(nullptr != ares_timeout(dns.channel, nullptr, &tv)) {
    uint64_t timeout = tv.tv_usec / 1000 + tv.tv_sec * 1000;
    uv_timer_start(&ares_timer, ares_timer_cb, timeout, 0);
  }

  fd_set read_fds, write_fds;
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);
  int nfds = ares_fds(dns.channel, &read_fds, &write_fds);

  ares_polls.reserve(nfds);
  for(int fd = 0; fd < nfds; ++fd) {
    if(FD_ISSET(fd, &read_fds)) {
      ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(&loop, &ares_polls.back().handle, fd);
      ares_polls.back().handle.data = this;
      uv_poll_start(&ares_polls.back().handle, UV_READABLE, ares_process_cb);
    }

    if(FD_ISSET(fd, &write_fds)) {
      ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(&loop, &ares_polls.back().handle, fd);
      ares_polls.back().handle.data = this;
      uv_poll_start(&ares_polls.back().handle, UV_WRITABLE, ares_process_cb);
    }
  }
}

int main(int argc, char **argv) {
  State state;
  if(int err = state.ares.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 1;
  }

  if(int err = state.dns.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 2;
  }

  for(int i = 1; i < argc; ++i) {
    const char *url = argv[i];
    // TODO: Custom URL parser
    http_parser_url result {};
    if(0 != http_parser_parse_url(url, strlen(url), 0, &result)) {
      fprintf(stderr, "WARN: Skipping unparsable URL %s\n", url);
      continue;
    }

    if(result.field_set & UF_SCHEMA &&
       0 != strncmp(url + result.field_data[UF_SCHEMA].off, "http", result.field_data[UF_SCHEMA].len)) {
      fprintf(stderr, "WARN: Skipping non-http URL %s\n", url);
      continue;
    }

    if(!(result.field_set & UF_HOST)) {
      fprintf(stderr, "WARN: Skipping URL missing host?! %s\n", url);
      continue;
    }

    char hostname[NS_MAXDNAME + 1];
    if(result.field_data[UF_HOST].len > sizeof(hostname)) {
      fprintf(stderr, "WARN: Skipping excessively long domain name %s\n", url);
      continue;
    }
    memcpy(hostname, url + result.field_data[UF_HOST].off, result.field_data[UF_HOST].len);
    hostname[result.field_data[UF_HOST].len] = '\0';
    printf("resolving %s\n", hostname);
    const in_port_t port = result.port ? result.port : 80;
    std::string path = result.field_set & UF_PATH
        ? std::string(url + result.field_data[UF_PATH].off, result.field_data[UF_PATH].len)
        : "/";
    state.resolutions.emplace_back(
      Resolution{std::string(url + result.field_data[UF_HOST].off, result.field_data[UF_HOST].len), port,
            std::move(path), state});
    ares_query(state.dns.channel, hostname, ns_c_in, ns_t_a, query4_cb, &state.resolutions.back());
    //ares_query(state.dns.channel, hostname, ns_c_in, ns_t_aaaa, query6_cb, nullptr);
  }

  state.ares_stage();

  uv_run(&state.loop, UV_RUN_DEFAULT);

  return 0;
}
