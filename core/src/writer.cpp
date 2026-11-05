#include "chwp/writer.hpp"

#include "chwp/hwp/hwp_writer.hpp"
#include "chwp/hwpx/hwpx_writer.hpp"

namespace chwp {

bool writeDocument(const model::Document& doc, model::SourceFormat format, Bytes& out,
                   ByteView sourceData, std::string* error) {
    switch (format) {
        case model::SourceFormat::Hwp: {
            hwp::WriteOptions opts;
            opts.sourceData = sourceData;
            opts.compress = true;
            return hwp::write(doc, out, opts, error);
        }
        case model::SourceFormat::Hwpx: {
            hwpx::WriteOptions opts;
            opts.sourceData = sourceData;
            return hwpx::write(doc, out, opts, error);
        }
        case model::SourceFormat::Unknown:
            break;
    }
    if (error) *error = "unsupported export format (expected HWP or HWPX)";
    return false;
}

ExportResult writeDocument(const model::Document& doc, const ExportOptions& options) {
    ExportResult res;
    switch (options.format) {
        case model::SourceFormat::Hwp: {
            hwp::WriteOptions opts;
            opts.sourceData = options.sourceData;
            opts.compress = options.compress;
            res.ok = hwp::write(doc, res.data, opts, &res.error);
            return res;
        }
        case model::SourceFormat::Hwpx: {
            hwpx::WriteOptions opts;
            opts.sourceData = options.sourceData;
            res.ok = hwpx::write(doc, res.data, opts, &res.error);
            return res;
        }
        case model::SourceFormat::Unknown:
            break;
    }
    res.ok = false;
    res.error = "unsupported export format (expected HWP or HWPX)";
    return res;
}

}  // namespace chwp
