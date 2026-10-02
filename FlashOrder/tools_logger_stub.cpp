// Local stub so algorithm-level benches link without the full libhotstuff.
// salticidae::Logger has no default ctor; construct with a category name.
#include "hotstuff/util.h"
#include <cstdarg>
#include <cstdio>
namespace hotstuff {
Logger logger("bench");
} // namespace hotstuff

namespace salticidae {
// Logger::info is only used by HOTSTUFF_LOG_INFO in the bench path; the
// algorithm-level tools parse stderr, so mirror the real format.
static void _log_fmt(const char *fmt, va_list ap) {
    fprintf(stderr, "[hotstuff info] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}
void Logger::info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); _log_fmt(fmt, ap); va_end(ap);
}
void Logger::set_color() {}
} // namespace salticidae
