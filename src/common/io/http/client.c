/***********************************************************************************************************************************
Http Client
***********************************************************************************************************************************/
#include "build.auto.h"

#include "common/debug.h"
#include "common/io/http/client.h"
#include "common/io/http/common.h"
#include "common/io/io.h"
#include "common/io/read.intern.h"
#include "common/io/tls/client.h"
#include "common/log.h"
#include "common/type/object.h"
#include "common/wait.h"

/***********************************************************************************************************************************
Http constants
***********************************************************************************************************************************/
STRING_EXTERN(HTTP_VERSION_STR,                                     HTTP_VERSION);

STRING_EXTERN(HTTP_VERB_DELETE_STR,                                 HTTP_VERB_DELETE);
STRING_EXTERN(HTTP_VERB_GET_STR,                                    HTTP_VERB_GET);
STRING_EXTERN(HTTP_VERB_HEAD_STR,                                   HTTP_VERB_HEAD);
STRING_EXTERN(HTTP_VERB_POST_STR,                                   HTTP_VERB_POST);
STRING_EXTERN(HTTP_VERB_PUT_STR,                                    HTTP_VERB_PUT);

STRING_EXTERN(HTTP_HEADER_AUTHORIZATION_STR,                        HTTP_HEADER_AUTHORIZATION);
STRING_EXTERN(HTTP_HEADER_CONTENT_LENGTH_STR,                       HTTP_HEADER_CONTENT_LENGTH);
STRING_EXTERN(HTTP_HEADER_CONTENT_MD5_STR,                          HTTP_HEADER_CONTENT_MD5);
STRING_EXTERN(HTTP_HEADER_ETAG_STR,                                 HTTP_HEADER_ETAG);
STRING_EXTERN(HTTP_HEADER_HOST_STR,                                 HTTP_HEADER_HOST);
STRING_EXTERN(HTTP_HEADER_LAST_MODIFIED_STR,                        HTTP_HEADER_LAST_MODIFIED);

// 5xx errors that should always be retried
#define HTTP_RESPONSE_CODE_RETRY_CLASS                              5

/***********************************************************************************************************************************
Statistics
***********************************************************************************************************************************/
static HttpClientStat httpClientStatLocal;

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct HttpClient
{
    MemContext *memContext;                                         // Mem context
    TimeMSec timeout;                                               // Request timeout
    TlsClient *tlsClient;                                           // TLS client

    TlsSession *tlsSession;                                         // Current TLS session
    HttpResponse *response;                                         // Current response object
};

OBJECT_DEFINE_FREE(HTTP_CLIENT);

/***********************************************************************************************************************************
Mark response object done
***********************************************************************************************************************************/
OBJECT_DEFINE_FREE_RESOURCE_BEGIN(HTTP_CLIENT, LOG, logLevelTrace)
{
    // When the response is marked done it is going to call back into httpClientDone(). Set the TLS session NULL so it will not be
    // freed again.
    this->tlsSession = NULL;
    httpResponseDone(this->response);
}
OBJECT_DEFINE_FREE_RESOURCE_END(LOG);

/**********************************************************************************************************************************/
HttpClient *
httpClientNew(
    const String *host, unsigned int port, TimeMSec timeout, bool verifyPeer, const String *caFile, const String *caPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug)
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM(UINT, port);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
        FUNCTION_LOG_PARAM(BOOL, verifyPeer);
        FUNCTION_LOG_PARAM(STRING, caFile);
        FUNCTION_LOG_PARAM(STRING, caPath);
    FUNCTION_LOG_END();

    ASSERT(host != NULL);

    HttpClient *this = NULL;

    MEM_CONTEXT_NEW_BEGIN("HttpClient")
    {
        this = memNew(sizeof(HttpClient));

        *this = (HttpClient)
        {
            .memContext = MEM_CONTEXT_NEW(),
            .timeout = timeout,
            .tlsClient = tlsClientNew(sckClientNew(host, port, timeout), timeout, verifyPeer, caFile, caPath),
        };

        httpClientStatLocal.object++;
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_LOG_RETURN(HTTP_CLIENT, this);
}

