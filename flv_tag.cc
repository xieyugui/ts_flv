/*
* @author:    daemon.xie
* @license:   Apache Licence
* @contact:   xieyugui@migu.cn
* @software:  CLion
* @file:      flv_tag.cc
* @date:      2017/9/11 上午10:54
* @desc:
*/


#include "flv_tag.h"

static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length);
static const char * get_amf_type_string(byte type);

static double int2double(uint64_t i)
{
    union av_intfloat64 v;
    v.i = i;
    return v.f;
}

static uint64_t double2int(double f)
{
	union av_intfloat64 v;
	v.f = f;
	return v.i;
}

int FlvTag::process_tag(TSIOBufferReader readerp, bool complete) {
	int64_t avail, head_avail, meta_avail, modify_meta_avail;
	int rc;

	avail = TSIOBufferReaderAvail(readerp);
	TSIOBufferCopy(tag_buffer, readerp, avail, 0);
    TSDebug(PLUGIN_NAME, "[process_tag] readerp avail=%ld",avail);
	TSIOBufferReaderConsume(readerp, avail);

	rc = (this->*current_handler)();
    TSDebug(PLUGIN_NAME, "[process_tag] rc=%d",rc);
	if (rc == 0 && complete) {
		rc = -1;
	}

	if (rc) {
		head_avail = TSIOBufferReaderAvail(head_reader);
        meta_avail = TSIOBufferReaderAvail(meta_reader);
        modify_meta_avail = TSIOBufferReaderAvail(modify_meta_reader);
        TSDebug(PLUGIN_NAME, "[process_tag] cl= %ld, head_avail=%ld, meta_avail=%ld, modify_meta_avail=%ld",
                this->cl, head_avail,meta_avail,modify_meta_avail);
		this->content_length = this->cl - (meta_avail - modify_meta_avail);
        if(this->start <= 0 && this->end > 0) {
            this->content_length -= (this->cl - this->lastkeyframelocation);
        } else if (this->start > 0 && this->end <= 0) {
            this->content_length -= this->start_duration_file_size;
        } else if (this->start > 0 && this->end > 0) {
            this->content_length -= ((this->cl - this->lastkeyframelocation) - this->start_duration_file_size);
        }
		TSDebug(PLUGIN_NAME, "[process_tag] content_length = %ld, lastkeyframelocation=%lf, start_duration_file_size=%lf",
                content_length, this->lastkeyframelocation, this->start_duration_file_size);
	}

	if(rc < 0) {
		//如果异常的话，就不做处理，直接返回全部长度
		this->content_length = this->cl;
	}

	return rc;
}

int64_t FlvTag::write_out(TSIOBuffer buffer, TSIOBuffer res_buffer) {
	int64_t head_avail, modify_meta_avail, tag_avail;

	head_avail = TSIOBufferReaderAvail(head_reader);

	if (head_avail > 0) {
		TSIOBufferCopy(buffer, head_reader, head_avail, 0);
		TSIOBufferReaderConsume(head_reader, head_avail);
	}

    modify_meta_avail = TSIOBufferReaderAvail(modify_meta_reader);

    if (modify_meta_avail > 0) {
        TSIOBufferCopy(buffer, modify_meta_reader, head_avail, 0);
        TSIOBufferReaderConsume(modify_meta_reader, head_avail);
    }
    TSDebug(PLUGIN_NAME, "[write_out] head_avail=%ld, modify_meta_avail=%ld",head_avail, modify_meta_avail);

	//将剩余的拷贝回去
	tag_avail = TSIOBufferReaderAvail(tag_reader);
	if(tag_avail > 0) {
		TSIOBufferCopy(res_buffer, tag_reader, tag_avail, 0);
		TSIOBufferReaderConsume(tag_reader, tag_avail);
	}
	TSDebug(PLUGIN_NAME, "[write_out] tag_avail=%ld",tag_avail);

	return head_avail + modify_meta_avail;
}


int FlvTag::process_header() {
	int64_t avail;
	flv_header header;
	int result;
	size_t flv_header_size = get_flv_header_size();
	size_t need_length  = flv_header_size +  sizeof(uint32_be);  // flv_header + first previoustagsize 第一个默认为0
	//header长度4bytes 整个文件头的长度，一般是9（3+1+1+4），当然头部字段也有可能包含其它信息这个时间其长度就不是9了。
	//FLV Body
	//FLV body就是由很多tag组成的，一个tag包括下列信息：
	//      previoustagsize 4bytes 前一个tag的长度，第一个tag就是0

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < (int64_t)need_length)
		return 0;

	//TSIOBufferCopy(flv_buffer, tag_reader, need_length, 0);
	TSIOBufferCopy(head_buffer, tag_reader, need_length, 0);
	result = flv_read_flv_header(tag_reader, &header);
	TSIOBufferReaderConsume(tag_reader, sizeof(uint32_be));
    TSDebug(PLUGIN_NAME, "[process_header] header_length=%lu",need_length);
	tag_pos += need_length;

	if(result != 0) {
		this->end = 0;
		return -1;
	}


	this->current_handler = &FlvTag::process_meta_body;
	return process_meta_body();
}

