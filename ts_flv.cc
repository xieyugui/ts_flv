/*
* @author:    daemon.xie
* @license:   Apache Licence
* @contact:   xieyugui
* @software:  CLion
* @file:      ts_flv.cc
* @date:      2017/9/11 上午9:52
* @desc:
*/
#include "flv_common.h"

static int
flv_transform_handler(TSCont contp, FlvContext *fc)
{
    TSDebug(PLUGIN_NAME, "[flv_transform_handler] start");
    TSVConn output_conn;
    TSIOBuffer input_buff;
    TSIOBufferReader input_reader;
    TSVIO input_vio;
    int64_t towrite;
    int64_t avail, tag_avail, res_avail;
    int64_t donewrite;
    bool write_down;
    int ret;

    FlvTag *ftag;
    FlvTransformContext *ftc;
    ftc = fc->ftc;
    ftag = &(ftc->ftag);


    output_conn = TSTransformOutputVConnGet(contp);

    input_vio = TSVConnWriteVIOGet(contp);

    input_buff = TSVIOBufferGet(input_vio);
    input_reader = TSVIOReaderGet(input_vio);

    write_down = false;

    if (!input_buff) {
        if (ftc->output.vio) {
            TSVIONBytesSet(ftc->output.vio, ftc->total);
            TSVIOReenable(ftc->output.vio);
            TSDebug(PLUGIN_NAME, "[flv_transform_handler] !input_buff Done Get=%ld, total=%ld",
                    TSVIONDoneGet(ftc->output.vio), ftc->total);
        }
        return 1;
    }

    towrite = TSVIONTodoGet(input_vio);
    TSDebug(PLUGIN_NAME, "[flv_transform_handler] toWrite is %" PRId64 "", towrite);

    if (towrite <= 0)
        goto LDone;

    avail = TSIOBufferReaderAvail(TSVIOReaderGet(input_vio));
    TSDebug(PLUGIN_NAME, "[flv_transform_handler] input_vio avail is %" PRId64 "", avail);

    TSIOBufferCopy(ftc->res_buffer, input_reader, avail, 0);
    TSIOBufferReaderConsume(TSVIOReaderGet(input_vio), avail);
    TSVIONDoneSet(input_vio, TSVIONDoneGet(input_vio) + avail);

    towrite = TSVIONTodoGet(input_vio);
    if (!ftc->parse_over) {
        TSDebug(PLUGIN_NAME, "[flv_transform_handler] in parse_over towrite-avail=%ld",(towrite-avail));
        ret = ftag->process_tag(ftc->res_reader, towrite <= 0);
        TSDebug(PLUGIN_NAME, "[flv_transform_handler] ret=%d",ret);
        if (ret == 0) {
            goto LDone;
        }
        ftc->parse_over = true;

        ftc->output.buffer = TSIOBufferCreate();
        ftc->output.reader = TSIOBufferReaderAlloc(ftc->output.buffer);
        ftc->output.vio = TSVConnWrite(output_conn, contp, ftc->output.reader, ftag->content_length);

        tag_avail = ftag->write_out(ftc->output.buffer, ftc->res_buffer);
        if (tag_avail > 0) {
            ftc->total += tag_avail;
            write_down = true;
        }
    }
    TSDebug(PLUGIN_NAME, "[flv_transform_handler] out parse_over");

    res_avail = TSIOBufferReaderAvail(ftc->res_reader);
    donewrite = TSVIONDoneGet(input_vio);
    donewrite = donewrite - avail;
    TSDebug(PLUGIN_NAME, "[flv_transform_handler] donewrite=%ld, res_avail=%ld", donewrite,res_avail);

    if (ftc->parse_over && ftag->real_end_keyframe_positions > 0 && !ftc->dup_end) {
        TSDebug(PLUGIN_NAME, "[flv_transform_handler] ftc->parse_over && ftag->real_end_keyframe_positions > 0 && !ftc->dup_end");
        if ((uint64_t)donewrite <= ftag->real_end_keyframe_positions) {
            if ((uint64_t)(donewrite + avail) <= ftag->real_end_keyframe_positions) {
                if(res_avail > 0) {
                    TSIOBufferCopy(ftc->output.buffer, ftc->res_reader, res_avail, 0);
                    ftc->total += res_avail;
                    write_down = true;
                }
            } else {
                ftc->dup_end = true;
                // 4 = 10+5 - 11
                int64_t consume_size;
                consume_size = ftag->real_end_keyframe_positions - donewrite;
                if (consume_size > 0) {
                    TSIOBufferCopy(ftc->output.buffer, ftc->res_reader, consume_size, avail - res_avail);
                    ftc->total += consume_size;
                    write_down = true;
                }
            }

        } else {
            ftc->dup_end = true;
        }
        //丢弃
        if (res_avail > 0) {
            TSIOBufferReaderConsume(ftc->res_reader, res_avail);
        }
    } else if (ftc->parse_over && ftag->real_end_keyframe_positions > 0 && ftc->dup_end) {
        TSDebug(PLUGIN_NAME, "[flv_transform_handler] ftc->parse_over && ftag->real_end_keyframe_positions > 0 && ftc->dup_end");
        //丢弃
        if (res_avail > 0) {
            TSIOBufferReaderConsume(ftc->res_reader, res_avail);
        }
    } else {
        TSDebug(PLUGIN_NAME, "[flv_transform_handler] else");
        if (res_avail > 0) {
            TSIOBufferCopy(ftc->output.buffer, ftc->res_reader, res_avail, 0);
            TSIOBufferReaderConsume(ftc->res_reader, res_avail);
            ftc->total += res_avail;
            write_down = true;
        }
    }


    LDone:

    if (write_down)
        TSVIOReenable(ftc->output.vio);

    if (TSVIONTodoGet(input_vio) > 0) {
        if (towrite > 0)
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_READY, input_vio);
    } else {

        TSVIONBytesSet(ftc->output.vio, ftc->total);
        TSDebug(PLUGIN_NAME, "last Done Get=%ld, input_vio Done=%ld", TSVIONDoneGet(ftc->output.vio),  TSVIONDoneGet(input_vio));
        TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_COMPLETE, input_vio);
    }

    return 1;
}

