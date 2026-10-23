#include <expat.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <qpdf/Pipeline.hh>
#include <stdexcept>
#include <string>
#include <vector>

#include "limits.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
const std::string rdfDescription =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#\x1f"
    "Description";
const std::string rdfAbout =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#\x1f"
    "about";
const std::string rdfRoot =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#\x1f"
    "RDF";
const std::string xmpRoot =
    "adobe:ns:meta/\x1f"
    "xmpmeta";

std::string xmlTrim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

class MetadataParser : public Pipeline {
 public:
  explicit MetadataParser(const std::vector<std::pair<std::string, std::string>>& identifiers)
      : Pipeline("original XMP", nullptr), parser(XML_ParserCreateNS("UTF-8", '\x1f')) {
    if (!parser) {
      throw std::runtime_error("could not create XML parser");
    }
    for (const auto& identifier : identifiers) {
      values.emplace(identifier.first, std::vector<std::string>{});
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &MetadataParser::start, &MetadataParser::end);
    XML_SetCharacterDataHandler(parser, &MetadataParser::characters);
    XML_SetStartDoctypeDeclHandler(parser, &MetadataParser::doctype);
    XML_SetXmlDeclHandler(parser, &MetadataParser::declaration);
    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
  }

  ~MetadataParser() override { XML_ParserFree(parser); }

  void write(const unsigned char* data, size_t length) override {
    if (length > kMaxXmpBytes - bytes) {
      throw std::runtime_error("original XMP exceeds the metadata size limit");
    }
    bytes += length;
    if (XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(length),
                  XML_FALSE) != XML_STATUS_OK) {
      throw std::runtime_error(error.empty() ? XML_ErrorString(XML_GetErrorCode(parser)) : error);
    }
  }

  void finish() override {
    if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
      throw std::runtime_error(error.empty() ? XML_ErrorString(XML_GetErrorCode(parser)) : error);
    }
  }

  bool matches(const std::pair<std::string, std::string>& expected) const {
    const auto& actual = values.at(expected.first);
    if (actual.size() != 1) {
      return false;
    }
    auto value = actual.front();
    if (expected.first ==
        "http://www.aiim.org/pdfa/ns/id/\x1f"
        "part") {
      if (!value.empty() && value.front() == '+') {
        value.erase(0, 1);
      }
      const auto firstDigit = value.find_first_not_of('0');
      value = firstDigit == std::string::npos ? "0" : value.substr(firstDigit);
    }
    return value == expected.second;
  }

 private:
  struct Element {
    std::string name;
    std::string text;
    bool documentDescription = false;
    bool property = false;
    bool hasChildren = false;
  };

  static void XMLCALL start(void* context, const XML_Char* name, const XML_Char** attributes) {
    auto& self = *static_cast<MetadataParser*>(context);
    if (self.elements.size() >= 128) {
      self.stop("original XMP nesting exceeds the metadata limit");
      return;
    }
    Element element;
    element.name = name;
    if (self.elements.empty() && element.name != rdfRoot && element.name != xmpRoot) {
      self.stop("XMP must have an RDF or xmpmeta document element");
      return;
    }
    element.documentDescription = element.name == rdfDescription && !self.elements.empty() &&
                                  self.elements.back().name == rdfRoot;
    for (size_t index = 0; attributes[index]; index += 2) {
      if (attributes[index] == rdfAbout && attributes[index + 1][0] != '\0') {
        element.documentDescription = false;
      }
    }
    if (!self.elements.empty()) {
      auto& parent = self.elements.back();
      parent.hasChildren = true;
      element.property = parent.documentDescription && self.values.count(element.name) != 0;
    }
    if (element.documentDescription) {
      for (size_t index = 0; attributes[index]; index += 2) {
        self.record(attributes[index], attributes[index + 1]);
      }
    }
    self.elements.push_back(std::move(element));
  }

  static void XMLCALL end(void* context, const XML_Char*) {
    auto& self = *static_cast<MetadataParser*>(context);
    const auto& element = self.elements.back();
    if (element.property) {
      self.record(element.name, element.hasChildren ? std::string() : xmlTrim(element.text));
    }
    self.elements.pop_back();
  }

  static void XMLCALL characters(void* context, const XML_Char* text, int length) {
    auto& self = *static_cast<MetadataParser*>(context);
    if (!self.elements.empty() && self.elements.back().property) {
      self.elements.back().text.append(text, static_cast<size_t>(length));
    }
  }

  static void XMLCALL doctype(void* context, const XML_Char*, const XML_Char*, const XML_Char*,
                              int) {
    static_cast<MetadataParser*>(context)->stop("DOCTYPE is not permitted in XMP");
  }

  static void XMLCALL declaration(void* context, const XML_Char*, const XML_Char* encoding, int) {
    if (encoding) {
      std::string value(encoding);
      const auto lower = [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      };
      std::transform(value.begin(), value.end(), value.begin(), lower);
      if (value != "utf-8") {
        static_cast<MetadataParser*>(context)->stop("XMP must use UTF-8 encoding");
      }
    }
  }

  void stop(const std::string& message) {
    error = message;
    XML_StopParser(parser, XML_FALSE);
  }

  void record(const std::string& name, const std::string& value) {
    const auto found = values.find(name);
    if (found != values.end()) {
      if (found->second.size() >= 2) {
        stop("duplicate XMP identification properties");
      } else {
        found->second.push_back(xmlTrim(value));
      }
    }
  }

  XML_Parser parser;
  std::vector<Element> elements;
  std::map<std::string, std::vector<std::string>> values;
  std::string error;
  size_t bytes = 0;
};
}

void checkOriginalMetadata(Ctx& ctx) {
  auto metadata = ctx.pdf.getRoot().getKey("/Metadata");
  if (!metadata.isStream()) {
    if (ctx.opt.level != Level::X1A && ctx.opt.level != Level::X3) {
      ctx.issue("XMP_MISSING", "original document has no XMP metadata stream", true);
    }
    return;
  }
  if (!nameIs(metadata.getDict().getKey("/Type"), "/Metadata") ||
      !nameIs(metadata.getDict().getKey("/Subtype"), "/XML")) {
    ctx.issue("XMP_STREAM_INVALID", "original metadata must have Type Metadata and Subtype XML",
              true);
  }
  const auto identifiers = metadataIdentifiers(ctx);
  try {
    MetadataParser parser(identifiers);
    bool filtering = false;
    if (!metadata.pipeStreamData(&parser, &filtering, 0, qpdf_dl_all, true, false)) {
      throw std::runtime_error("could not decode original metadata stream");
    }
    for (const auto& identifier : identifiers) {
      if (!parser.matches(identifier)) {
        ctx.issue("XMP_IDENTIFICATION_INVALID",
                  "original XMP is missing or has conflicting/incorrect " +
                      identifier.first.substr(identifier.first.find('\x1f') + 1),
                  true);
      }
    }
  } catch (const std::exception& exception) {
    ctx.issue("XMP_INVALID", std::string("original XMP is invalid: ") + exception.what(), true);
  }
}
}