//解析metadataTag
int FlvTag::process_meta_body() {
	uint64_t avail, sz;
	uint32 body_length;
	size_t flv_tag_length = get_flv_tag_size(); //
	avail = TSIOBufferReaderAvail(tag_reader);

	flv_tag tag;
	if (avail < flv_tag_length)
		return 0;
	flv_read_flv_tag(tag_reader, &tag);

	//如果第一个tag 不是脚本tag 直接return -1
	if(tag.type != FLV_TAG_TYPE_META) {
		this->end = 0;
		return -1;
	}

	body_length = flv_tag_get_body_length(tag);

	sz = flv_tag_length + body_length + sizeof(uint32_be); //tag->(tag header, tag body), tagsize

	if (avail < sz)     // insure the whole tag
		return 0;

	TSIOBufferCopy(meta_buffer, tag_reader, sz, 0);
	TSIOBufferReaderConsume(tag_reader, sz);

    TSDebug(PLUGIN_NAME, "[process_meta_body] sz=%ld, tag_pos=%ld",sz, tag_pos);
	tag_pos += sz;

	this->current_handler = &FlvTag::parse_meta_body;
	return parse_meta_body();
}

int FlvTag::parse_meta_body() {
	size_t flv_tag_size = get_flv_tag_size();
	TSDebug(PLUGIN_NAME, "parse_meta_body ");

	TSIOBufferCopy(copy_meta_buffer, meta_reader, TSIOBufferReaderAvail(meta_reader), 0);
	//parse flv tag header
	flv_tag tag;
	//parse flv tag data
	uint32 body_length;

	amf_data * name;
	amf_data * data;
	amf_data * on_metadata, *on_metadata_name;
	byte *buf;
	//这里指本身的tag长度
	uint32 prev_tag_size;

	size_t on_medata_size;
	name = NULL;
	data = NULL;
	on_metadata = NULL;
	on_metadata_name = NULL;
	on_medata_size = 0;

	//tag
	flv_read_flv_tag(copy_meta_reader, &tag);
	TSIOBufferReaderConsume(copy_meta_reader, flv_tag_size);

	body_length = flv_tag_get_body_length(tag);
	TSDebug(PLUGIN_NAME,"[parse_meta_body] body_length=%u",body_length);
	//body data
	buf = (byte *)TSmalloc(sizeof(byte) * body_length);
	memset(buf, 0, body_length);

	IOBufferReaderCopy(copy_meta_reader, buf, body_length);
	TSIOBufferReaderConsume(copy_meta_reader, body_length);

	flv_read_metadata(buf, &name, &data,body_length);

	IOBufferReaderCopy(copy_meta_reader, &prev_tag_size, sizeof(uint32_be));
	TSIOBufferReaderConsume(copy_meta_reader, sizeof(uint32_be));

	prev_tag_size = swap_uint32(prev_tag_size);
	TSDebug(PLUGIN_NAME,"[parse_meta_body] prev_tag_size=%u",prev_tag_size);

	/* onMetaData checking */
	if (!strcmp((char*) amf_string_get_bytes(name),"onMetaData")) {
		on_medata_size = amf_data_size(data);
		on_metadata = amf_data_clone(data);
		on_metadata_name = amf_data_clone(name);
		/* check onMetadata type */
		if (amf_data_get_type(on_metadata) != AMF_TYPE_ASSOCIATIVE_ARRAY) {
			TSDebug(PLUGIN_NAME,"invalid onMetaData data type: %u, should be an associative array (8)\n",amf_data_get_type(on_metadata));
			amf_data_free(name);
			amf_data_free(data);
			TSfree(buf);
			goto end;
		}
	}
	amf_data_free(name);
	amf_data_free(data);
	TSfree(buf);

	//parse metadata
	TSDebug(PLUGIN_NAME,"[parse_meta_body] start parse metadata");
	amf_node * n;
	/* more metadata checks */
	//get keyframes array
	for (n = amf_associative_array_first(on_metadata); n != NULL; n = amf_associative_array_next(n)) {
		byte * name;
		amf_data * data;
		byte type;

		name = amf_string_get_bytes(amf_associative_array_get_name(n));
		data = amf_associative_array_get_data(n);
		type = amf_data_get_type(data);

		/* TODO: check UTF-8 strings, in key, and value if string type */
        /* duration (number) */
        if (!strcmp((char*) name, "duration")) {
            if (type == AMF_TYPE_NUMBER) {
                number64 file_duration;
                file_duration = amf_number_get_value(data);
                this->duration = int2double(file_duration);
                TSDebug(PLUGIN_NAME,"[parse_meta_body] duration=%lf",this->duration);
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for duration: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }
        /* filesize: (number) */
        if (!strcmp((char*) name, "filesize")) {
            if (type == AMF_TYPE_NUMBER) {
                number64 file_filesize;

                file_filesize = amf_number_get_value(data);
                this->filesize = int2double(file_filesize);
                TSDebug(PLUGIN_NAME,"[parse_meta_body] filesize=%lf",this->filesize);
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

        /* videosize: (number) */
        if (!strcmp((char*) name, "videosize")) {
            if (type == AMF_TYPE_NUMBER) {
                number64 file_videosize;
                file_videosize = amf_number_get_value(data);
                this->videosize = int2double(file_videosize);
                TSDebug(PLUGIN_NAME,"[parse_meta_body] videosize=%lf",this->videosize);
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for videosize: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

        /* audiosize: (number) */
        if (!strcmp((char*) name, "audiosize")) {
            if (type == AMF_TYPE_NUMBER) {
                number64 file_audiosize;
                file_audiosize = amf_number_get_value(data);
                this->audiosize = int2double(file_audiosize);
                TSDebug(PLUGIN_NAME,"[parse_meta_body] audiosize=%lf",this->audiosize);
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for audiosize: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

        /* datasize: (number) */
        if (!strcmp((char*) name, "datasize")) {
            if (type == AMF_TYPE_NUMBER) {
                number64 file_datasize;
                file_datasize = amf_number_get_value(data);
                this->datasize = int2double(file_datasize);
                TSDebug(PLUGIN_NAME,"[parse_meta_body] datasize=%lf",this->datasize);
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for datasize: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

		/* keyframes: (object) */
		if (!strcmp((char*) name, "keyframes")) {
			if (type == AMF_TYPE_OBJECT) {
				amf_data * file_times, *file_filepositions;

				file_times = amf_object_get(data, "times");
				file_filepositions = amf_object_get(data, "filepositions");

				/* check sub-arrays' presence */
				if (file_times == NULL) {
					TSDebug(PLUGIN_NAME,"Missing times metadata\n");
				}
				if (file_filepositions == NULL) {
					TSDebug(PLUGIN_NAME,"Missing filepositions metadata\n");
				}

				if (file_times != NULL && file_filepositions != NULL) {
					/* check types */
					uint8 times_type, fp_type;

					times_type = amf_data_get_type(file_times);
					if (times_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"times_type != AMF_TYPE_ARRAY －－ invalid type for times: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(times_type));
					}

					fp_type = amf_data_get_type(file_filepositions);
					if (fp_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"fp_type != AMF_TYPE_ARRAY －－ invalid type for filepositions: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(fp_type));
					}

                    if (times_type == AMF_TYPE_ARRAY && fp_type == AMF_TYPE_ARRAY) {
                        TSDebug(PLUGIN_NAME,"[parse_meta_body] fp_len=%u",amf_array_size(file_filepositions));
                        number64 last_file_time;
                        int have_last_time;
                        amf_node * ff_node, *ft_node;

                        /* iterate in parallel, report diffs */
                        last_file_time = 0;
                        have_last_time = 0;

                        ft_node = amf_array_first(file_times);
                        ff_node = amf_array_first(file_filepositions);

                        if (ft_node != NULL && ff_node != NULL)
                            haskeyframe = true;

                        number64 f_time,f_position;
                        double df_time ,df_position;

                        while (ft_node != NULL && ff_node != NULL) {
                            f_time =0;
                            f_position = 0;
                            df_time = 0;
                            df_position = 0;
                            /* time */
                            if (amf_data_get_type(amf_array_get(ft_node)) != AMF_TYPE_NUMBER) {
                                TSDebug(PLUGIN_NAME,"!= AMF_TYPE_NUMBER  －－ invalid type for time: expected %s, got %s\n",
                                        get_amf_type_string(AMF_TYPE_NUMBER),
                                        get_amf_type_string(type));
                            } else {
                                f_time = amf_number_get_value(amf_array_get(ft_node));
                                df_time = int2double(f_time);
                                /* check for duplicate time, can happen in H.264 files */
                                if (have_last_time && last_file_time == f_time) {
                                    TSDebug(PLUGIN_NAME,"Duplicate keyframe time: %lf\n",int2double(f_time));
                                }
                                have_last_time = 1;
                                last_file_time = f_time;
                            }

							/* position */
							if (amf_data_get_type(amf_array_get(ff_node)) != AMF_TYPE_NUMBER) {
								TSDebug(PLUGIN_NAME,
										"!= AMF_TYPE_NUMBER invalid type for file position: expected %s, got %s\n",
										get_amf_type_string(AMF_TYPE_NUMBER),
										get_amf_type_string(type));
							} else {
								f_position = amf_number_get_value(amf_array_get(ff_node));
								df_position = int2double(f_position);
								TSDebug(PLUGIN_NAME,"invalid keyframe file position: expected  got [uint64=%ld], %lf\n",f_position, df_position);
							}

                            TSDebug(PLUGIN_NAME,"[parse_meta_body] keyframes time=%lf,position=%lf",df_time, df_position);

                            if (start > 0) {
                                if (end > 0) {
                                    if (df_position < start)
                                        start_keyframe_len += 1;
                                    else if(df_position >= start and df_position <= end) {
                                        if (start_keyframe_positions <= 0) {
                                            start_keyframe_len += 1;
                                            start_keyframe_positions = df_position;
                                            start_keyframe_times = df_time;
                                        }
                                    } else {
                                        break;
                                    }
                                    end_keyframe_len += 1;
                                    end_keyframe_positions = df_position;
                                    end_keyframe_times = df_time;
                                    lastkeyframelocation = df_position;
                                    lastkeyframetimestamp = df_time;
                                    lasttimestamp = df_time;
                                } else {
                                    start_keyframe_len += 1;
                                    if (df_position >= start) {
                                        start_keyframe_positions = df_position;
                                        start_keyframe_times = df_time;
                                        break;
                                    }
                                }
                            } else {
                                if (df_position > end)
                                    break;
                                end_keyframe_len += 1;
                                end_keyframe_positions = df_position;
                                end_keyframe_times = df_time;
                                lastkeyframelocation = df_position;
                                lastkeyframetimestamp = df_time;
                                lasttimestamp = df_time;
                            }

							/* next entry */
                            ft_node = amf_array_next(ft_node);
							ff_node = amf_array_next(ff_node);
						}
					}
				}
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for keyframes: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_BOOLEAN),
						get_amf_type_string(type));
			}
		}//end keyframes
	}// end for

	end:
    TSDebug(PLUGIN_NAME,"start_keyframe=%d,%lf end_keyframe=%d,%lf",start_keyframe_len,start_keyframe_positions
            ,end_keyframe_len,end_keyframe_positions);
    TSDebug(PLUGIN_NAME,"lastkeyframe location=%lf, timestamp=%lf lasttimestamp=%lf",lastkeyframelocation,lastkeyframetimestamp
            ,lasttimestamp);
	amf_data_free(on_metadata);
	amf_data_free(on_metadata_name);

    TSIOBufferReaderConsume(copy_meta_reader, TSIOBufferReaderAvail(copy_meta_reader));

    //如果没有关键帧，就不实现end功能
    if (!haskeyframe) {
        this->end = 0;
        this->end_keyframe_len = 0;
        this->end_keyframe_positions = 0;
    } else {
        this->start = double2int(this->start_keyframe_positions);
        this->end = double2int(this->end_keyframe_positions);
		this->real_end_keyframe_positions = this->end;
    }

    if (this->start <=0 && this->end <= 0) {
		this->end = 0;
		this->real_end_keyframe_positions = this->end;
		return -1;
	}


    if (this->start > 0) {
        this->current_handler = &FlvTag::process_medial_body;
        return process_medial_body();
    } else {
        this->current_handler = &FlvTag::update_flv_meta_data;
        return update_flv_meta_data();
    }
}

//丢失视频和音频
int FlvTag::process_medial_body() {
	int64_t avail, sz;
	uint32 body_length, timestamp;
	size_t flv_tag_length = get_flv_tag_size();

	avail = TSIOBufferReaderAvail(tag_reader);

	do {
		flv_tag tag;
		if (avail < (int64_t)flv_tag_length)
			return 0;

		flv_read_flv_tag(tag_reader, &tag);
		body_length = flv_tag_get_body_length(tag);
		sz = flv_tag_length + body_length + sizeof(uint32_be); //tag->(tag header, tag body), tagsize

		if (avail < sz)     // insure the whole tag
			return 0;

		start_dup_size += sz;
        TSDebug(PLUGIN_NAME,"[process_medial_body] start_duration_file_size=%lu",start_duration_file_size);

		if (tag.type == FLV_TAG_TYPE_VIDEO ) {

			timestamp = flv_tag_get_timestamp(tag);

			if (start_dup_size <= start) {
				start_duration_time = timestamp; //ms
				start_duration_video_size += flv_tag_length + body_length;

			} else {

				TSDebug(PLUGIN_NAME, "process_medial_body success!!! start_duration_audio_size＝%lu, tag_pos= %ld",
                        start_duration_audio_size,tag_pos);
				this->current_handler = &FlvTag::update_flv_meta_data;
				return update_flv_meta_data();
			}
		} else if(tag.type  == FLV_TAG_TYPE_AUDIO) {
			start_duration_audio_size += flv_tag_length + body_length;
		}

		TSIOBufferReaderConsume(tag_reader, sz);
		start_duration_file_size += sz;

		avail -= sz;

		tag_pos += sz;

	} while (avail > 0);

	return 0;
}

/**
 * 1. 当start=0，end>0；更新duration，lasttimestamp，lastkeyframetimestamp，lastkeyframelocation，filesize，keyframes
 * 2. 当start>0，end=0; 更新duration，videosize，audiosize，datasize，lasttimestamp，lastkeyframetimestamp，lastkeyframelocation，
 *      filesize，如果有keyframes的话，也需要更新
 * 3. 当start>0，end>0; 更新duration，lasttimestamp，lastkeyframetimestamp，lastkeyframelocation，filesize，keyframes
 * 注意 由于end 的videosize，audiosize没办法更新，脚本数据是先于end产生的
 */
int FlvTag::update_flv_meta_data() {

    TSDebug(PLUGIN_NAME, "update_flv_meta_data start");
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_file_size=%lf",this->start_duration_file_size);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_time=%lf",this->start_duration_time);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_video_size=%lf",this->start_duration_video_size);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_audio_size=%lf",this->start_duration_audio_size);
    if(this->start <= 0 && this->end > 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start <= 0 && this->end > 0");
        this->lastkeyframelocation;
        this->lastkeyframetimestamp;
        this->lasttimestamp;
        this->duration = this->lastkeyframetimestamp;
        this->filesize = this->lastkeyframelocation;
        //keyframes 在遍历过程中删除
    } else if (this->start > 0 && this->end <= 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start > 0 && this->end <= 0");
        this->lastkeyframelocation = this->lastkeyframelocation - this->start_duration_file_size;
        this->lastkeyframetimestamp = this->lastkeyframetimestamp - this->start_duration_time/1000;
        this->lasttimestamp = this->lastkeyframetimestamp;
        this->duration = this->lastkeyframetimestamp;
        this->filesize = this->filesize - this->start_duration_file_size;
        this->videosize = this->videosize - this->start_duration_video_size;
        this->audiosize = this->audiosize - this->start_duration_audio_size;
        this->datasize = this->datasize - this->start_duration_video_size - this->start_duration_audio_size;
        //keyframes 在遍历过程中删除
    } else if (this->start > 0 && this->end > 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start > 0 && this->end > 0");
        this->lastkeyframelocation -= this->start_duration_file_size;
        this->lastkeyframetimestamp -= this->start_keyframe_times;
        this->lasttimestamp = this->lastkeyframetimestamp;
        this->duration = this->lastkeyframetimestamp;
        this->filesize = this->lastkeyframelocation;
        //keyframes 在遍历过程中删除
    } else {
		this->end = 0;
		this->real_end_keyframe_positions = this->end;
        return -1;
    }

    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lastkeyframelocation=%lf",this->lastkeyframelocation);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lastkeyframetimestamp=%lf",this->lastkeyframetimestamp);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lasttimestamp=%lf",this->lasttimestamp);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] duration=%lf",this->duration);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] filesize=%lf",this->filesize);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] videosize=%lf",this->videosize);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] audiosize=%lf",this->audiosize);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] datasize=%lf",this->datasize);


    int64_t avail;
    avail = TSIOBufferReaderAvail(meta_reader);
    TSDebug(PLUGIN_NAME, "update_flv_meta_data meta data length=%ld",avail);

    TSIOBufferCopy(copy_meta_buffer, meta_reader,avail , 0);

	size_t flv_tag_size = get_flv_tag_size();

	//parse flv tag
	flv_tag tag;
	uint32 body_length;
	amf_data * name;
	amf_data * data;
	amf_data * on_metadata, *on_metadata_name;
	byte *buf;
	uint32 prev_tag_size;
	size_t on_medata_size;
	name = NULL;
	data = NULL;
	on_metadata = NULL;
	on_metadata_name = NULL;
	on_medata_size = 0;

	flv_read_flv_tag(copy_meta_reader, &tag);
	TSIOBufferReaderConsume(copy_meta_reader, flv_tag_size);

	body_length = flv_tag_get_body_length(tag);


	buf = (byte *)TSmalloc(sizeof(byte) * body_length);
	memset(buf, 0, body_length);
	IOBufferReaderCopy(copy_meta_reader, buf, body_length);
	TSIOBufferReaderConsume(copy_meta_reader, body_length);
	flv_read_metadata(buf, &name, &data,body_length);
	IOBufferReaderCopy(copy_meta_reader, &prev_tag_size, sizeof(uint32_be));
	TSIOBufferReaderConsume(copy_meta_reader, sizeof(uint32_be));
	prev_tag_size = swap_uint32(prev_tag_size);
	/* onMetaData checking */
	if (!strcmp((char*) amf_string_get_bytes(name),"onMetaData")) {
		on_medata_size = amf_data_size(data);
        TSDebug(PLUGIN_NAME,"start on_medata_size: %u",on_medata_size);
		on_metadata = amf_data_clone(data);
		on_metadata_name = amf_data_clone(name);
		/* check onMetadata type */
		if (amf_data_get_type(on_metadata) != AMF_TYPE_ASSOCIATIVE_ARRAY) {
			TSDebug(PLUGIN_NAME,"invalid onMetaData data type: %u, should be an associative array (8)\n",amf_data_get_type(on_metadata));
			amf_data_free(name);
			amf_data_free(data);
			TSfree(buf);
			goto end;
		}
	}
	amf_data_free(name);
	amf_data_free(data);
	TSfree(buf);

    //parse metadata
    amf_node * n;
	/* more metadata checks */
	for (n = amf_associative_array_first(on_metadata); n != NULL; n =
			amf_associative_array_next(n)) {
		byte * name;
		amf_data * data;
		byte type;
		double data_value;

		name = amf_string_get_bytes(amf_associative_array_get_name(n));
		data = amf_associative_array_get_data(n);
		type = amf_data_get_type(data);

		/* TODO: check UTF-8 strings, in key, and value if string type */
		/* duration (number) */
		if (!strcmp((char*) name, "duration")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_duration;
				file_duration = amf_number_get_value(data);
				if(this->duration > file_duration)
					goto end;
				amf_number_set_value(data,double2int(this->duration));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for duration: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* lasttimestamp: (number) */
		if (!strcmp((char*) name, "lasttimestamp")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lasttimestamp;
				file_lasttimestamp = amf_number_get_value(data);
				if(this->lasttimestamp > file_lasttimestamp)
					goto end;
				amf_number_set_value(data,double2int(this->lasttimestamp));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lasttimestamp: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* lastkeyframetimestamp: (number) */
		if (!strcmp((char*) name, "lastkeyframetimestamp")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lastkeyframetimestamp;
				file_lastkeyframetimestamp = amf_number_get_value(data);
				if(this->lastkeyframetimestamp > file_lastkeyframetimestamp)
					goto end;
				amf_number_set_value(data,double2int(this->lastkeyframetimestamp));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lastkeyframetimestamp: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* filesize: (number) */
		if (!strcmp((char*) name, "filesize")) {
			if (type == AMF_TYPE_NUMBER) {
				//number64 file_filesize;

				//file_filesize = amf_number_get_value(data);
				amf_number_set_value(data,double2int(this->filesize));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* videosize: (number) */
		if (!strcmp((char*) name, "videosize")) {
			if (type == AMF_TYPE_NUMBER) {
//				number64 file_videosize;
//				file_videosize = amf_number_get_value(data);
//				data_value = int2double(file_videosize) - duration_video_size;
				amf_number_set_value(data,double2int(this->videosize));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for videosize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* audiosize: (number) */
		if (!strcmp((char*) name, "audiosize")) {
			if (type == AMF_TYPE_NUMBER) {
//				number64 file_audiosize;
//				file_audiosize = amf_number_get_value(data);
//				data_value = int2double(file_audiosize) - duration_audio_size;
				amf_number_set_value(data,double2int(this->audiosize));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for audiosize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* datasize: (number) */
		if (!strcmp((char*) name, "datasize")) {
			if (type == AMF_TYPE_NUMBER) {
//				number64 file_datasize;
//				file_datasize = amf_number_get_value(data);
//				data_value = int2double(file_datasize) - duration_video_size - duration_audio_size;
				amf_number_set_value(data,double2int(this->datasize));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for datasize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* keyframes: (object) */
		if (!strcmp((char*) name, "keyframes")) {
			if (type == AMF_TYPE_OBJECT) {
				amf_data * file_times, *file_filepositions;

				file_times = amf_object_get(data, "times");
				file_filepositions = amf_object_get(data, "filepositions");

				/* check sub-arrays' presence */
				if (file_times == NULL) {
					TSDebug(PLUGIN_NAME,"Missing times metadata\n");
				}
				if (file_filepositions == NULL) {
					TSDebug(PLUGIN_NAME,"Missing filepositions metadata\n");
				}

				if (file_times != NULL && file_filepositions != NULL) {
					/* check types */
					uint8 times_type, fp_type;

					times_type = amf_data_get_type(file_times);
					if (times_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"times_type != AMF_TYPE_ARRAY －－ invalid type for times: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(times_type));
					}

					fp_type = amf_data_get_type(file_filepositions);
					if (fp_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"fp_type != AMF_TYPE_ARRAY －－ invalid type for filepositions: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(fp_type));
					}

					if (times_type == AMF_TYPE_ARRAY && fp_type == AMF_TYPE_ARRAY) {
						number64 last_file_time;
						int have_last_time;
						amf_node * ff_node, *ft_node;

						/* iterate in parallel, report diffs */
						last_file_time = 0;
						have_last_time = 0;

						ft_node = amf_array_first(file_times);
						ff_node = amf_array_first(file_filepositions);

						amf_node * first_t_node, *first_p_node;
						first_t_node = ft_node;
						first_p_node = ff_node;
						number64 f_time,f_position;
						double df_time ,df_position;
                        uint32_t arr_len;
                        arr_len = 0;
                        bool delete_node;
						while (ft_node != NULL && ff_node != NULL) {
							f_time =0;
							f_position = 0;
							df_time = 0;
							df_position = 0;
                            arr_len += 1;
                            delete_node = false;
							/* time */
							if (amf_data_get_type(amf_array_get(ft_node)) != AMF_TYPE_NUMBER) {
								TSDebug(PLUGIN_NAME,"!= AMF_TYPE_NUMBER  －－ invalid type for time: expected %s, got %s\n",
										get_amf_type_string(AMF_TYPE_NUMBER),
										get_amf_type_string(type));
							} else {
								f_time = amf_number_get_value(amf_array_get(ft_node));
								df_time = int2double(f_time);
								/* check for duplicate time, can happen in H.264 files */
								if (have_last_time && last_file_time == f_time) {
									TSDebug(PLUGIN_NAME,"Duplicate keyframe time: %lf\n",int2double(f_time));
								}
								have_last_time = 1;
								last_file_time = f_time;
							}

							/* position */
							if (amf_data_get_type(amf_array_get(ff_node)) != AMF_TYPE_NUMBER) {
								TSDebug(PLUGIN_NAME,
										"!= AMF_TYPE_NUMBER invalid type for file position: expected %s, got %s\n",
										get_amf_type_string(AMF_TYPE_NUMBER),
										get_amf_type_string(type));
							} else {
								f_position = amf_number_get_value(amf_array_get(ff_node));
								TSDebug(PLUGIN_NAME,"invalid keyframe file position: expected  got %lf\n",int2double(f_position));
							}

                            if(this->start <= 0 && this->end > 0) {
                                if (arr_len > this->end_keyframe_len) {
                                    delete_node = true;
//                                    first_t_node->next = amf_array_next(ft_node);
//                                    first_p_node->next = amf_array_next(ff_node);
//                                    first_t_node->next->prev = first_t_node;
//                                    first_p_node->next->prev = first_p_node;
//
//                                    amf_array_delete(file_times,ft_node);
//                                    amf_array_delete(file_filepositions,ff_node);
//                                    ft_node = first_t_node->next;
//                                    ff_node = first_p_node->next;
//
//                                    continue;
                                }
//                                else {
//                                    amf_number_set_value(amf_array_get(ft_node), double2int(df_time - duration_time));
//                                    amf_number_set_value(amf_array_get(ff_node), double2int(df_position - duration_file_size));
//                                }
                            } else if (this->start > 0 && this->end <= 0) {

                                if (arr_len < this->start_keyframe_len) {
                                    delete_node = true;
//                                    first_t_node->next = amf_array_next(ft_node);
//                                    first_p_node->next = amf_array_next(ff_node);
//                                    first_t_node->next->prev = first_t_node;
//                                    first_p_node->next->prev = first_p_node;
//
//                                    amf_array_delete(file_times,ft_node);
//                                    amf_array_delete(file_filepositions,ff_node);
//                                    ft_node = first_t_node->next;
//                                    ff_node = first_p_node->next;
//
//                                    continue;
                                } else {
                                    amf_number_set_value(amf_array_get(ft_node), double2int(df_time - this->start_duration_time));
                                    amf_number_set_value(amf_array_get(ff_node), double2int(df_position - this->start_duration_file_size));
                                }
                            } else if (this->start > 0 && this->end > 0) {

                                if (arr_len < this->start_keyframe_len) {
                                    delete_node = true;
//                                    first_t_node->next = amf_array_next(ft_node);
//                                    first_p_node->next = amf_array_next(ff_node);
//                                    first_t_node->next->prev = first_t_node;
//                                    first_p_node->next->prev = first_p_node;
//
//                                    amf_array_delete(file_times,ft_node);
//                                    amf_array_delete(file_filepositions,ff_node);
//                                    ft_node = first_t_node->next;
//                                    ff_node = first_p_node->next;
//                                    continue;
                                } else if ( arr_len >= this->start_keyframe_len && arr_len <= this->end_keyframe_len) {
                                    amf_number_set_value(amf_array_get(ft_node), double2int(df_time - this->start_duration_time));
                                    amf_number_set_value(amf_array_get(ff_node), double2int(df_position - this->start_duration_file_size));
                                } else {
                                    delete_node = true;
//                                    first_t_node->next = amf_array_next(ft_node);
//                                    first_p_node->next = amf_array_next(ff_node);
//                                    first_t_node->next->prev = first_t_node;
//                                    first_p_node->next->prev = first_p_node;
//
//                                    amf_array_delete(file_times,ft_node);
//                                    amf_array_delete(file_filepositions,ff_node);
//                                    ft_node = first_t_node->next;
//                                    ff_node = first_p_node->next;
//                                    continue;
                                }
                            }

                            if(delete_node) {
                                TSDebug(PLUGIN_NAME, "[update_flv_meta_data] delete keyframe n=%d time=%lf, positions=%lf",arr_len,df_time, df_position);
                                first_t_node->next = amf_array_next(ft_node);
                                first_p_node->next = amf_array_next(ff_node);
                                first_t_node->next->prev = first_t_node;
                                first_p_node->next->prev = first_p_node;

                                amf_array_delete(file_times,ft_node);
                                amf_array_delete(file_filepositions,ff_node);
                                ft_node = first_t_node->next;
                                ff_node = first_p_node->next;
                                continue;
                            }

							/* next entry */
							ft_node = amf_array_next(ft_node);
							ff_node = amf_array_next(ff_node);
						}
					}
				}
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for keyframes: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_BOOLEAN),
						get_amf_type_string(type));
			}
		}//end keyframes
	}// end for


    TSDebug(PLUGIN_NAME,"end on_medata_size: %u",amf_data_size(on_metadata));
	//on_medata_size - amf_data_size(on_metadata);  //等于丢弃了多少关键帧
//	for (n = amf_associative_array_first(on_metadata); n != NULL; n =
//			amf_associative_array_next(n)) {
//		byte * name;
//		amf_data * data;
//		byte type;
//
//		name = amf_string_get_bytes(amf_associative_array_get_name(n));
//		data = amf_associative_array_get_data(n);
//		type = amf_data_get_type(data);
//		/* filesize: (number) */
//		if (!strcmp((char*) name, "filesize")) {
//			if (type == AMF_TYPE_NUMBER) {
//				number64 file_filesize;
//
//				file_filesize = amf_number_get_value(data);
//				double data_value = int2double(file_filesize) -  (on_medata_size - amf_data_size(on_metadata));  //此处有bug ，因为在删除onmetadata关键帧的时候, 大小会再次变化，还需要再次修改
//				TSDebug(PLUGIN_NAME,"filesize should be got %lf %lf\n", data_value,int2double(file_filesize));
//				amf_number_set_value(data,double2int(data_value));
//				break;
//			} else {
//				TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
//						get_amf_type_string(AMF_TYPE_NUMBER),
//						get_amf_type_string(type));
//			}
//		}
//
//	}

end:
	uint32 meta_data_length = amf_data_size(on_metadata_name) + amf_data_size(on_metadata);
	prev_tag_size = flv_tag_size + meta_data_length;

	tag.body_length = uint32_to_uint24_be(meta_data_length);
	IOBufferReaderCopy(modify_meta_reader, &tag, flv_tag_size);

	byte metadata_name_b[amf_data_size(on_metadata_name)];
	amf_data_buffer_write(on_metadata_name,metadata_name_b, amf_data_size(on_metadata_name));
	IOBufferReaderCopy(modify_meta_reader, metadata_name_b, amf_data_size(on_metadata_name));

	byte *on_medata_data_b = (byte *)TSmalloc(sizeof(byte) * amf_data_size(on_metadata));
	amf_data_buffer_write(on_metadata,on_medata_data_b, amf_data_size(on_metadata));
	IOBufferReaderCopy(modify_meta_reader, on_medata_data_b , amf_data_size(on_metadata));
	/* first "previous tag size" */
	uint32_be size = swap_uint32(prev_tag_size);
	IOBufferReaderCopy(modify_meta_reader, &size, sizeof(uint32_be));

	amf_data_free(on_metadata);
	amf_data_free(on_metadata_name);

    TSDebug(PLUGIN_NAME, "update_flv_meta_data modify meta data length=%ld",TSIOBufferReaderAvail(modify_meta_reader));
	return 1;
}


static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf,
		int64_t length) {
	int64_t avail, need, n;
	const char *start;
	TSIOBufferBlock blk;

	n = 0;
	blk = TSIOBufferReaderStart(readerp);

	while (blk) {
		start = TSIOBufferBlockReadStart(blk, readerp, &avail);
		need = length < avail ? length : avail;

		if (need > 0) {
			memcpy((char *) buf + n, start, need);
			length -= need;
			n += need;
		}

		if (length == 0)
			break;

		blk = TSIOBufferBlockNext(blk);
	}

	return n;
}


int FlvTag::flv_read_metadata(byte *stream,amf_data ** name, amf_data ** data, size_t maxbytes) {
    amf_data * d;
//    byte error_code;

    /* read metadata name */
//    d = amf_data_file_read(stream);
    d = amf_data_buffer_read(stream,maxbytes);
    *name = d;
//    error_code = amf_data_get_error_code(d);

    size_t name_length = amf_data_size(d);
    /* if only name can be read, metadata are invalid */
//    data_size = amf_data_size(d);

    /* read metadata contents */
    d = amf_data_buffer_read(stream+name_length ,maxbytes);
    *data = d;
//    error_code = amf_data_get_error_code(d);

//    data_size = amf_data_size(d);

    return 0;
}

/* get string representing given AMF type */
static const char * get_amf_type_string(byte type) {
	switch (type) {
	case AMF_TYPE_NUMBER:
		return "Number";
	case AMF_TYPE_BOOLEAN:
		return "Boolean";
	case AMF_TYPE_STRING:
		return "String";
	case AMF_TYPE_NULL:
		return "Null";
	case AMF_TYPE_UNDEFINED:
		return "Undefined";
		/*case AMF_TYPE_REFERENCE:*/
	case AMF_TYPE_OBJECT:
		return "Object";
	case AMF_TYPE_ASSOCIATIVE_ARRAY:
		return "Associative array";
	case AMF_TYPE_ARRAY:
		return "Array";
	case AMF_TYPE_DATE:
		return "Date";
		/*case AMF_TYPE_SIMPLEOBJECT:*/
	case AMF_TYPE_XML:
		return "XML";
	case AMF_TYPE_CLASS:
		return "Class";
	default:
		return "Unknown type";
	}
}

size_t FlvTag::get_flv_tag_size() {
	flv_tag  tag;
	return (sizeof(tag.type) + sizeof(tag.body_length) +sizeof(tag.timestamp) + sizeof(tag.timestamp_extended) + sizeof(tag.stream_id));
}

int FlvTag::flv_read_flv_tag(TSIOBufferReader readerp, flv_tag * tag) {

	size_t flv_tag_size = get_flv_tag_size();
	byte buf[flv_tag_size];
	IOBufferReaderCopy(readerp, buf, flv_tag_size);

	memcpy(&tag->type,buf,sizeof(tag->type));
	memcpy(&tag->body_length,buf + sizeof(tag->type),sizeof(tag->body_length));
	memcpy(&tag->timestamp,buf +sizeof(tag->type) +sizeof(tag->body_length) ,sizeof(tag->timestamp));
	memcpy(&tag->timestamp_extended,buf + sizeof(tag->type) +sizeof(tag->body_length) +sizeof(tag->timestamp),sizeof(tag->timestamp_extended));
	memcpy(&tag->stream_id,buf + sizeof(tag->type) +sizeof(tag->body_length) +sizeof(tag->timestamp) +sizeof(tag->timestamp_extended),sizeof(tag->stream_id));

    return 0;
}

size_t FlvTag::get_flv_header_size() {
	flv_header  header;
	return (sizeof(header.signature) + sizeof(header.version) +sizeof(header.flags) + sizeof(header.offset));
}

int FlvTag::flv_read_flv_header(TSIOBufferReader readerp, flv_header * header) {

	IOBufferReaderCopy(readerp, &header->signature, sizeof(header->signature));
	TSIOBufferReaderConsume(readerp, sizeof(header->signature));

	IOBufferReaderCopy(readerp, &header->version, sizeof(header->version));
	TSIOBufferReaderConsume(readerp, sizeof(header->version));

	IOBufferReaderCopy(readerp, &header->flags, sizeof(header->flags));
	TSIOBufferReaderConsume(readerp, sizeof(header->flags));

	IOBufferReaderCopy(readerp, &header->offset, sizeof(header->offset));
	TSIOBufferReaderConsume(readerp, sizeof(header->offset));

    if (header->signature[0] != 'F'
    || header->signature[1] != 'L'
    || header->signature[2] != 'V') {
		this->end = 0;
        return -1;
    }

    return 0;
}