/***********************************************************************************************************************************
S3 Storage Internal
***********************************************************************************************************************************/
#ifndef STORAGE_S3_STORAGE_INTERN_H
#define STORAGE_S3_STORAGE_INTERN_H

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
typedef struct StorageS3 StorageS3;

#include "common/io/http/client.h"
#include "storage/s3/storage.h"

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Perform an S3 request
HttpResponse *storageS3Request(
    StorageS3 *this, const String *verb, const String *uri, const HttpQuery *query, const Buffer *body, bool contentRequired,
    bool allowMissing);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
#define FUNCTION_LOG_STORAGE_S3_TYPE                                                                                               \
    StorageS3 *
#define FUNCTION_LOG_STORAGE_S3_FORMAT(value, buffer, bufferSize)                                                                  \
    objToLog(value, "StorageS3", buffer, bufferSize)

#endif