/**********************************************************************************************************************************/
HttpResponse *
httpClientRequest(HttpClient *this, HttpRequest *request, bool contentCache)
{
    FUNCTION_LOG_BEGIN(logLevelDebug)
        FUNCTION_LOG_PARAM(HTTP_CLIENT, this);
        FUNCTION_LOG_PARAM(HTTP_REQUEST, request);
        FUNCTION_LOG_PARAM(BOOL, contentCache);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(!this->response);

    // HTTP Response
    HttpResponse *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        bool retry;
        Wait *wait = waitNew(this->timeout);

        do
        {
            // Assume there will be no retry
            retry = false;

            TRY_BEGIN()
            {
                // Get TLS session
                if (this->tlsSession == NULL)
                {
                    MEM_CONTEXT_BEGIN(this->memContext)
                    {
                        this->tlsSession = tlsClientOpen(this->tlsClient);
                        httpClientStatLocal.session++;
                    }
                    MEM_CONTEXT_END();
                }

                // Write the request
                String *queryStr = httpQueryRender(httpRequestQuery(request));

                ioWriteStrLine(
                    tlsSessionIoWrite(this->tlsSession),
                    strNewFmt(
                        "%s %s%s%s " HTTP_VERSION "\r", strPtr(httpRequestVerb(request)),
                        strPtr(httpUriEncode(httpRequestUri(request), true)), queryStr == NULL ? "" : "?",
                        queryStr == NULL ? "" : strPtr(queryStr)));

                // Write headers
                if (httpRequestHeader(request) != NULL)
                {
                    const StringList *headerList = httpHeaderList(httpRequestHeader(request));

                    for (unsigned int headerIdx = 0; headerIdx < strLstSize(headerList); headerIdx++)
                    {
                        const String *headerKey = strLstGet(headerList, headerIdx);
                        ioWriteStrLine(
                            tlsSessionIoWrite(this->tlsSession),
                            strNewFmt("%s:%s\r", strPtr(headerKey), strPtr(httpHeaderGet(httpRequestHeader(request), headerKey))));
                    }
                }

                // Write out blank line to end the headers
                ioWriteLine(tlsSessionIoWrite(this->tlsSession), CR_BUF);

                // Write out content if any
                if (httpRequestContent(request) != NULL)
                    ioWrite(tlsSessionIoWrite(this->tlsSession), httpRequestContent(request));

                // Flush all writes
                ioWriteFlush(tlsSessionIoWrite(this->tlsSession));

                // Wait for response
                result = httpResponseNew(this, tlsSessionIoRead(this->tlsSession), httpRequestVerb(request), contentCache);

                // Retry when response code is 5xx.  These errors generally represent a server error for a request that looks valid.
                // There are a few errors that might be permanently fatal but they are rare and it seems best not to try and pick
                // and choose errors in this class to retry.
                if (httpResponseCode(result) / 100 == HTTP_RESPONSE_CODE_RETRY_CLASS)
                    THROW_FMT(ServiceError, "[%u] %s", httpResponseCode(result), strPtr(httpResponseReason(result)));
            }
            CATCH_ANY()
            {
                // Close the client since we don't want to reuse the same client on error
                httpClientDone(this, true, false);

                // Retry if wait time has not expired
                if (waitMore(wait))
                {
                    LOG_DEBUG_FMT("retry %s: %s", errorTypeName(errorType()), errorMessage());
                    retry = true;

                    httpClientStatLocal.retry++;
                }
                else
                    RETHROW();
            }
            TRY_END();
        }
        while (retry);

        // Move response to prior context
        httpResponseMove(result, memContextPrior());

        // If the response is still busy make sure it gets marked done
        if (httpResponseBusy(result))
        {
            this->response = result;
            memContextCallbackSet(this->memContext, httpClientFreeResource, this);
        }

        httpClientStatLocal.request++;
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(HTTP_RESPONSE, result);
}

/**********************************************************************************************************************************/
String *
httpClientStatStr(void)
{
    FUNCTION_TEST_VOID();

    String *result = NULL;

    if (httpClientStatLocal.object > 0)
    {
        result = strNewFmt(
            "http statistics: objects %" PRIu64 ", sessions %" PRIu64 ", requests %" PRIu64 ", retries %" PRIu64
                ", closes %" PRIu64,
            httpClientStatLocal.object, httpClientStatLocal.session, httpClientStatLocal.request, httpClientStatLocal.retry,
            httpClientStatLocal.close);
    }

    FUNCTION_TEST_RETURN(result);
}

/**********************************************************************************************************************************/
void
httpClientDone(HttpClient *this, bool close, bool closeRequired)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(HTTP_CLIENT, this);
        FUNCTION_LOG_PARAM(BOOL, close);
        FUNCTION_LOG_PARAM(BOOL, closeRequired);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(close || !closeRequired);

    // If it looks like we were in the middle of a response then close the TLS session so we can start clean next time
    if (close)
    {
        tlsSessionFree(this->tlsSession);
        this->tlsSession = NULL;

        // If a close was required by the server then increment stats
        if (closeRequired)
            httpClientStatLocal.close++;
    }

    memContextCallbackClear(this->memContext);
    this->response = NULL;

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
bool
httpClientBusy(const HttpClient *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_CLIENT, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->response != NULL);
}
