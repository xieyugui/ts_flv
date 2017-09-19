/*
* @author:    daemon.xie
* @license:   Apache Licence
* @contact:   xieyugui@migu.cn
* @software:  CLion
* @file:      flv_tag.h
* @date:      2017/9/11 上午10:54
* @desc:
*/

#ifndef __FLV_TAG_H__
#define __FLV_TAG_H__

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ts/ts.h>
#include "amf.h"
#include "types.h"

const char PLUGIN_NAME[] = "ts_flv";

//将字符串转为整数
#define FLV_UI32(x) (int)(((x[0]) << 24) + ((x[1]) << 16) + ((x[2]) << 8) + (x[3]))
#define FLV_UI24(x) (int)(((x[0]) << 16) + ((x[1]) << 8) + (x[2]))
#define FLV_UI16(x) (int)(((x[0]) << 8) + (x[1]))
#define FLV_UI8(x) (int)((x))

//TAG 类型 8:音频 9:视频 18:脚本 其他:保留
/* FLV tag */
#define FLV_TAG_TYPE_AUDIO  ((uint8)0x08)
#define FLV_TAG_TYPE_VIDEO  ((uint8)0x09)
#define FLV_TAG_TYPE_META   ((uint8)0x12)

typedef enum { VIDEO_VERSION_1 = 1, VIDEO_VERSION_3 = 3, VIDEO_VERSION_4  = 4 } video_version;

typedef struct __flv_header {
    byte            signature[3]; /* always "FLV" */
    uint8           version; /* should be 1 */
    uint8_bitmask   flags;
    uint32_be       offset; /* always 9 */
} flv_header;


typedef struct __flv_tag {
    uint8       type; //1 bytes TAG 类型 8:音频 9:视频 18:脚本 其他:保留
    uint24_be   body_length; /* in bytes, total tag size minus 11 */
    uint24_be   timestamp; /* milli-seconds */
    uint8       timestamp_extended; /* timestamp extension */
    uint24_be   stream_id; /* reserved, must be "\0\0\0" */
    /* body comes next */
} flv_tag;

union av_intfloat64 {
    uint64_t i;
    double f;
};

#define flv_tag_get_body_length(tag)    (uint24_be_to_uint32((tag).body_length))
#define flv_tag_get_timestamp(tag) \
    (uint24_be_to_uint32((tag).timestamp) + ((tag).timestamp_extended << 24))


class FlvTag;
typedef int (FlvTag::*FTHandler) ();

class FlvTag
{
public:
	FlvTag() : tag_buffer(NULL), tag_reader(NULL), dup_reader(NULL), meta_buffer(NULL), meta_reader(NULL),meta_buffer_start_tag(NULL),
               meta_reader_start_tag(NULL), copy_meta_buffer(NULL), copy_meta_reader(NULL),modify_meta_buffer(NULL), modify_meta_reader(NULL),
			   head_buffer(NULL), head_reader(NULL),tag_pos(0),cl(0),content_length(0), start_dup_size(0),
			   start_duration_file_size(0),start_duration_time(0),start_duration_video_size(0),start_duration_audio_size(0),
			   start(0),end(0),haskeyframe(false),start_keyframe_len(0),start_keyframe_positions(0),start_keyframe_times(0),
               end_keyframe_len(0),end_keyframe_positions(0),end_keyframe_times(0),real_end_keyframe_positions(0),
               duration(0),filesize(0),videosize(0),audiosize(0),datasize(0),lastkeyframelocation(0),lastkeyframetimestamp(0),
               lasttimestamp(0),delete_meta_size(0),keyframes_len(0),key_found(false)

	{

		tag_buffer = TSIOBufferCreate();
		tag_reader = TSIOBufferReaderAlloc(tag_buffer);
		dup_reader = TSIOBufferReaderAlloc(tag_buffer);

		meta_buffer = TSIOBufferCreate();
		meta_reader = TSIOBufferReaderAlloc(meta_buffer);

        meta_buffer_start_tag = TSIOBufferCreate();
        meta_reader_start_tag = TSIOBufferReaderAlloc(meta_buffer_start_tag);

		copy_meta_buffer = TSIOBufferCreate();
		copy_meta_reader = TSIOBufferReaderAlloc(copy_meta_buffer);

		modify_meta_buffer = TSIOBufferCreate();
		modify_meta_reader = TSIOBufferReaderAlloc(modify_meta_buffer);

		head_buffer = TSIOBufferCreate();
		head_reader = TSIOBufferReaderAlloc(head_buffer);

		current_handler = &FlvTag::process_header;
	}

