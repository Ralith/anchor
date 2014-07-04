#ifndef ANCHOR_URL_H_
#define ANCHOR_URL_H_

#include <cstddef>
#include <cstring>

struct Url {
  Url(const char *begin, const char *end);
  Url(const char *c_str) : Url(c_str, c_str + strlen(c_str)) {}

  struct Component {
    const char *base = nullptr;
    size_t len = 0;
  };

  Component scheme, userinfo, host, port, path, query, fragment;
};

#endif
