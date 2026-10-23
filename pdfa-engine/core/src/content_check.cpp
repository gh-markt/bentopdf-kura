#include <qpdf/QPDFPageDocumentHelper.hh>
#include <set>
#include <string>

#include "limits.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
class OperatorScanner : public QPDFObjectHandle::ParserCallbacks {
 public:
  void handleObject(QPDFObjectHandle object, size_t, size_t) override {
    if (!object.isOperator() || !error.empty()) {
      return;
    }
    const std::string operation = object.getOperatorValue();
    if (operation == "BX") {
      if (++compatibilityDepth > kMaxContentNest) {
        error = "compatibility section nesting exceeds the content limit";
      }
    } else if (operation == "EX") {
      if (compatibilityDepth == 0) {
        error = "EX without a matching BX";
      } else {
        --compatibilityDepth;
      }
    } else if (compatibilityDepth == 0 && knownOperators.count(operation) == 0) {
      error = "undefined content operator: " + operation;
    }
    if (!error.empty()) {
      terminateParsing();
    }
  }

  void contentSize(size_t size) override {
    if (size > kMaxInflateBytes) {
      error = "decoded content exceeds the content size limit";
      terminateParsing();
    }
  }

  void handleEOF() override {
    if (compatibilityDepth != 0 && error.empty()) {
      error = "BX without a matching EX";
    }
  }

  std::string message() const { return error; }

 private:
  inline static const std::set<std::string> knownOperators{
      "b",  "B",  "b*", "B*",  "BDC", "BI", "BMC", "BT", "c",  "cm", "CS", "cs", "d",  "d0", "d1",
      "Do", "DP", "EI", "EMC", "ET",  "f",  "F",   "f*", "G",  "g",  "gs", "h",  "i",  "ID", "j",
      "J",  "K",  "k",  "l",   "m",   "M",  "MP",  "n",  "q",  "Q",  "re", "RG", "rg", "ri", "s",
      "S",  "SC", "sc", "SCN", "scn", "sh", "T*",  "Tc", "Td", "TD", "Tf", "Tj", "TJ", "TL", "Tm",
      "Tr", "Ts", "Tw", "Tz",  "v",   "w",  "W",   "W*", "y",  "'",  "\""};
  std::string error;
  int compatibilityDepth = 0;
};

class ContentChecker {
 public:
  explicit ContentChecker(Ctx& context) : context(context) {}

  void page(QPDFPageObjectHelper& page) {
    OperatorScanner scanner;
    try {
      page.parseContents(&scanner);
      if (!scanner.message().empty()) {
        context.fatal("CONTENT_OPERATOR_INVALID", scanner.message());
        return;
      }
      resources(page.getAttribute("/Resources", true), 0);
      auto annotations = page.getObjectHandle().getKey("/Annots");
      if (annotations.isArray()) {
        for (int index = 0; index < annotations.getArrayNItems(); ++index) {
          auto annotation = annotations.getArrayItem(index);
          if (annotation.isDictionary()) {
            auto appearances = annotation.getKey("/AP");
            if (appearances.isDictionary()) {
              for (const auto& name : appearances.getKeys()) {
                auto appearance = appearances.getKey(name);
                if (appearance.isStream()) {
                  stream(appearance, 0);
                } else if (appearance.isDictionary()) {
                  for (const auto& state : appearance.getKeys()) {
                    stream(appearance.getKey(state), 0);
                  }
                }
              }
            }
          }
        }
      }
    } catch (const std::exception& exception) {
      context.fatal("CONTENT_SCAN_FAILED", exception.what());
    }
  }

 private:
  void stream(QPDFObjectHandle content, int depth) {
    if (context.failed() || !content.isStream()) {
      return;
    }
    if (depth > kMaxResourceNest || streams.size() >= 250000) {
      context.fatal("CONTENT_SCAN_LIMIT", "content resource traversal limit exceeded");
      return;
    }
    if (content.isIndirect() && !streams.insert(content.getObjGen()).second) {
      return;
    }
    OperatorScanner scanner;
    QPDFObjectHandle::parseContentStream(content, &scanner);
    if (!scanner.message().empty()) {
      context.fatal("CONTENT_OPERATOR_INVALID", scanner.message());
      return;
    }
    resources(content.getDict().getKey("/Resources"), depth + 1);
  }

  void resources(QPDFObjectHandle dictionary, int depth) {
    if (context.failed() || !dictionary.isDictionary()) {
      return;
    }
    if (depth > kMaxResourceNest) {
      context.fatal("CONTENT_SCAN_LIMIT", "content resource nesting limit exceeded");
      return;
    }
    if (dictionary.isIndirect() && !resourceDictionaries.insert(dictionary.getObjGen()).second) {
      return;
    }
    for (const std::string category : {"/XObject", "/Pattern"}) {
      auto entries = dictionary.getKey(category);
      if (entries.isDictionary()) {
        for (const auto& name : entries.getKeys()) {
          auto entry = entries.getKey(name);
          if (entry.isStream() && (nameIs(entry.getDict().getKey("/Subtype"), "/Form") ||
                                   entry.getDict().getKey("/PatternType").isInteger())) {
            stream(entry, depth);
          }
        }
      }
    }
    auto fontResources = dictionary.getKey("/Font");
    if (fontResources.isDictionary()) {
      for (const auto& name : fontResources.getKeys()) {
        auto font = fontResources.getKey(name);
        if (font.isDictionary() && nameIs(font.getKey("/Subtype"), "/Type3")) {
          if (font.isIndirect() && !fonts.insert(font.getObjGen()).second) {
            continue;
          }
          auto characters = font.getKey("/CharProcs");
          if (characters.isDictionary()) {
            for (const auto& character : characters.getKeys()) {
              stream(characters.getKey(character), depth);
            }
          }
          resources(font.getKey("/Resources"), depth + 1);
        }
      }
    }
  }

  Ctx& context;
  std::set<QPDFObjGen> streams;
  std::set<QPDFObjGen> resourceDictionaries;
  std::set<QPDFObjGen> fonts;
};
}

void checkContentOperators(Ctx& ctx) {
  ContentChecker checker(ctx);
  for (auto& page : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    checker.page(page);
    if (ctx.failed()) {
      return;
    }
  }
}
}
