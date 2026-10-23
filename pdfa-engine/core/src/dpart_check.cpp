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
      pageNumbers.emplace(page.getObjectHandle().getObjGen(), pages.size());
      pages.push_back(page.getObjectHandle());
    }
    owners.resize(pages.size());
  }

  bool check(std::string& reason) {
    auto root = context.pdf.getRoot().getKey("/DPartRoot");
    if (!root.isIndirect() || !root.isDictionary() ||
        (root.hasKey("/Type") && !nameIs(root.getKey("/Type"), "/DPartRoot")) ||
        !node(root.getKey("/DPartRootNode"), root, 0)) {
      reason = error.empty() ? "invalid document-part root" : error;
      return false;
    }
    for (size_t index = 0; index < pages.size(); ++index) {
      auto part = pages[index].getKey("/DPart");
      if (!part.isIndirect() || owners[index] != part.getObjGen()) {
        reason = "page DPart does not match its reachable leaf range";
        return false;
      }
    }
    return true;
  }

 private:
  bool reject(const std::string& reason) {
    error = reason;
    return false;
  }

  bool node(QPDFObjectHandle part, QPDFObjectHandle parent, int depth) {
    if (depth > kMaxStructDepth || visited.size() >= 250000) {
      return reject("document-part hierarchy exceeds traversal limits");
    }
    if (!part.isIndirect() || !part.isDictionary() ||
        (part.hasKey("/Type") && !nameIs(part.getKey("/Type"), "/DPart"))) {
      return reject("document-part node must be an indirect DPart dictionary");
    }
    if (!visited.insert(part.getObjGen()).second) {
      return reject("cycle or shared child in document-part hierarchy");
    }
    auto actualParent = part.getKey("/Parent");
    if (!actualParent.isIndirect() || actualParent.getObjGen() != parent.getObjGen()) {
      return reject("document-part Parent does not match the containing node");
    }
    auto children = part.getKey("/DParts");
    if (part.hasKey("/DParts")) {
      if (!children.isArray() || children.getArrayNItems() == 0) {
        return reject("invalid internal document-part children");
      }
      for (int index = 0; index < children.getArrayNItems(); ++index) {
        if (!node(children.getArrayItem(index), part, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    auto start = part.getKey("/Start");
    auto end = part.hasKey("/End") ? part.getKey("/End") : start;
    if (!start.isIndirect() || !end.isIndirect()) {
      return reject("document-part leaf requires indirect page references");
    }
    const auto first = pageNumbers.find(start.getObjGen());
    const auto last = pageNumbers.find(end.getObjGen());
    if (first == pageNumbers.end() || last == pageNumbers.end() || first->second > last->second) {
      return reject("document-part leaf has an invalid page range");
    }
    for (size_t index = first->second; index <= last->second; ++index) {
      if (owners[index].getObj() != 0) {
        return reject("overlapping document-part leaf page ranges");
      }
      owners[index] = part.getObjGen();
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
}

bool validDPartTree(Ctx& ctx, std::string& reason) { return DPartChecker(ctx).check(reason); }
}
