#include <unistd.h>
#include "tmm_log.h"

void tmm_log(const char *severity, FILE *err_file, bool persist, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    printf("local0.%s: ", severity);
    vprintf(fmt, ap);
    printf("\n");
    if (persist && err_file) {
        fprintf(err_file, "local0.%s: ", severity);
        vfprintf(err_file, fmt, ap2);
        fprintf(err_file, "\r\n");
        fflush(err_file);
        fsync(fileno(err_file));
    }
    va_end(ap2);
    va_end(ap);
}
