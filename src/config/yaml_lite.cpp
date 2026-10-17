#include "config/yaml_lite.hpp"

#include <fstream>
#include <optional>
#include <sstream>

namespace pp::yaml {

const Node* Node::Find(const std::string& key) const {
  if (kind != Kind::kMap) return nullptr;
  for (const auto& [k, v] : map)
    if (k == key) return &v;
  return nullptr;
}

namespace {

struct Line {
  int no = 0;
  int indent = 0;
  std::string text;  // after indentation, comment stripped, right-trimmed
};

std::string RTrim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
  return s;
}

std::string Trim(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  return RTrim(s.substr(a));
}

class Parser {
public:
  Parser(const std::string& text, std::string source)
      : source_(std::move(source)) {
    std::istringstream in(text);
    std::string raw;
    int no = 0;
    while (std::getline(in, raw)) {
      ++no;
      std::size_t i = 0;
      while (i < raw.size() && raw[i] == ' ') ++i;
      if (i < raw.size() && raw[i] == '\t')
        Fail(no, "tab in indentation (use spaces)");
      std::string body = RTrim(StripComment(raw.substr(i), no));
      if (body.empty()) continue;
      if (lines_.empty() && body == "---") continue;  // optional doc marker
      if (body == "---" || body == "...")
        Fail(no, "multiple YAML documents are not supported");
      lines_.push_back({no, static_cast<int>(i), body});
    }
  }

  Node Run() {
    if (lines_.empty()) return Node{};  // empty file
    if (lines_[0].indent != 0) Fail(lines_[0].no, "unexpected indentation");
    Node n = ParseBlock(0);
    if (pos_ < lines_.size())
      Fail(lines_[pos_].no, "unexpected content (check indentation)");
    return n;
  }

private:
  [[noreturn]] void Fail(int line, const std::string& msg) const {
    throw ParseError(source_ + ":" + std::to_string(line) + ": " + msg);
  }

