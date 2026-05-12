#include "services/pdf_parser.h"

#include <sstream>
#include <vector>
#include <podofo/podofo.h>

#include "common/logger.h"

namespace interview::services{

class PDFParser::Impl {
public:
    bool IsValidPDF(const std::string& path) const {
        try {
            PoDoFo::PdfMemDocument doc;
            doc.Load(path);
            return doc.GetPages().GetCount() > 0;
        } catch (const std::exception& e) {
            LOG_WARN("IsValidPDF failed : {}", e.what());
            return false;
        } catch (...) {
            LOG_WARN("IsValidPDF failed : unkown error");
            return false;
        }
    }

    std::string ExtractText(const std::string& path) const {
        try {
            PoDoFo::PdfMemDocument doc;
            doc.Load(path);

            std::ostringstream oss;
            const auto& pages = doc.GetPages();
            const unsigned n = pages.GetCount();
            for (unsigned i = 0; i < n; i++) {
                const PoDoFo::PdfPage& page = pages.GetPageAt(i);
                std::vector<PoDoFo::PdfTextEntry> entries;
                // 使用默认提取参数；复杂版式PDF 可能顺序略乱，够用可再调 PdfTextExtractParams
                page.ExtractTextTo(entries);
                for (const auto&e : entries) {
                    oss << e.Text;
                }
                oss << '\n';
            }
            return oss.str();
        } catch (const std::exception& e) {
            LOG_WARN("ExtractText failed: {}", e.what());
            return {};
        } catch (...) {
            LOG_WARN("ExtractText failed : unknown error");
            return {};
        }
    }
};

PDFParser::PDFParser() : impl_(std::make_unique<Impl>()) {}

PDFParser::~PDFParser() = default;

bool PDFParser::IsValidPDF(const std::string& path) const {
    return impl_->IsValidPDF(path);
}

std::string PDFParser::ExtractText(const std::string& path) const {
    return impl_->ExtractText(path);
}

} // namespace interview::services
