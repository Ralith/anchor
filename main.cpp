#include <vector>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

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

struct State {
  int dns_pending = 0;
  std::vector<AresPoll> ares_polls;
};

void query4_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  auto &state = *reinterpret_cast<State *>(arg);
  assert(state.dns_pending != 0);
  --state.dns_pending;
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
  char addrstr[INET_ADDRSTRLEN+1];
  uv_inet_ntop(AF_INET, &addrs[0].ipaddr, addrstr, sizeof(addrstr));
  printf("resolved %s\n", addrstr);
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
  if(state.dns_pending == 0) {
    for(auto &poll : state.ares_polls) {
      uv_poll_stop(&poll.handle);
    }
  }
}

int main(int argc, char **argv) {
  Ares ares;
  if(int err = ares.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 1;
  }

  Ares::Channel dns;
  if(int err = dns.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 2;
  }

  auto loop = uv_default_loop();
  State state;
  for(int i = 1; i < argc; ++i) {
    const char *url = argv[i];
    http_parser_url result {};
    if(0 != http_parser_parse_url(url, strlen(url), 0, &result)) {
      fprintf(stderr, "WARN: Failed to parse URL %s\n", url);
      continue;
    }

    if(result.field_set & UF_SCHEMA &&
       0 != strncmp(url + result.field_data[UF_SCHEMA].off, "http", result.field_data[UF_SCHEMA].len)) {
      fprintf(stderr, "WARN: Skipping non-http URL %s\n", url);
      continue;
    }

    if(!(result.field_set & UF_HOST)) {
      fprintf(stderr, "WARN: URL missing host?! %s\n", url);
      continue;
    }

    char hostname[NS_MAXDNAME + 1];
    if(result.field_data[UF_HOST].len > sizeof(hostname)) {
      fprintf(stderr, "FATAL: Domain name too long in %s\n", url);
      return 3;
    }
    memcpy(hostname, url + result.field_data[UF_HOST].off, result.field_data[UF_HOST].len);
    hostname[result.field_data[UF_HOST].len] = '\0';
    printf("resolving %s\n", hostname);
    //ares_query(dns.channel, hostname, ns_c_in, ns_t_aaaa, query6_cb, nullptr);
    ares_query(dns.channel, hostname, ns_c_in, ns_t_a, query4_cb, &state);
    ++state.dns_pending;
  }

  fd_set read_fds, write_fds;
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);
  int nfds = ares_fds(dns.channel, &read_fds, &write_fds);

  state.ares_polls.reserve(nfds);
  for(int fd = 0; fd < nfds; ++fd) {
    if(FD_ISSET(fd, &read_fds)) {
      state.ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(loop, &state.ares_polls.back().handle, fd);
      state.ares_polls.back().handle.data = &state;
      uv_poll_start(&state.ares_polls.back().handle, UV_READABLE, ares_process_cb);
    }

    if(FD_ISSET(fd, &write_fds)) {
      state.ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(loop, &state.ares_polls.back().handle, fd);
      state.ares_polls.back().handle.data = &state;
      uv_poll_start(&state.ares_polls.back().handle, UV_WRITABLE, ares_process_cb);
    }
  }

  uv_run(loop, UV_RUN_DEFAULT);

  return 0;
}
