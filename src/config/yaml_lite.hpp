// A small, strict reader for the YAML subset used by scan configuration files.
//
// Supported:
//   - block mappings      key: value        (nested by indentation, spaces only)
//   - block sequences     - item  /  - key: value  (item continues indented)
//   - flow collections    {a: 1, b: 2}   [1, 2, 3]   (on one line)
//   - scalars: plain, 'single' or "double" quoted (\" \\ \n \t escapes)
//   - comments: '#' at line start or after whitespace (outside quotes)
// Not supported (rejected with an error naming the line): tabs, anchors and
// aliases (& *), tags (!), multi-line scalars (| >), multiple documents,
// duplicate keys.
//
// Scalars are kept as text; typed access is done by the caller so errors can
// say which setting was wrong.
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pp::yaml {

class ParseError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Node {
  enum class Kind { kNull, kScalar, kMap, kSeq } kind = Kind::kNull;
  std::string scalar;                              // kScalar
  bool quoted = false;                             // scalar was quoted
  std::vector<std::pair<std::string, Node>> map;   // kMap, in file order
  std::vector<Node> seq;                           // kSeq
  int line = 0;                                    // 1-based source line

  bool IsNull() const { return kind == Kind::kNull; }
  // nullptr if absent (or this is not a map).
  const Node* Find(const std::string& key) const;
};

// `source` is used in error messages ("<source>:<line>: ...").
Node Parse(const std::string& text, const std::string& source);
Node ParseFile(const std::string& path);

}  // namespace pp::yaml