static int
flv_transform_entry(TSCont contp, TSEvent event, void * /* edata ATS_UNUSED */)
{
    TSDebug(PLUGIN_NAME, "Entering flv_transform_entry()");
    TSVIO input_vio;
    FlvContext *fc = (FlvContext *)TSContDataGet(contp);

    if (TSVConnClosedGet(contp)) {
        TSDebug(PLUGIN_NAME, "\tVConn is closed");
        TSContDestroy(contp);
        return 0;
    }

    switch (event) {
        case TS_EVENT_ERROR:
            TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_ERROR");

            input_vio = TSVConnWriteVIOGet(contp);
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_ERROR, input_vio);
            break;

        case TS_EVENT_VCONN_WRITE_COMPLETE:
            TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_VCONN_WRITE_COMPLETE");

            TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);
            break;

        case TS_EVENT_VCONN_WRITE_READY:
            TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_VCONN_WRITE_READY");
        default:
            TSDebug(PLUGIN_NAME, "\tEvent is %d", event);
            flv_transform_handler(contp, fc);
            break;
    }

    return 0;
}

static void
flv_add_transform(FlvContext *fc, TSHttpTxn txnp)
{

    TSVConn connp;
    FlvTransformContext *ftc;

    if (!fc)
        return;

    if (fc->start >= fc->cl || (fc->start == 0 && fc->end == 0)) {
        return;
    }

    if (fc->end >= fc->cl) {
        fc->end = 0;
    }

    if (fc->end <= fc->start) {
        fc->end = 0;
    }

    if (fc->transform_added)
        return;

    TSDebug(PLUGIN_NAME, "[flv_add_transform] start");
    ftc = new FlvTransformContext(fc->start, fc->end, fc->cl);
    TSDebug(PLUGIN_NAME, "[flv_add_transform] start=%lu, end=%lu, cl=%lu",fc->start, fc->end, fc->cl);

    //只缓存transform之前的数据，不缓存transform之后的数据
    TSHttpTxnUntransformedRespCache(txnp, 1);
    TSHttpTxnTransformedRespCache(txnp, 0);

    connp = TSTransformCreate(flv_transform_entry, txnp);
    TSContDataSet(connp, fc);
    TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);
    fc->transform_added = true;
    fc->ftc = ftc;
    TSDebug(PLUGIN_NAME, "[flv_add_transform] end");

}

static void
flv_read_response(FlvContext *fc, TSHttpTxn txnp)
{
    TSMBuffer bufp;
    TSMLoc hdrp;
    TSMLoc cl_field;
    TSHttpStatus status;
    uint64_t n;

    if (TSHttpTxnServerRespGet(txnp, &bufp, &hdrp) != TS_SUCCESS) {
        TSError("[%s] could not get request os data", __FUNCTION__);
        return;
    }

    status = TSHttpHdrStatusGet(bufp, hdrp);
    TSDebug(PLUGIN_NAME, " %s response code %d", __FUNCTION__, status);
    if (status != TS_HTTP_STATUS_OK)
        goto release;

    n = 0;
    cl_field = TSMimeHdrFieldFind(bufp, hdrp, TS_MIME_FIELD_CONTENT_LENGTH, TS_MIME_LEN_CONTENT_LENGTH);
    if (cl_field) {
        n = (uint64_t)TSMimeHdrFieldValueInt64Get(bufp, hdrp, cl_field, -1);
        TSHandleMLocRelease(bufp, hdrp, cl_field);
    }

    TSDebug(PLUGIN_NAME, "[flv_read_response]  content_length=%lu",n);

    if (n <= 0)
        goto release;

    fc->cl = n;
    flv_add_transform(fc, txnp);

    release:

    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdrp);

}

