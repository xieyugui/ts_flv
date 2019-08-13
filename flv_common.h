/*
* @author:    daemon.xie
* @license:   Apache Licence
* @contact:   xieyugui
* @software:  CLion
* @file:      flv_common.h
* @date:      2017/9/11 上午9:51
* @desc:
*/
#ifndef TS_FLV_FLV_COMMON_H
#define TS_FLV_FLV_COMMON_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

#include <ts/ink_inet.h>
#include <ts/ts.h>
#include <ts/experimental.h>
#include <ts/remap.h>

#include "flv_tag.h"

class IOHandle
{
public:
    IOHandle() : vio(NULL), buffer(NULL), reader(NULL){};

    ~IOHandle()
    {
        if(reader) {
            TSIOBufferReaderFree(reader);
            reader = NULL;
        }

        if(buffer) {
            TSIOBufferDestroy(buffer);
            buffer = NULL;
        }
    }

public:
    TSVIO vio;
    TSIOBuffer buffer;
    TSIOBufferReader reader;
};

class FlvTransformContext
{
public:
    FlvTransformContext(uint64_t st,uint64_t e, uint64_t n) : total(0), parse_over(false), dup_end(false)
    {
        res_buffer = TSIOBufferCreate();
        res_reader = TSIOBufferReaderAlloc(res_buffer);

        ftag.start = st;
        ftag.end = e;
        ftag.cl = n;
    }

    ~FlvTransformContext()
    {
        if (res_reader) {
            TSIOBufferReaderFree(res_reader);
        }

        if (res_buffer) {
            TSIOBufferDestroy(res_buffer);
        }
    }

public:
    IOHandle output;
    TSIOBuffer res_buffer;
    TSIOBufferReader res_reader;
    FlvTag ftag;

    uint64_t total;
    bool  parse_over;
    bool  dup_end; //是否已经找到end
};

class FlvContext
{
public:
    FlvContext(uint64_t s, uint64_t e) :start(s), end(e), cl(0) ,transform_added(false),ftc(NULL){};

    ~FlvContext()
    {
        if(ftc) {
            delete ftc;
            ftc = NULL;
        }
    }

public:
    uint64_t start;
    uint64_t end;
    uint64_t cl;
    bool transform_added;

    FlvTransformContext *ftc;
};


#endif //TS_FLV_FLV_COMMON_H
