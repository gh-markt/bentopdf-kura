#include <algorithm>
#include <iostream>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <stdexcept>
#include <string>
#include <vector>

#include "limits.hh"
#include "passes.hh"

namespace {
using Bytes = std::vector<unsigned char>;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Bytes serialize(QPDF& document) {
  QPDFWriter writer(document);
  writer.setOutputMemory();
  writer.setNewlineBeforeEndstream(true);
  writer.setDeterministicID(true);
  writer.write();
  const auto buffer = writer.getBufferSharedPointer();
  return {buffer->getBuffer(), buffer->getBuffer() + buffer->getSize()};
}

Bytes baseline(pdfa::Level level) {
  QPDF document;
  document.emptyPDF();
  QPDFPageDocumentHelper pages(document);
  for (int index = 0; index < 2; ++index) {
    auto page =
        QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 300 400] /Resources << >> >>");
    page.replaceKey("/Contents",
                    QPDFObjectHandle::newStream(&document, "0 g 20 20 100 100 re f\n"));
    pages.addPage(page, false);
  }
  auto info = QPDFObjectHandle::parse(
      "<< /Title (Validation controls) /CreationDate (D:20260101000000Z) "
      "/ModDate (D:20260101000000Z) >>");
  document.getTrailer().replaceKey("/Info", document.makeIndirectObject(info));
  const auto source = serialize(document);
  pdfa::Options options;
  options.level = level;
  const auto converted = pdfa::convert(source.data(), source.size(), options);
  expect(converted.ok, "could not create control: " + converted.error);
  return converted.pdf;
}