static void
flv_cache_lookup_complete(FlvContext *fc, TSHttpTxn txnp)
{
    TSMBuffer bufp;
    TSMLoc hdrp;
    TSMLoc cl_field;
    TSHttpStatus code;
    int obj_status;
    uint64_t n;

    if (TSHttpTxnCacheLookupStatusGet(txnp, &obj_status) == TS_ERROR) {
        TSError("[%s] %s Couldn't get cache status of object", PLUGIN_NAME, __FUNCTION__);
        return;
    }
    TSDebug(PLUGIN_NAME, " %s object status %d", __FUNCTION__, obj_status);
    if (obj_status != TS_CACHE_LOOKUP_HIT_STALE && obj_status != TS_CACHE_LOOKUP_HIT_FRESH)
        return;

    if (TSHttpTxnCachedRespGet(txnp, &bufp, &hdrp) != TS_SUCCESS) {
        TSError("[%s] %s Couldn't get cache resp", PLUGIN_NAME, __FUNCTION__);
        return;
    }

    code = TSHttpHdrStatusGet(bufp, hdrp);
    if (code != TS_HTTP_STATUS_OK) {
        goto release;
    }

    n = 0;

    cl_field = TSMimeHdrFieldFind(bufp, hdrp, TS_MIME_FIELD_CONTENT_LENGTH, TS_MIME_LEN_CONTENT_LENGTH);
    if (cl_field) {
        n = (uint64_t)TSMimeHdrFieldValueInt64Get(bufp, hdrp, cl_field, -1);
        TSHandleMLocRelease(bufp, hdrp, cl_field);
    }

    TSDebug(PLUGIN_NAME, "[flv_cache_lookup_complete]  content_length=%lu", n);

    if (n <= 0)
        goto release;

    fc->cl = n;
    flv_add_transform(fc, txnp);

    release:

    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdrp);
}


static int
flv_handler(TSCont contp, TSEvent event, void *edata) {

    TSHttpTxn txnp;
    FlvContext *fc;

    txnp = static_cast<TSHttpTxn>(edata);
    fc = (FlvContext *)TSContDataGet(contp);

    switch (event) {
        case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
            TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE");
            flv_cache_lookup_complete(fc, txnp);
            break;

        case TS_EVENT_HTTP_READ_RESPONSE_HDR:
            TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_HTTP_READ_RESPONSE_HDR");
            flv_read_response(fc, txnp);
            break;

        case TS_EVENT_HTTP_TXN_CLOSE:
            TSDebug(PLUGIN_NAME, "TS_EVENT_HTTP_TXN_CLOSE");
            if (fc != NULL)
                delete fc;
            TSContDestroy(contp);
            break;

        default:
            break;
    }

    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
}

TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
    if (!api_info) {
        snprintf(errbuf, errbuf_size, "[TSRemapInit] - Invalid TSRemapInterface argument");
        return TS_ERROR;
    }

    if (api_info->size < sizeof(TSRemapInterface)) {
        snprintf(errbuf, errbuf_size, "[TSRemapInit] - Incorrect size of TSRemapInterface structure");
        return TS_ERROR;
    }

    return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char *argv[], void **instance, char *errbuf, int errbuf_size) {

    return TS_SUCCESS;
}

void TSRemapDeleteInstance(void *instance) {
    return;
}


