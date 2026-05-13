#include "services/pdf_parser.h"

#include <mutex>
#include <sstream>
#include <vector>
#include <podofo/podofo.h>

#include "common/logger.h"

namespace {

// PoDoFo 的日志级别是进程级全局状态，不是某个 PdfMemDocument 的局部配置。
// Text extraction 遇到缺失字体 spacing 信息时，PoDoFo 会输出大量 Warning：
//   "Unable to provide a word spacing length..."
//   "Unable to provide a hard spacing length..."
// 这些 warning 表示库在用默认字体大小兜底估算空格宽度，通常不代表 PDF 解析失败。
// 这里用 RAII 临时把 PoDoFo 日志级别降到 Error，只屏蔽第三方库的噪声 warning，
// 仍保留真正的 Error；函数返回或抛异常时析构函数会自动恢复调用前的日志级别。
class ScopedPoDoFoLogSeverity {
public:
    explicit ScopedPoDoFoLogSeverity(PoDoFo::PdfLogSeverity severity)
        : lock_(Mutex()),
          previous_(PoDoFo::PdfCommon::GetMaxLoggingSeverity()) {
        PoDoFo::PdfCommon::SetMaxLoggingSeverity(severity);
    }

    ~ScopedPoDoFoLogSeverity() {
        PoDoFo::PdfCommon::SetMaxLoggingSeverity(previous_);
    }

    ScopedPoDoFoLogSeverity(const ScopedPoDoFoLogSeverity&) = delete;
    ScopedPoDoFoLogSeverity& operator=(const ScopedPoDoFoLogSeverity&) = delete;

private:
    // SetMaxLoggingSeverity/GetMaxLoggingSeverity 操作的是 PoDoFo 全局状态。
    // 如果未来多个线程同时解析 PDF，必须串行修改和恢复这个全局日志级别，
    // 否则线程 A 保存的 previous_ 可能被线程 B 覆盖，导致日志级别恢复错乱。
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    // lock_ 的声明顺序必须早于 previous_：
    // 构造时先加锁，再读取并修改 PoDoFo 全局日志级别；
    // 析构函数体先恢复日志级别，随后成员析构时 lock_ 才释放锁。
    std::lock_guard<std::mutex> lock_;
    PoDoFo::PdfLogSeverity previous_;
};

} // namespace


namespace interview::services{

class PDFParser::Impl {
public:
    bool IsValidPDF(const std::string& path) const {
        try {
            // 只在 PoDoFo 加载/检查 PDF 的这段范围内静音 warning。
            // 离开 try 块时会恢复原始日志级别，不影响程序其他模块的日志策略。
            ScopedPoDoFoLogSeverity quiet_podofo(PoDoFo::PdfLogSeverity::Error);
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
            // ExtractTextTo 会触发 PoDoFo 字体 spacing 推断逻辑，也是 warning 最多的位置。
            // 解析结果仍由返回文本和异常处理决定；这里不吞掉异常，也不改变业务日志。
            ScopedPoDoFoLogSeverity quiet_podofo(PoDoFo::PdfLogSeverity::Error);
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