class TestDocument {
 public:
  explicit TestDocument(pdfa::Level level = pdfa::Level::A2B)
      : bytes(baseline(level)),
        context{document,
                options,
                result,
                pdfa::levelPart(level),
                pdfa::levelConformance(level),
                pdfa::levelFamily(level)} {
    options.level = level;
    options.verifyOnly = true;
    document.processMemoryFile("control", reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
  }

  QPDF& pdf() { return document; }
  pdfa::Ctx& ctx() { return context; }
  QPDFObjectHandle root() { return document.getRoot(); }
  QPDFObjectHandle metadata() { return root().getKey("/Metadata"); }
  QPDFObjectHandle page(size_t index = 0) {
    return QPDFPageDocumentHelper(document).getAllPages().at(index).getObjectHandle();
  }
  std::string xml() {
    const auto buffer = metadata().getStreamData(qpdf_dl_all);
    return {reinterpret_cast<const char*>(buffer->getBuffer()), buffer->getSize()};
  }
  void xml(const std::string& value) {
    metadata().replaceStreamData(value, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  }
  void content(const std::string& value) {
    page().replaceKey("/Contents", QPDFObjectHandle::newStream(&document, value));
  }
  bool hasIssue(const std::string& code) const {
    const auto matches = [&code](const pdfa::Issue& issue) { return issue.code == code; };
    return std::any_of(result.issues.begin(), result.issues.end(), matches);
  }
  bool findings() const { return !result.issues.empty(); }
  pdfa::Result check() {
    const auto source = serialize(document);
    return pdfa::convert(source.data(), source.size(), options);
  }
  pdfa::Result convert() {
    const auto source = serialize(document);
    auto conversion = options;
    conversion.verifyOnly = false;
    return pdfa::convert(source.data(), source.size(), conversion);
  }

 private:
  Bytes bytes;
  QPDF document;
  pdfa::Options options;
  pdfa::Result result;
  pdfa::Ctx context;
};

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
  size_t position = 0;
  while ((position = text.find(from, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
}

void metadataCase(const std::string& name) {
  const auto level = name == "metadata_legacy_x1a"  ? pdfa::Level::X1A
                     : name == "metadata_legacy_x3" ? pdfa::Level::X3
                                                    : pdfa::Level::A2B;
  TestDocument document(level);
  std::string xml = document.xml();
  bool valid = false;
  std::string expected = "XMP_INVALID";
  if (name == "metadata_control") {
    valid = true;
  } else if (name == "metadata_legacy_x1a" || name == "metadata_legacy_x3") {
    document.root().removeKey("/Metadata");
    valid = true;
  } else if (name == "metadata_integer") {
    replaceAll(xml, "<pdfaid:part>2</pdfaid:part>", "<pdfaid:part>+02</pdfaid:part>");
    valid = true;
  } else if (name == "metadata_missing") {
    document.root().removeKey("/Metadata");
    expected = "XMP_MISSING";
  } else if (name == "metadata_malformed") {
    xml = "<broken";
  } else if (name == "metadata_wrong_part") {
    replaceAll(xml, "<pdfaid:part>2</pdfaid:part>", "<pdfaid:part>9</pdfaid:part>");
    expected = "XMP_IDENTIFICATION_INVALID";
  } else if (name == "metadata_wrong_namespace") {
    replaceAll(xml, "http://www.aiim.org/pdfa/ns/id/", "urn:wrong-schema");
    expected = "XMP_IDENTIFICATION_INVALID";
  } else if (name == "metadata_alternate_prefix") {
    replaceAll(xml, "pdfaid:", "archive:");
    replaceAll(xml, "xmlns:pdfaid=", "xmlns:archive=");
    valid = true;
  } else if (name == "metadata_attributes") {
    replaceAll(xml, "rdf:about=\"\"", "rdf:about=\"\" pdfaid:part=\"2\" pdfaid:conformance=\"B\"");
    replaceAll(xml, "<pdfaid:part>2</pdfaid:part>", "");
    replaceAll(xml, "<pdfaid:conformance>B</pdfaid:conformance>", "");
    valid = true;
  } else if (name == "metadata_duplicate") {
    replaceAll(xml, "rdf:about=\"\"", "rdf:about=\"\" pdfaid:part=\"9\"");
    expected = "XMP_IDENTIFICATION_INVALID";
  } else if (name == "metadata_doctype") {
    const auto start = xml.find("<x:xmpmeta");
    xml.insert(start, "<!DOCTYPE x:xmpmeta [<!ENTITY external SYSTEM 'file:///not-read'>]>\n");
  } else if (name == "metadata_encoding") {
    xml.insert(0, "<?xml version='1.0' encoding='ISO-8859-1'?>\n");
  } else if (name == "metadata_stream_type") {
    document.metadata().getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Text"));
    expected = "XMP_STREAM_INVALID";
  } else if (name == "metadata_depth") {
    xml = "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>";
    for (int depth = 0; depth < 130; ++depth) {
      xml += "<a>";
    }
    for (int depth = 0; depth < 130; ++depth) {
      xml += "</a>";
    }
    xml += "</rdf:RDF>";
  } else if (name == "metadata_size") {
    xml.insert(xml.find("</rdf:RDF>"), pdfa::kMaxXmpBytes, ' ');
  } else if (name == "metadata_wrong_root") {
    replaceAll(xml, "x:xmpmeta", "x:other");
  } else if (name == "metadata_other_subject") {
    replaceAll(xml, "rdf:about=\"\"", "rdf:about=\"urn:other-document\"");
    expected = "XMP_IDENTIFICATION_INVALID";
  } else {
    throw std::runtime_error("unknown metadata case");
  }
  if (name != "metadata_missing" && name != "metadata_legacy_x1a" && name != "metadata_legacy_x3") {
    document.xml(xml);
  }
  pdfa::checkOriginalMetadata(document.ctx());
  expect(valid ? !document.findings() : document.hasIssue(expected), name);
}

void contentCase(const std::string& name) {
  TestDocument document;
  bool valid = false;
  const std::string unknown = "1 2 UnknownOperator\n";
  if (name == "content_control") {
    valid = true;
  } else if (name == "content_unknown" || name == "content_conversion_reject") {
    document.content(unknown);
  } else if (name == "content_compatibility") {
    document.content("BX\n" + unknown + "EX\n");
    valid = true;
  } else if (name == "content_nested_compatibility") {
    document.content("BX BX\n" + unknown + "EX EX\n");
    valid = true;
  } else if (name == "content_unmatched_begin") {
    document.content("BX\n");
  } else if (name == "content_unmatched_end") {
    document.content("EX\n");
  } else if (name == "content_split_streams") {
    auto contents = QPDFObjectHandle::newArray();
    contents.appendItem(QPDFObjectHandle::newStream(&document.pdf(), "BX\n"));
    contents.appendItem(QPDFObjectHandle::newStream(&document.pdf(), unknown));
    contents.appendItem(QPDFObjectHandle::newStream(&document.pdf(), "EX\n"));
    document.page().replaceKey("/Contents", contents);
    valid = true;
  } else if (name == "content_comments_strings") {
    document.content("% UnknownOperator\n/Span << /ActualText (UnknownOperator) >> BDC EMC\n");
    valid = true;
  } else if (name == "content_inline_image") {
    document.content(std::string("BI /W 1 /H 1 /CS /G /BPC 8 ID ") + char(0) + " EI\n");
    valid = true;
  } else if (name == "content_form" || name == "content_pattern" || name == "content_appearance" ||
             name == "content_resource_cycle") {
    auto stream = QPDFObjectHandle::newStream(&document.pdf(), unknown);
    stream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
    stream.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 100 100]"));
    auto resources = QPDFObjectHandle::newDictionary();
    if (name == "content_appearance") {
      auto annotation = QPDFObjectHandle::parse("<< /Subtype /Stamp /Rect [0 0 100 100] >>");
      auto appearances = QPDFObjectHandle::newDictionary();
      appearances.replaceKey("/N", stream);
      annotation.replaceKey("/AP", appearances);
      auto annotations = QPDFObjectHandle::newArray();
      annotations.appendItem(document.pdf().makeIndirectObject(annotation));
      document.page().replaceKey("/Annots", annotations);
    } else {
      auto entries = QPDFObjectHandle::newDictionary();
      entries.replaceKey("/Test", stream);
      resources.replaceKey(name == "content_pattern" ? "/Pattern" : "/XObject", entries);
      if (name == "content_pattern") {
        stream.getDict().removeKey("/Subtype");
        stream.getDict().replaceKey("/PatternType", QPDFObjectHandle::newInteger(1));
      }
      if (name == "content_resource_cycle") {
        stream.replaceStreamData("q Q\n", QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        stream.getDict().replaceKey("/Resources", resources);
        valid = true;
      }
      document.page().replaceKey("/Resources", resources);
    }
  } else if (name == "content_type3") {
    auto characters = QPDFObjectHandle::newDictionary();
    characters.replaceKey("/A", QPDFObjectHandle::newStream(&document.pdf(), unknown));
    auto font = QPDFObjectHandle::parse("<< /Subtype /Type3 >>");
    font.replaceKey("/CharProcs", characters);
    auto fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/Test", document.pdf().makeIndirectObject(font));
    auto resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);
    document.page().replaceKey("/Resources", resources);
  } else {
    throw std::runtime_error("unknown content case");
  }
  pdfa::checkContentOperators(document.ctx());
  expect(valid ? !document.ctx().failed() : document.ctx().failed(), name);
  if (name == "content_conversion_reject") {
    const auto converted = document.convert();
    expect(!converted.ok && converted.errorCode == "CONTENT_OPERATOR_INVALID", name);
  }
}

void dpartCase(const std::string& name) {
  TestDocument document(pdfa::Level::VT1);
  auto root = document.root().getKey("/DPartRoot");
  auto node = root.getKey("/DPartRootNode");
  auto leaves = node.getKey("/DParts");
  auto leaf = leaves.getArrayItem(0);
  bool valid = false;
  if (name == "dpart_control") {
    valid = true;
  } else if (name == "dpart_optional_types") {
    root.removeKey("/Type");
    node.removeKey("/Type");
    leaf.removeKey("/Type");
    valid = true;
  } else if (name == "dpart_parent" || name == "dpart_repair") {
    node.replaceKey("/Parent", node);
  } else if (name == "dpart_child_cycle") {
    leaves.setArrayItem(0, node);
  } else if (name == "dpart_shared_child") {
    leaves.appendItem(leaf);
  } else if (name == "dpart_start") {
    leaf.replaceKey("/Start", document.root().getKey("/Pages"));
  } else if (name == "dpart_reversed") {
    leaf.replaceKey("/Start", document.page(1));
    leaf.replaceKey("/End", document.page(0));
  } else if (name == "dpart_page_owner") {
    document.page(0).replaceKey("/DPart", node);
  } else if (name == "dpart_overlap") {
    leaves.appendItem(document.pdf().makeIndirectObject(leaf.shallowCopy()));
  } else if (name == "dpart_multiple_leaves") {
    leaf.removeKey("/End");
    auto second = document.pdf().makeIndirectObject(leaf.shallowCopy());
    second.replaceKey("/Start", document.page(1));
    leaves.appendItem(second);
    document.page(1).replaceKey("/DPart", second);
    valid = true;
  } else if (name == "dpart_nested") {
    auto parent = document.pdf().makeIndirectObject(QPDFObjectHandle::newDictionary());
    parent.replaceKey("/Parent", node);
    auto children = QPDFObjectHandle::newArray();
    children.appendItem(leaf);
    parent.replaceKey("/DParts", children);
    leaf.replaceKey("/Parent", parent);
    leaves.setArrayItem(0, parent);
    valid = true;
  } else {
    throw std::runtime_error("unknown DPart case");
  }
  std::string reason;
  expect(pdfa::validDPartTree(document.ctx(), reason) == valid, name + ": " + reason);
  if (name == "dpart_repair") {
    const auto repaired = document.convert();
    expect(repaired.ok, repaired.error);
    QPDF parsed;
    parsed.processMemoryFile("repaired", reinterpret_cast<const char*>(repaired.pdf.data()),
                             repaired.pdf.size());
    pdfa::Options options;
    options.level = pdfa::Level::VT1;
    pdfa::Result result;
    pdfa::Ctx context{parsed, options, result, 0, 0, pdfa::Family::PDFVT};
    expect(pdfa::validDPartTree(context, reason),
           "repair did not produce a valid hierarchy: " + reason);
  }
}

void integrationCase(const std::string& name) {
  for (const auto level : {pdfa::Level::A2B, pdfa::Level::X4, pdfa::Level::E1, pdfa::Level::VT1}) {
    TestDocument document(level);
    if (name == "integration_missing_metadata") {
      document.root().removeKey("/Metadata");
    } else if (name == "integration_malformed_metadata") {
      document.xml("<broken");
    } else if (name == "integration_unknown_operator") {
      document.content("UnknownOperator\n");
    } else if (name == "integration_dpart_parent") {
      if (level != pdfa::Level::VT1) {
        continue;
      }
      auto node = document.root().getKey("/DPartRoot").getKey("/DPartRootNode");
      node.replaceKey("/Parent", node);
    } else {
      throw std::runtime_error("unknown integration case");
    }
    const auto result = document.check();
    expect(!result.ok || !result.compliant, "checker accepted a known-invalid input");
  }
}
}

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  try {
    const std::string name(argv[1]);
    if (name.find("metadata_") == 0) {
      metadataCase(name);
    } else if (name.find("content_") == 0) {
      contentCase(name);
    } else if (name.find("dpart_") == 0) {
      dpartCase(name);
    } else if (name.find("integration_") == 0) {
      integrationCase(name);
    } else {
      throw std::runtime_error("unknown case");
    }
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
