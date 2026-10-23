#include <map>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <set>
#include <string>
#include <vector>

#include "limits.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
class DPartChecker {
 public:
  explicit DPartChecker(Ctx& context) : context(context) {
    for (auto& page : QPDFPageDocumentHelper(context.pdf).getAllPages()) {
      this->pageNumbers.emplace(page.getObjectHandle().getObjGen(), this->pages.size());
      this->pages.push_back(page.getObjectHandle());
    }
    this->owners.resize(this->pages.size());
  }

  bool check(std::string& reason) {
    auto root = this->context.pdf.getRoot().getKey("/DPartRoot");
    if (!root.isIndirect() || !root.isDictionary() ||
        (root.hasKey("/Type") && !nameIs(root.getKey("/Type"), "/DPartRoot")) ||
        !this->node(root.getKey("/DPartRootNode"), root, 0)) {
      reason = this->error.empty() ? "invalid document-part root" : this->error;
      return false;
    }
    for (size_t index = 0; index < this->pages.size(); ++index) {
      auto part = this->pages[index].getKey("/DPart");
      if (!part.isIndirect() || this->owners[index] != part.getObjGen()) {
        reason = "page DPart does not match its reachable leaf range";
        return false;
      }
    }
    return true;
  }

 private:
  bool reject(const std::string& reason) {
    this->error = reason;
    return false;
  }

  bool node(QPDFObjectHandle part, QPDFObjectHandle parent, int depth) {
    if (depth > kMaxStructDepth || this->visited.size() >= 250000) {
      return this->reject("document-part hierarchy exceeds traversal limits");
    }
    if (!part.isIndirect() || !part.isDictionary() ||
        (part.hasKey("/Type") && !nameIs(part.getKey("/Type"), "/DPart"))) {
      return this->reject("document-part node must be an indirect DPart dictionary");
    }
    if (!this->visited.insert(part.getObjGen()).second) {
      return this->reject("cycle or shared child in document-part hierarchy");
    }
    auto actualParent = part.getKey("/Parent");
    if (!actualParent.isIndirect() || actualParent.getObjGen() != parent.getObjGen()) {
      return this->reject("document-part Parent does not match the containing node");
    }
    auto children = part.getKey("/DParts");
    if (part.hasKey("/DParts")) {
      if (!children.isArray() || children.getArrayNItems() == 0) {
        return this->reject("invalid internal document-part children");
      }
      for (int index = 0; index < children.getArrayNItems(); ++index) {
        if (!this->node(children.getArrayItem(index), part, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    auto start = part.getKey("/Start");
    auto end = part.hasKey("/End") ? part.getKey("/End") : start;
    if (!start.isIndirect() || !end.isIndirect()) {
      return this->reject("document-part leaf requires indirect page references");
    }
    const auto first = this->pageNumbers.find(start.getObjGen());
    const auto last = this->pageNumbers.find(end.getObjGen());
    if (first == this->pageNumbers.end() || last == this->pageNumbers.end() ||
        first->second > last->second) {
      return this->reject("document-part leaf has an invalid page range");
    }
    for (size_t index = first->second; index <= last->second; ++index) {
      if (this->owners[index].getObj() != 0) {
        return this->reject("overlapping document-part leaf page ranges");
      }
      this->owners[index] = part.getObjGen();
    }
    return true;
  }

  Ctx& context;
  std::vector<QPDFObjectHandle> pages;
  std::map<QPDFObjGen, size_t> pageNumbers;
  std::vector<QPDFObjGen> owners;
  std::set<QPDFObjGen> visited;
  std::string error;
};
}  // namespace

bool validDPartTree(Ctx& ctx, std::string& reason) { return DPartChecker(ctx).check(reason); }
}  // namespace pdfa
