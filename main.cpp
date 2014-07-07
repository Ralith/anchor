#include <string>
#include <algorithm>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

#include <uv.h>

#include "Url.h"
#include "Connection.h"
#include "Client.h"
#include "Util.h"
#include "Options.h"

#if UV_VERSION_MAJOR != 0 || UV_VERSION_MINOR != 11
#error unsupported libuv version
#endif

enum OptionId {
  OUTPUT,
  USER_AGENT
};

const std::vector<Option::Specifier> options({
    {OUTPUT, "output", 'o', "path", Option::Type::STRING, "file to write"},
    {USER_AGENT, "user-agent", 'u', "user agent", Option::Type::STRING, "user-agent to transmit to the server"},
  });

void usage(const char *name) {
  fprintf(stderr, "Usage: %s [options] <url>*\nOptions:\n", name);
  print_options(options);
}

int main(int argc, char **argv) {
  if(argc < 2) {
    usage(argv[0]);
    return 1;
  }

  std::vector<Url> urls;
  urls.reserve(argc-1);
  const char *path = nullptr, *user_agent = "Mozilla/5.0 (X11; Linux x86_64; rv:29.0) Gecko/20100101 Firefox/29.0";
  for(const auto &param : parse_options(argc, argv, options)) {
    switch(param.id) {
    case OUTPUT:
      path = param.parameter.string;
      break;

    case USER_AGENT:
      user_agent = param.parameter.string;
      break;

    default: {
      urls.emplace_back(param.parameter.string);
      const auto &url = urls.back();
      if(path == nullptr && url.path.base != nullptr && url.path.len != 0) {
        path = url.path.base;
        for(const char *ch = url.path.base; ch != url.path.base + url.path.len - 1; ++ch) {
          if(*ch == '/') path = ch+1;
        }
      }
      break;
    }
    }
  }

  if(path == nullptr) {
    fprintf(stderr, "Output filename could not be guessed and must be specified!\n");
    usage(argv[0]);
    return 4;
  }

  if(urls.empty()) {
    fprintf(stderr, "No URLs provided!\n");
    usage(argv[0]);
    return 5;
  }

  Client client;
  client.file_name = path;
  client.user_agent = user_agent;

  if(int err = client.ares.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 2;
  }

  if(int err = client.dns.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 3;
  }

  for(const auto &url : urls) {
    if(url.scheme.base != nullptr &&
       url.scheme.len != 4 &&
       0 != strncmp(url.scheme.base, "http", url.scheme.len)) {
      fprintf(stderr, "WARN: Skipping url with non-http scheme %s\n", std::string(url.scheme.base, url.scheme.len).c_str());
      continue;
    }

    if(url.host.base == nullptr || url.host.len == 0) {
      fprintf(stderr, "WARN: Skipping URL with no host component\n(did you forget the leading \"//\"?)\n");
      continue;
    }

    const in_port_t port = url.port.base == nullptr ? 80 : strtol(url.port.base, nullptr, 10);
    if(port == 0) {
      fprintf(stderr, "WARN: Skipping URL with invalid port: %s\n", std::string(url.port.base, url.port.len).c_str());
      continue;
    }
    std::string path = url.path.base != nullptr ? std::string(url.path.base, url.path.len) : "/";
    client.open(std::string(url.host.base, url.host.len) + (url.port.base ? ":" + std::string(url.port.base, url.port.len) : ""),
                std::string(url.host.base, url.host.len), port, std::move(path));
  }

  client.ares_stage();

  uv_run(&client.loop, UV_RUN_DEFAULT);

  if(std::any_of(client.connections.begin(), client.connections.end(),
                 [](const Connection &c) { return c.state != Connection::State::COMPLETE; })) {
    fprintf(stderr, "Download failed!\n");
    return -1;
  }

  puts("");

  return 0;
}
