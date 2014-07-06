#include "Client.h"

#include <cerrno>
#include <cstring>
#include <cmath>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <arpa/nameser.h>

#include "Util.h"

namespace {
void ares_process_cb(uv_poll_t *handle, int status, int events) {
  auto &ares_poll = *reinterpret_cast<Client::AresPoll *>(handle);
  if(status < 0) {
    // FIXME
    fprintf(stderr, "FATAL: %s\n", uv_strerror(status));
    abort();
  }
  ares_process_fd(ares_poll.channel,
                  events & UV_READABLE ? ares_poll.fd : ARES_SOCKET_BAD,
                  events & UV_WRITABLE ? ares_poll.fd : ARES_SOCKET_BAD);
  auto &client = *reinterpret_cast<Client *>(handle->data);
  client.ares_stage();
}

void ares_timer_cb(uv_timer_t *timer) {
  auto &client = *reinterpret_cast<Client *>(timer->data);
  ares_process_fd(client.dns.channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  client.ares_stage();
}

void query4_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  auto &res = *reinterpret_cast<Client::Resolution *>(arg);
  if(status != ARES_SUCCESS) {
    fprintf(stderr, "WARN: DNS resolution failed: %s: %s\n", res.host.c_str(), ares_strerror(status));
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
  if(naddrs == 0) {
    fprintf(stderr, "WARN: DNS lookup returned no addresses for %s\n", res.host.c_str());
    return;
  }

  res.client.connections.emplace_back(res.client, res.host + ":" + std::to_string(res.port), res.path);
  uv_tcp_init(&res.client.loop, &res.client.connections.back().handle);
  res.client.connections.back().connect(addrs[0].ipaddr, res.port);
}

void query6_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  auto &res = *reinterpret_cast<Client::Resolution *>(arg);
  if(status != ARES_SUCCESS) {
    fprintf(stderr, "WARN: DNS resolution failed: %s: %s\n", res.host.c_str(), ares_strerror(status));
    return;
  }
  assert(abuf != nullptr && alen != 0);
  struct ares_addr6ttl addrs[1];
  int naddrs = elementsof(addrs);
  auto result = ares_parse_aaaa_reply(abuf, alen, nullptr, addrs, &naddrs);
  if(result != ARES_SUCCESS) {
    fprintf(stderr, "WARN: Couldn't parse reply from DNS server: %s\n", ares_strerror(status));
    return;
  }
  if(naddrs == 0) {
    fprintf(stderr, "WARN: DNS lookup returned no addresses for %s\n", res.host.c_str());
    return;
  }

  res.client.connections.emplace_back(res.client, res.host + ":" + std::to_string(res.port), res.path);
  uv_tcp_init(&res.client.loop, &res.client.connections.back().handle);
  res.client.connections.back().connect(*reinterpret_cast<in6_addr*>(&addrs[0].ip6addr), res.port);
}
void print_bytes(uint64_t bytes) {
  uint8_t exponent = log(bytes) / log(1024);
  if(exponent == 0) {
    printf("%" PRIu64 "B", bytes);
    return;
  }
  const static char abbrevs[] = "KMGTPE";
  if(exponent - 1 >= elementsof(abbrevs)) {
    printf("%.1fEiB", bytes / pow(1024, 6));
    return;
  }
  char abbrev = abbrevs[exponent - 1];
  printf("%.1f%ciB", bytes / pow(1024, exponent), abbrev);
}
}

Client::~Client() {
  if(fd != -1) {
    munmap(file_data, file_size);
    close(fd);
  }
  uv_loop_close(&loop);
}

void Client::ares_stage() {
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

void Client::init_file(uint64_t size) {
  file_size = size;
  fd = ::open(file_name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if(fd == -1) {
    fprintf(stderr, "FATAL: Failed to open file %s for writing: %s\n", file_name.c_str(), strerror(errno));
    exit(1);
  }

  {
    int result = posix_fallocate(fd, 0, size);
    if(result != 0) {
      fprintf(stderr, "FATAL: Couldn't allocate %lu bytes of file space for output: %s\n", size, strerror(result));
    }
  }

  file_data = static_cast<uint8_t *>(mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0));
  if(file_data == MAP_FAILED) {
    fprintf(stderr, "FATAL: mmap: %s\n", strerror(errno));
    abort();
  }
  chunks.push_back(Chunk{0, size});
  schedule_work();
}

void Client::schedule_work() {
  balance_chunks();

  for(auto &conn : connections) {
    if(conn.state == Connection::State::IDLE) {
      if(chunks.empty()) {
        break;
      }
      conn.get(chunks.back());
      chunks.pop_back();
    }
  }

  for(auto &conn : connections) {
    if(conn.state != Connection::State::IDLE && conn.state != Connection::State::FAILED)
      return;
  }

  for(auto &conn : connections) {
    if(conn.state == Connection::State::IDLE) {
      conn.close();
    }
  }
}

void Client::balance_chunks() {
  if(chunks.empty())
    return;

  size_t available_connections = 0;
  for(auto &conn : connections) {
    if(conn.state <= Connection::State::IDLE)
      ++available_connections;
  }
  if(available_connections == 0)
    return;

  std::vector<Chunk> concat(chunks.begin(), chunks.begin() + 1);

  // Merge adjacent chunks and compute total size remaining
  size_t bytes = chunks[0].len;
  for(auto it = chunks.begin() + 1; it != chunks.end(); ++it) {
    bytes += it->len;
    if(concat.back().off + concat.back().len == it->off) {
      concat.back().len += it->len;
    } else {
      concat.emplace_back(*it);
    }
  }

  // Split evenly
  chunks.clear();
  const size_t max_chunk_size = bytes / available_connections;
  for(auto &chunk : concat) {
    size_t divisor = 1;
    while(chunk.len / divisor > max_chunk_size)
      ++divisor;
    size_t accum = chunk.off;
    for(size_t i = 0; i < divisor; ++i) {
      auto len = chunk.len / divisor + (i < chunk.len % divisor ? 1 : 0);
      chunks.push_back(Chunk{accum, len});
      accum += len;
    }
  }
}

void Client::open(std::string host, in_port_t port, std::string path) {
  resolutions.emplace_back(
    Resolution{std::move(host), port, std::move(path), *this});
  ares_query(dns.channel, resolutions.back().host.c_str(), ns_c_in, ns_t_a, query4_cb, &resolutions.back());
  //ares_query(dns.channel, resolutions.back().host.c_str(), ns_c_in, ns_t_aaaa, query6_cb, &resolutions.back());
  (void)query6_cb;
}

void Client::progress(uint64_t bytes) {
  auto now = uv_now(&loop);
  if(stats.bytes == 0) {
    stats.start_time = now;
  }
  stats.bytes += bytes;

  // cursor horizontal absolute 0 - erase in line - print
  printf("\x1B[0G" "\x1B[K" "%.1f%%", 100.f * (double)stats.bytes / (double)file_size);

  {
    auto dt = now - stats.start_time;
    if(dt != 0) {
      printf(" - ");
      print_bytes(stats.bytes / dt * 1000);
      printf("/s = ");
    }
  }

  bool first = true;
  for(auto &conn : connections) {
    if(conn.state == Connection::State::GET_COPY || conn.state == Connection::State::GET_DIRECT) {
      auto dt = now - conn.stats.start_time;
      if(dt != 0) {
        if(!first) {
          printf(" + ");
        } else {
          first = false;
        }
        print_bytes(conn.stats.bytes / dt * 1000);
        printf("/s");
      }
    }
  }

  fflush(stdout);
}