TSRemapStatus
TSRemapDoRemap(void * /* ih ATS_UNUSED */, TSHttpTxn txnp, TSRemapRequestInfo *rri)
{
    const char *method, *path, *query;
    const char *f_start, *f_end;
    int method_len, path_len, query_len;
    uint64_t start, end;
    FlvContext *fc;
    TSMLoc ae_field, range_field;
    TSCont contp;
    bool start_find, end_find;


    method = TSHttpHdrMethodGet(rri->requestBufp, rri->requestHdrp, &method_len);
    if (method != TS_HTTP_METHOD_GET) {
        TSDebug(PLUGIN_NAME, "TSRemapDoRemap not TS_HTTP_METHOD_GET");
        return TSREMAP_NO_REMAP;
    }

    // check suffix
    path = TSUrlPathGet(rri->requestBufp, rri->requestUrl, &path_len);
    if (path == NULL || path_len <= 4) {
        return TSREMAP_NO_REMAP;
    } else if(strncasecmp(path + path_len - 4, ".flv", 4) != 0) {
        return TSREMAP_NO_REMAP;
    }

    start = 0;
    end = 0;
    start_find = false;
    end_find = false;
    query = TSUrlHttpQueryGet(rri->requestBufp, rri->requestUrl, &query_len);
    TSDebug(PLUGIN_NAME, "TSRemapDoRemap query=%.*s!",query_len,query);
    if(!query || query_len > 1024) {
        TSDebug(PLUGIN_NAME, "TSRemapDoRemap query is null or len > 1024!");
        return TSREMAP_NO_REMAP;
    }
    char *startptr, *endptr;

    char no_start_buf[1025], no_end_buf[1025];
    const char *end_static_query;
    int buf_len, end_buf_len;
    f_start = strcasestr(query, "&start=");
    if(f_start) {
        start = strtoul(f_start + 7, &startptr, 10);
        buf_len = sprintf(no_start_buf, "%.*s%.*s", f_start-query, query, query_len - (startptr-query),startptr);
        start_find = true;
    } else {
        f_start = strcasestr(query, "start=");
        if (f_start) {
            start = strtoul(f_start + 6, &startptr, 10);
            buf_len = sprintf(no_start_buf, "%.*s%.*s", f_start-query, query, query_len - (startptr-query),startptr);
            start_find = true;
        }
    }

    if(start_find) {
        end_static_query = no_start_buf;
    } else {
        end_static_query = query;
        buf_len = query_len;
    }


    f_end = strcasestr(end_static_query, "&end=");
    if(f_end) {
        end = strtoul(f_end + 5, &endptr, 10);
        end_buf_len = sprintf(no_end_buf, "%.*s%.*s", f_end-end_static_query, end_static_query, buf_len - (endptr-end_static_query),endptr);
        end_find = true;
    } else {
        f_end = strcasestr(query, "end=");
        if (f_end) {
            end = strtoul(f_end + 4, &endptr, 10);
            end_buf_len = sprintf(no_end_buf, "%.*s%.*s", f_end-end_static_query, end_static_query, buf_len - (endptr-end_static_query),endptr);
            end_find = true;
        }
    }


    if (!start_find && !end_find) {
        TSDebug(PLUGIN_NAME, "TSRemapDoRemap not found start= or end=");
        return TSREMAP_NO_REMAP;
    }

    if (end_find) {
        TSDebug(PLUGIN_NAME, "TSRemapDoRemap end_buf_len=%d, no_end_buf=%s!", end_buf_len, no_end_buf);
        TSUrlHttpQuerySet(rri->requestBufp, rri->requestUrl, no_end_buf, end_buf_len);

    } else if(start_find) {
        TSDebug(PLUGIN_NAME, "TSRemapDoRemap buf_len = %ld, no_start_buf=%s!",buf_len, no_start_buf);
        TSUrlHttpQuerySet(rri->requestBufp, rri->requestUrl, no_start_buf, buf_len);
    }


    //如果有range 就根据range 大小来匹配
    //request Range: bytes=500-999, response Content-Range: bytes 21010-47021/47022
    // remove Range
    range_field = TSMimeHdrFieldFind(rri->requestBufp, rri->requestHdrp, TS_MIME_FIELD_RANGE, TS_MIME_LEN_RANGE);
    if (range_field) {

        TSDebug(PLUGIN_NAME, "TSRemapDoRemap range request");
        TSMimeHdrFieldDestroy(rri->requestBufp, rri->requestHdrp, range_field);
        TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, range_field);
    }


    TSDebug(PLUGIN_NAME, "TSRemapDoRemap start=%lu, end=%lu", start, end);
    if (start < 0 || end < 0 || (start > 0 && end > 0 && start >= end)) {
        return TSREMAP_NO_REMAP;
    }


    // remove Accept-Encoding
    ae_field = TSMimeHdrFieldFind(rri->requestBufp, rri->requestHdrp, TS_MIME_FIELD_ACCEPT_ENCODING, TS_MIME_LEN_ACCEPT_ENCODING);
    if (ae_field) {
        TSMimeHdrFieldDestroy(rri->requestBufp, rri->requestHdrp, ae_field);
        TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, ae_field);
    }


    fc = new FlvContext(start, end);
    TSDebug(PLUGIN_NAME, "TSRemapDoRemap start=%lu, end=%lu", start, end);

    if (NULL == (contp = TSContCreate((TSEventFunc) flv_handler, NULL))) {
        if(fc != NULL)
            delete fc;
        TSError("[%s] TSContCreate(): failed to create the transaction handler continuation.", PLUGIN_NAME);
    } else {
        TSContDataSet(contp, fc);
        TSHttpTxnHookAdd(txnp, TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, contp);
        TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, contp);
        TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, contp);
    }

    return TSREMAP_NO_REMAP;
}