  // Removes a '#' comment that starts at the line start or after a space,
  // outside quotes.
  std::string StripComment(const std::string& s, int no) const {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (quote) {
        if (quote == '"' && c == '\\') ++i;
        else if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '[' || s[i - 1] == '{' ||
            s[i - 1] == ',' || s[i - 1] == ':')
          quote = c;
      } else if (c == '#' && (i == 0 || s[i - 1] == ' ')) {
        return s.substr(0, i);
      }
    }
    if (quote) Fail(no, "unterminated quoted string");
    return s;
  }

  static bool IsSeqItem(const std::string& t) {
    return t == "-" || (t.size() > 1 && t[0] == '-' && t[1] == ' ');
  }

  Node ParseBlock(int indent) {
    return IsSeqItem(lines_[pos_].text) ? ParseSeq(indent) : ParseMap(indent);
  }

  // Splits "key: rest" (key plain or quoted). nullopt if the text is not a
  // mapping entry.
  std::optional<std::pair<std::string, std::string>> SplitKey(
      const std::string& t, int no) const {
    if (t.empty() || t[0] == '{' || t[0] == '[') return std::nullopt;
    std::string key;
    std::size_t i = 0;
    if (t[0] == '"' || t[0] == '\'') {
      key = ParseQuoted(t, i, no);
      while (i < t.size() && t[i] == ' ') ++i;
      if (i >= t.size() || t[i] != ':') return std::nullopt;
    } else {
      for (; i < t.size(); ++i)
        if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) break;
      if (i >= t.size()) return std::nullopt;
      key = Trim(t.substr(0, i));
      if (key.empty()) Fail(no, "empty key");
    }
    return std::make_pair(key, Trim(t.substr(i + 1)));
  }

  Node ParseMap(int indent) {
    Node n;
    n.kind = Node::Kind::kMap;
    n.line = lines_[pos_].no;
    while (pos_ < lines_.size() && lines_[pos_].indent == indent &&
           !IsSeqItem(lines_[pos_].text)) {
      const Line& l = lines_[pos_];
      const auto kv = SplitKey(l.text, l.no);
      if (!kv) Fail(l.no, "expected 'key: value'");
      const auto& [key, rest] = *kv;
      if (n.Find(key)) Fail(l.no, "duplicate key '" + key + "'");
      Node child;
      ++pos_;
      if (rest.empty()) {
        child.line = l.no;
        if (pos_ < lines_.size() && lines_[pos_].indent > indent)
          child = ParseBlock(lines_[pos_].indent);
        else if (pos_ < lines_.size() && lines_[pos_].indent == indent &&
                 IsSeqItem(lines_[pos_].text))
          child = ParseSeq(indent);  // "key:\n- item" at the same indent
      } else {
        child = ParseInline(rest, l.no);
      }
      n.map.emplace_back(key, std::move(child));
    }
    if (pos_ < lines_.size() && lines_[pos_].indent > indent)
      Fail(lines_[pos_].no, "unexpected indentation");
    return n;
  }

  Node ParseSeq(int indent) {
    Node n;
    n.kind = Node::Kind::kSeq;
    n.line = lines_[pos_].no;
    while (pos_ < lines_.size() && lines_[pos_].indent == indent &&
           IsSeqItem(lines_[pos_].text)) {
      Line& l = lines_[pos_];
      std::size_t off = 1;
      while (off < l.text.size() && l.text[off] == ' ') ++off;
      const std::string rest = l.text.substr(off);
      if (rest.empty()) {
        ++pos_;
        Node child;
        child.line = l.no;
        if (pos_ < lines_.size() && lines_[pos_].indent > indent)
          child = ParseBlock(lines_[pos_].indent);
        n.seq.push_back(std::move(child));
      } else if (SplitKey(rest, l.no)) {
        // "- key: value" starts a mapping whose entries line up with `key`.
        l.indent += static_cast<int>(off);
        l.text = rest;
        n.seq.push_back(ParseMap(l.indent));
      } else {
        n.seq.push_back(ParseInline(rest, l.no));
        ++pos_;
      }
    }
    if (pos_ < lines_.size() && lines_[pos_].indent > indent)
      Fail(lines_[pos_].no, "unexpected indentation");
    return n;
  }

  // ---- inline values -------------------------------------------------------

  std::string ParseQuoted(const std::string& s, std::size_t& i, int no) const {
    const char q = s[i++];
    std::string out;
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (c == q) {
        if (q == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
          out += '\'';  // '' inside single quotes
          ++i;
          continue;
        }
        ++i;
        return out;
      }
      if (q == '"' && c == '\\' && i + 1 < s.size()) {
        const char e = s[++i];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          default: Fail(no, std::string("unsupported escape \\") + e);
        }
        continue;
      }
      out += c;
    }
    Fail(no, "unterminated quoted string");
  }

  Node Scalar(std::string text, bool quoted, int no) const {
    Node n;
    n.line = no;
    if (!quoted) {
      if (text.empty() || text == "~" || text == "null") return n;  // kNull
      const char c = text[0];
      if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>' ||
          c == '%' || c == '@' || c == '`')
        Fail(no, "unsupported YAML syntax '" + text.substr(0, 1) +
                     "' (anchors, tags and block scalars are not supported)");
      if (text.find(": ") != std::string::npos)
        Fail(no, "unexpected ': ' in value (quote it, or check indentation)");
    }
    n.kind = Node::Kind::kScalar;
    n.scalar = std::move(text);
    n.quoted = quoted;
    return n;
  }

  Node ParseInline(const std::string& s, int no) const {
    std::size_t i = 0;
    Node n = ParseFlowValue(s, i, no, /*inFlow=*/false);
    while (i < s.size() && s[i] == ' ') ++i;
    if (i != s.size()) Fail(no, "unexpected text after value: '" + s.substr(i) + "'");
    return n;
  }

  Node ParseFlowValue(const std::string& s, std::size_t& i, int no,
                      bool inFlow) const {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size() && s[i] == '{') return ParseFlowMap(s, i, no);
    if (i < s.size() && s[i] == '[') return ParseFlowSeq(s, i, no);
    if (i < s.size() && (s[i] == '"' || s[i] == '\''))
      return Scalar(ParseQuoted(s, i, no), true, no);
    const std::size_t start = i;
    if (inFlow)
      while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    else
      i = s.size();
    return Scalar(Trim(s.substr(start, i - start)), false, no);
  }

  Node ParseFlowMap(const std::string& s, std::size_t& i, int no) const {
    Node n;
    n.kind = Node::Kind::kMap;
    n.line = no;
    ++i;  // '{'
    while (true) {
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size()) Fail(no, "unterminated '{'");
      if (s[i] == '}') { ++i; return n; }
      std::string key;
      if (s[i] == '"' || s[i] == '\'') {
        key = ParseQuoted(s, i, no);
      } else {
        const std::size_t start = i;
        while (i < s.size() && s[i] != ':' && s[i] != ',' && s[i] != '}') ++i;
        key = Trim(s.substr(start, i - start));
      }
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size() || s[i] != ':') Fail(no, "expected ':' after key '" + key + "'");
      ++i;
      if (key.empty()) Fail(no, "empty key");
      if (n.Find(key)) Fail(no, "duplicate key '" + key + "'");
      n.map.emplace_back(key, ParseFlowValue(s, i, no, true));
      while (i < s.size() && s[i] == ' ') ++i;
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; return n; }
      Fail(no, "expected ',' or '}'");
    }
  }

  Node ParseFlowSeq(const std::string& s, std::size_t& i, int no) const {
    Node n;
    n.kind = Node::Kind::kSeq;
    n.line = no;
    ++i;  // '['
    while (true) {
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size()) Fail(no, "unterminated '['");
      if (s[i] == ']') { ++i; return n; }
      n.seq.push_back(ParseFlowValue(s, i, no, true));
      while (i < s.size() && s[i] == ' ') ++i;
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; return n; }
      Fail(no, "expected ',' or ']'");
    }
  }

  std::string source_;
  std::vector<Line> lines_;
  std::size_t pos_ = 0;
};

}  // namespace

Node Parse(const std::string& text, const std::string& source) {
  return Parser(text, source).Run();
}

Node ParseFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ParseError("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.erase(0, 3);  // UTF-8 BOM (Windows editors)
  return Parse(text, path);
}

}  // namespace pp::yaml
