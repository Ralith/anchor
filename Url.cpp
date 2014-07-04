#include "Url.h"

#define PARSE_NOSKIP(elt)                       \
  elt.base = token_start;                       \
  elt.len = cursor - token_start;               \
  token_start = cursor;

#define PARSE(elt) PARSE_NOSKIP(elt); ++token_start

#define PARSE_END(elt)                          \
  elt.base = token_start;                       \
  elt.len = cursor - token_start + 1

#define RESET(st) state = st; cursor = token_start

Url::Url(const char *begin, const char *end) {
  enum class State { SCHEME, USERINFO, HOST, PORT, PATH, QUERY, FRAGMENT } state = State::SCHEME;
  const char *token_start = begin;
  for(const char *cursor = begin; cursor != end; ++cursor) {
    switch(state) {
    case State::SCHEME:
      if(*cursor == ':') {
        PARSE(scheme);
        state = State::USERINFO;
        if(token_start >= end - 2 || *token_start != '/' || *(token_start+1) != '/') {
          RESET(State::PATH);
        } else {
          RESET(State::USERINFO);
          token_start += 2;
        }
      } else if(cursor == end - 1) {
        if(token_start >= end - 2 || *token_start != '/' || *(token_start+1) != '/') {
          RESET(State::PATH);
        } else {
          RESET(State::USERINFO);
          token_start += 2;
        }
      }
      break;

    case State::USERINFO:
      if(*cursor == '@') {
        PARSE(userinfo);
        state = State::HOST;
      } else if(cursor == end - 1) {
        RESET(State::HOST);
      }
      break;

    case State::HOST:
      if(*cursor == '/') {
        PARSE_NOSKIP(host);
        state = State::PATH;
      } else if(cursor == end - 1) {
        PARSE_END(host);
      } else if(*cursor == ':') {
        PARSE_NOSKIP(host);
        state = State::PORT;
      }
      break;

    case State::PORT:
      if(*cursor == '/') {
        PARSE(port);
        state = State::PATH;
      } else if(cursor == end - 1) {
        PARSE_END(port);
      }
      break;

    case State::PATH:
      if(*cursor == '?') {
        PARSE(path);
        state = State::QUERY;
      } else if(*cursor == '#') {
        PARSE(path);
        state = State::FRAGMENT;
      } else if(cursor == end - 1) {
        PARSE_END(path);
      }
      break;

    case State::QUERY:
      if(*cursor == '#') {
        PARSE(query);
        state = State::FRAGMENT;
      } else if(cursor == end - 1) {
        PARSE_END(query);
      }
      break;

    case State::FRAGMENT:
      if(cursor == end - 1) {
        PARSE_END(query);
      }

    default:
      break;
    }
  }
}
