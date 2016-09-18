// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/inspector/SearchUtil.h"

#include "src/inspector/V8InspectorImpl.h"
#include "src/inspector/V8InspectorSessionImpl.h"
#include "src/inspector/V8Regex.h"
#include "src/inspector/protocol/Protocol.h"

namespace v8_inspector {

namespace {

String16 findMagicComment(const String16& content, const String16& name,
                          bool multiline) {
  DCHECK(name.find("=") == String16::kNotFound);
  unsigned length = content.length();
  unsigned nameLength = name.length();

  size_t pos = length;
  size_t equalSignPos = 0;
  size_t closingCommentPos = 0;
  while (true) {
    pos = content.reverseFind(name, pos);
    if (pos == String16::kNotFound) return String16();

    // Check for a /\/[\/*][@#][ \t]/ regexp (length of 4) before found name.
    if (pos < 4) return String16();
    pos -= 4;
    if (content[pos] != '/') continue;
    if ((content[pos + 1] != '/' || multiline) &&
        (content[pos + 1] != '*' || !multiline))
      continue;
    if (content[pos + 2] != '#' && content[pos + 2] != '@') continue;
    if (content[pos + 3] != ' ' && content[pos + 3] != '\t') continue;
    equalSignPos = pos + 4 + nameLength;
    if (equalSignPos < length && content[equalSignPos] != '=') continue;
    if (multiline) {
      closingCommentPos = content.find("*/", equalSignPos + 1);
      if (closingCommentPos == String16::kNotFound) return String16();
    }

    break;
  }

  DCHECK(equalSignPos);
  DCHECK(!multiline || closingCommentPos);
  size_t urlPos = equalSignPos + 1;
  String16 match = multiline
                       ? content.substring(urlPos, closingCommentPos - urlPos)
                       : content.substring(urlPos);

  size_t newLine = match.find("\n");
  if (newLine != String16::kNotFound) match = match.substring(0, newLine);
  match = match.stripWhiteSpace();

  for (unsigned i = 0; i < match.length(); ++i) {
    UChar c = match[i];
    if (c == '"' || c == '\'' || c == ' ' || c == '\t') return "";
  }

  return match;
}

String16 createSearchRegexSource(const String16& text) {
  String16Builder result;

  for (unsigned i = 0; i < text.length(); i++) {
    UChar c = text[i];
    if (c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
        c == '+' || c == '-' || c == '*' || c == '.' || c == ',' || c == '?' ||
        c == '\\' || c == '^' || c == '$' || c == '|') {
      result.append('\\');
    }
    result.append(c);
  }

  return result.toString();
}

std::unique_ptr<std::vector<unsigned>> lineEndings(const String16& text) {
  std::unique_ptr<std::vector<unsigned>> result(new std::vector<unsigned>());

  const String16 lineEndString = "\n";
  unsigned start = 0;
  while (start < text.length()) {
    size_t lineEnd = text.find(lineEndString, start);
    if (lineEnd == String16::kNotFound) break;

    result->push_back(static_cast<unsigned>(lineEnd));
    start = lineEnd + 1;
  }
  result->push_back(static_cast<unsigned>(text.length()));

  return result;
}

std::vector<std::pair<int, String16>> scriptRegexpMatchesByLines(
    const V8Regex& regex, const String16& text) {
  std::vector<std::pair<int, String16>> result;
  if (text.isEmpty()) return result;

  std::unique_ptr<std::vector<unsigned>> endings(lineEndings(text));
  unsigned size = endings->size();
  unsigned start = 0;
  for (unsigned lineNumber = 0; lineNumber < size; ++lineNumber) {
    unsigned lineEnd = endings->at(lineNumber);
    String16 line = text.substring(start, lineEnd - start);
    if (line.length() && line[line.length() - 1] == '\r')
      line = line.substring(0, line.length() - 1);

    int matchLength;
    if (regex.match(line, 0, &matchLength) != -1)
      result.push_back(std::pair<int, String16>(lineNumber, line));

    start = lineEnd + 1;
  }
  return result;
}

std::unique_ptr<protocol::Debugger::SearchMatch> buildObjectForSearchMatch(
    int lineNumber, const String16& lineContent) {
  return protocol::Debugger::SearchMatch::create()
      .setLineNumber(lineNumber)
      .setLineContent(lineContent)
      .build();
}

std::unique_ptr<V8Regex> createSearchRegex(V8InspectorImpl* inspector,
                                           const String16& query,
                                           bool caseSensitive, bool isRegex) {
  String16 regexSource = isRegex ? query : createSearchRegexSource(query);
  return wrapUnique(new V8Regex(inspector, regexSource, caseSensitive));
}

}  // namespace

std::vector<std::unique_ptr<protocol::Debugger::SearchMatch>>
searchInTextByLinesImpl(V8InspectorSession* session, const String16& text,
                        const String16& query, const bool caseSensitive,
                        const bool isRegex) {
  std::unique_ptr<V8Regex> regex = createSearchRegex(
      static_cast<V8InspectorSessionImpl*>(session)->inspector(), query,
      caseSensitive, isRegex);
  std::vector<std::pair<int, String16>> matches =
      scriptRegexpMatchesByLines(*regex.get(), text);

  std::vector<std::unique_ptr<protocol::Debugger::SearchMatch>> result;
  for (const auto& match : matches)
    result.push_back(buildObjectForSearchMatch(match.first, match.second));
  return result;
}

String16 findSourceURL(const String16& content, bool multiline) {
  return findMagicComment(content, "sourceURL", multiline);
}

String16 findSourceMapURL(const String16& content, bool multiline) {
  return findMagicComment(content, "sourceMappingURL", multiline);
}

}  // namespace v8_inspector