	~FlvTag()
	{
		if (tag_reader) {
			TSIOBufferReaderFree(tag_reader);
			tag_reader = NULL;
		}

        if (dup_reader) {
            TSIOBufferReaderFree(dup_reader);
            dup_reader = NULL;
        }

		if (tag_buffer) {
			TSIOBufferDestroy(tag_buffer);
			tag_buffer = NULL;
		}

		if (meta_reader) {
			TSIOBufferReaderFree(meta_reader);
			meta_reader = NULL;
		}

		if (meta_buffer) {
			TSIOBufferDestroy(meta_buffer);
			meta_buffer = NULL;
		}

        if (meta_reader_start_tag) {
            TSIOBufferReaderFree(meta_reader_start_tag);
            meta_reader_start_tag = NULL;
        }

        if (meta_buffer_start_tag) {
            TSIOBufferDestroy(meta_buffer_start_tag);
            meta_buffer_start_tag = NULL;
        }

		if (copy_meta_reader) {
			TSIOBufferReaderFree(copy_meta_reader);
			copy_meta_reader = NULL;
		}

		if (copy_meta_buffer) {
			TSIOBufferDestroy(copy_meta_buffer);
			copy_meta_buffer = NULL;
		}

		if (modify_meta_reader) {
			TSIOBufferReaderFree(modify_meta_reader);
			modify_meta_reader = NULL;
		}

		if (modify_meta_buffer) {
			TSIOBufferDestroy(modify_meta_buffer);
			modify_meta_buffer = NULL;
		}

        if (head_reader) {
            TSIOBufferReaderFree(head_reader);
            head_reader = NULL;
        }

        if (head_buffer) {
            TSIOBufferDestroy(head_buffer);
            head_buffer = NULL;
        }
	}


	int process_tag(TSIOBufferReader reader, bool complete);
	int64_t write_out(TSIOBuffer buffer, TSIOBuffer res_buffer);

	size_t get_flv_header_size();
	int flv_read_flv_header(TSIOBufferReader readerp, flv_header * header);

	size_t get_flv_tag_size();
	int flv_read_flv_tag(TSIOBufferReader readerp, flv_tag * tag);

	int process_header();
	int process_meta_body();
	int parse_meta_body();
	int process_medial_body();

	int update_flv_meta_data();
	int flv_read_metadata(byte *stream,amf_data ** name, amf_data ** data, size_t maxbytes);



public:
	TSIOBuffer tag_buffer;
	TSIOBufferReader tag_reader;
	TSIOBufferReader    dup_reader;

	TSIOBuffer head_buffer;
	TSIOBufferReader head_reader;

	TSIOBuffer meta_buffer;
	TSIOBufferReader meta_reader;

    TSIOBuffer meta_buffer_start_tag;
    TSIOBufferReader meta_reader_start_tag;

	TSIOBuffer copy_meta_buffer;
	TSIOBufferReader copy_meta_reader;

	TSIOBuffer modify_meta_buffer;
	TSIOBufferReader modify_meta_reader;

	FTHandler current_handler;

	uint64_t tag_pos; //How many bytes have been consumed
	uint64_t cl; //文件总长度
	uint64_t content_length;  //经过start,end 之后的文件大小


//	uint64_t on_meta_data_size; //脚本数据 大小

    uint64_t start_dup_size; //丢弃的(音频和视频)tag 包括Previous Tag Size 主要用来比较
	uint64_t start_duration_file_size; //start 丢弃的字节大小
    double start_duration_time;  //start 丢弃的时间 ms
    uint64_t start_duration_video_size; //start 丢弃的video 大小
    uint64_t start_duration_audio_size; //start 丢弃的audio 大小

//    uint64_t end_dup_size; //丢弃的(音频和视频)tag 包括Previous Tag Size 主要用来比较
//    uint64_t end_duration_file_size; //end 丢弃的字节大小
//    double end_duration_time;  //end 丢弃的时间 ms
//    uint64_t end_duration_video_size; //end 丢弃的video 大小
//    uint64_t end_duration_audio_size; //end 丢弃的audio 大小

	uint64_t start;   //请求flv 播放的起始字节数
    uint64_t end;	//请求flv 播放的结束字节数

	bool haskeyframe; //是否包含关键帧
	int start_keyframe_len;
	double start_keyframe_positions;
    double start_keyframe_times;

	int end_keyframe_len;
	double end_keyframe_positions;
    double end_keyframe_times;

    uint64_t real_end_keyframe_positions;

    double duration; //总时长
    double filesize; //文件总长度
    double videosize; // video 大小
    double audiosize; // audio 大小
    double datasize; // video + audio 大小

    double lastkeyframelocation; //最后一个关键帧位置　　双精度
    double lastkeyframetimestamp; //　最后一个关键帧时间戳　双精度
    double lasttimestamp; //最后一个时间戳　　　　双精度

    //如果为Number(double), 第一个字节为数据类型，如果为double ,后面的8bytes为Double类型的数据
    //所以删除一个keyframe元素的size 为9
    size_t delete_meta_size; //删除的keyframes元素大小
	uint32_t keyframes_len;// 关键帧原始大小
	bool                key_found;
};

#endif /* __FLV_TAG_H__ */
