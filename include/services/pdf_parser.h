#ifndef INCLUDE_SERVICES_PDF_PARSER_H_
#define INCLUDE_SERVICES_PDF_PARSER_H_

#include <memory>
#include <string>

namespace interview::services {
// PDFParser：从 PDF 抽取 UTF-8 文本，供出题简历驱动。
// PoDoFo 头文件只在 pdf_parser.cc 中 include，这里 Pimpl 隔离第三方类型。

class PDFParser {
public:
    PDFParser();
    ~PDFParser();

    PDFParser(const PDFParser&) = delete;
    PDFParser& operator=(const PDFParser&) = delete;

    // 
    bool IsValidPDF(const std::string& path) const;

    // 抽取全文： 失败返回空串 （调用方可配合
    std::string ExtractText(const std::string& path) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} //namespace interview::services
 
#endif //