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
    if (!object.isOperator() || !this->error.empty()) {
      return;
    }
    const std::string operation = object.getOperatorValue();
    if (operation == "BX") {
      if (++this->compatibilityDepth > kMaxContentNest) {
        this->error = "compatibility section nesting exceeds the content limit";
      }
    } else if (operation == "EX") {
      if (this->compatibilityDepth == 0) {
        this->error = "EX without a matching BX";
      } else {
        --this->compatibilityDepth;
      }
    } else if (this->compatibilityDepth == 0 && knownOperators.count(operation) == 0) {
      this->error = "undefined content operator: " + operation;
    }
    if (!this->error.empty()) {
      this->terminateParsing();
    }
  }

  void contentSize(size_t size) override {
    if (size > kMaxInflateBytes) {
      this->error = "decoded content exceeds the content size limit";
      this->terminateParsing();
    }
  }

  void handleEOF() override {
    if (this->compatibilityDepth != 0 && this->error.empty()) {
      this->error = "BX without a matching EX";
    }
  }

  std::string message() const { return this->error; }

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
        this->context.fatal("CONTENT_OPERATOR_INVALID", scanner.message());
        return;
      }
      this->resources(page.getAttribute("/Resources", true), 0);
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
                  this->stream(appearance, 0);
                } else if (appearance.isDictionary()) {
                  for (const auto& state : appearance.getKeys()) {
                    this->stream(appearance.getKey(state), 0);
                  }
                }
              }
            }
          }
        }
      }
    } catch (const std::exception& exception) {
      this->context.fatal("CONTENT_SCAN_FAILED", exception.what());
    }
  }

 private:
  void stream(QPDFObjectHandle content, int depth) {
    if (this->context.failed() || !content.isStream()) {
      return;
    }
    if (depth > kMaxResourceNest || this->streams.size() >= 250000) {
      this->context.fatal("CONTENT_SCAN_LIMIT", "content resource traversal limit exceeded");
      return;
    }
    if (content.isIndirect() && !this->streams.insert(content.getObjGen()).second) {
      return;
    }
    OperatorScanner scanner;
    QPDFObjectHandle::parseContentStream(content, &scanner);
    if (!scanner.message().empty()) {
      this->context.fatal("CONTENT_OPERATOR_INVALID", scanner.message());
      return;
    }
    this->resources(content.getDict().getKey("/Resources"), depth + 1);
  }

  void resources(QPDFObjectHandle resources, int depth) {
    if (this->context.failed() || !resources.isDictionary()) {
      return;
    }
    if (depth > kMaxResourceNest) {
      this->context.fatal("CONTENT_SCAN_LIMIT", "content resource nesting limit exceeded");
      return;
    }
    if (resources.isIndirect() &&
        !this->resourceDictionaries.insert(resources.getObjGen()).second) {
      return;
    }
    for (const std::string category : {"/XObject", "/Pattern"}) {
      auto entries = resources.getKey(category);
      if (entries.isDictionary()) {
        for (const auto& name : entries.getKeys()) {
          auto entry = entries.getKey(name);
          if (entry.isStream() && (nameIs(entry.getDict().getKey("/Subtype"), "/Form") ||
                                   entry.getDict().getKey("/PatternType").isInteger())) {
            this->stream(entry, depth);
          }
        }
      }
    }
    auto fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
      for (const auto& name : fonts.getKeys()) {
        auto font = fonts.getKey(name);
        if (font.isDictionary() && nameIs(font.getKey("/Subtype"), "/Type3")) {
          if (font.isIndirect() && !this->fonts.insert(font.getObjGen()).second) {
            continue;
          }
          auto characters = font.getKey("/CharProcs");
          if (characters.isDictionary()) {
            for (const auto& character : characters.getKeys()) {
              this->stream(characters.getKey(character), depth);
            }
          }
          this->resources(font.getKey("/Resources"), depth + 1);
        }
      }
    }
  }

  Ctx& context;
  std::set<QPDFObjGen> streams;
  std::set<QPDFObjGen> resourceDictionaries;
  std::set<QPDFObjGen> fonts;
};
}  // namespace

void checkContentOperators(Ctx& ctx) {
  ContentChecker checker(ctx);
  for (auto& page : QPDFPageDocumentHelper(ctx.pdf).getAllPages()) {
    checker.page(page);
    if (ctx.failed()) {
      return;
    }
  }
}
}  // namespace pdfa
