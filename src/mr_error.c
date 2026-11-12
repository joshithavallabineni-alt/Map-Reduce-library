/* mr_error.c - readable text for error codes. */
#include "mapreduce.h"

const char *mr_error_string(mr_error_t err)
{
    switch (err) {
    case MR_OK:            return "success";
    case MR_ERR_NULL:      return "a required pointer was NULL";
    case MR_ERR_NO_MEMORY: return "out of memory";
    case MR_ERR_INVALID:   return "invalid argument";
    case MR_ERR_IO:        return "file input/output error";
    case MR_ERR_CALLBACK:  return "the map or reduce function reported a failure";
    case MR_ERR_STATE:     return "the job is not ready for this call";
    case MR_ERR_THREAD:    return "a thread could not be used";
    }
    return "unknown error";
}
