#include "packrat/build.h"

const char *pr_status_string(pr_status_t status)
{
    switch (status) {
    case PR_STATUS_OK:
        return "ok";
    case PR_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case PR_STATUS_IO_ERROR:
        return "io error";
    case PR_STATUS_PARSE_ERROR:
        return "parse error";
    case PR_STATUS_VALIDATION_ERROR:
        return "validation error";
    case PR_STATUS_ALLOCATION_FAILED:
        return "allocation failed";
    case PR_STATUS_INTERNAL_ERROR:
        return "internal error";
    default:
        return "unknown status";
    }
}

