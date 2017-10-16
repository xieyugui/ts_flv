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
    int64_t avail;

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
		this->content_length = this->cl;
        if(this->start <= 0 && this->end > 0) {
            this->content_length = this->end;
        } else if (this->start > 0 && this->end <= 0) {
            this->content_length = this->content_length -  this->start_duration_file_size;
        } else if (this->start > 0 && this->end > 0) {
            this->content_length = this->end - this->start_duration_file_size;
        }

		TSDebug(PLUGIN_NAME, "[process_tag] content_length = %ld", content_length);
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
        TSIOBufferCopy(buffer, modify_meta_reader, modify_meta_avail, 0);
        TSIOBufferReaderConsume(modify_meta_reader, modify_meta_avail);
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
	uint32 body_length,timestamp;
	size_t flv_tag_length = get_flv_tag_size();
    char        buf[12];

	avail = TSIOBufferReaderAvail(tag_reader);

    do {
        flv_tag tag;

        if (avail < flv_tag_length + 1) // find video key frame
            return 0;
        flv_read_flv_tag(tag_reader, &tag);
        IOBufferReaderCopy(tag_reader, buf, flv_tag_length + 1);

		body_length = flv_tag_get_body_length(tag);

		sz = flv_tag_length + body_length + sizeof(uint32_be); //dup body keyframe size

		if (avail < sz)     // insure the whole tag
			return 0;

        timestamp = flv_tag_get_timestamp(tag);

        if (timestamp != 0)
            goto process_end;

        if (tag.type == FLV_TAG_TYPE_VIDEO && (((uint8_t)buf[11]) >> 4) == 1) {
            if (!key_found) {
                key_found = true;
            } else {
                goto process_end;
            }
        }
        TSIOBufferCopy(meta_buffer, tag_reader, sz, 0);
        TSIOBufferReaderConsume(tag_reader, sz);

        TSDebug(PLUGIN_NAME, "[process_meta_body] sz=%lu, tag_pos=%lu", sz, tag_pos);

        tag_pos += sz;
		avail -= sz;

    } while(avail > 0);
    return 0;

	process_end:
    TSDebug(PLUGIN_NAME, "[process_meta_body] meta_buff=%lu", TSIOBufferReaderAvail(meta_reader));
	key_found = false;

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

	name = NULL;
	data = NULL;
	on_metadata = NULL;
	on_metadata_name = NULL;

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

		/* lasttimestamp: (number) */
		if (!strcmp((char*) name, "lasttimestamp")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lasttimestamp;
				file_lasttimestamp = amf_number_get_value(data);
				this->lasttimestamp = int2double(file_lasttimestamp);
				TSDebug(PLUGIN_NAME,"[parse_meta_body] lasttimestamp=%lf",this->lasttimestamp);
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
				this->lastkeyframetimestamp = int2double(file_lastkeyframetimestamp);
				TSDebug(PLUGIN_NAME,"[parse_meta_body] lastkeyframetimestamp=%lf",this->lastkeyframetimestamp);
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lastkeyframetimestamp: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* lastkeyframelocation: (number) */
		if (!strcmp((char*) name, "lastkeyframelocation")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lastkeyframelocation;
				file_lastkeyframelocation = amf_number_get_value(data);
				this->lastkeyframelocation = int2double(file_lastkeyframelocation);
				TSDebug(PLUGIN_NAME,"[parse_meta_body] lastkeyframelocation=%lf",this->lastkeyframelocation);
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lastkeyframelocation: expected %s, got %s\n",
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
						this->keyframes_len = amf_array_size(file_filepositions);
						amf_node * ff_node, *ft_node;

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
							}

							TSDebug(PLUGIN_NAME,"[parse_meta_body] keyframes time=%lf,position=%lf",df_time, df_position);

							TSDebug(PLUGIN_NAME,"[parse_meta_body] start=%lu",start);
							if (start > 0) {
								TSDebug(PLUGIN_NAME,"[parse_meta_body] start > 0");
								if (end > 0) {
									TSDebug(PLUGIN_NAME,"[parse_meta_body] start > 0    end > 0");
									if (df_position < start)
										start_keyframe_len += 1;
									else if(df_position >= start and df_position <= end) {
										if (start_keyframe_positions <= 0) {
											start_keyframe_len += 1;
											start_keyframe_positions = df_position;
											start_keyframe_times = df_time;
										}
									} else {
                                        if(end_keyframe_times > 0)
										    break;
									}
									end_keyframe_len += 1;
									end_keyframe_positions = df_position;
									end_keyframe_times = df_time;
								} else {
									TSDebug(PLUGIN_NAME,"[parse_meta_body] start > 0 else");
									start_keyframe_len += 1;
									if (df_position >= start) {
										start_keyframe_positions = df_position;
										start_keyframe_times = df_time;
										break;
									}
								}
							} else {
								if (df_position > end && end_keyframe_times > 0)
									break;
								end_keyframe_len += 1;
								end_keyframe_positions = df_position;
								end_keyframe_times = df_time;
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
	amf_data_free(on_metadata);
	amf_data_free(on_metadata_name);

	//如果没有关键帧，就不实现end功能
	if (!haskeyframe) {
		this->end = 0;
		this->end_keyframe_len = 0;
		this->end_keyframe_positions = 0;
	} else {
		this->start = (uint64_t)this->start_keyframe_positions;
		this->end = (uint64_t)this->end_keyframe_positions;
		if (this->end > 0) {
			lastkeyframelocation = end_keyframe_positions;
			lastkeyframetimestamp = end_keyframe_times;
			lasttimestamp = end_keyframe_times;
		}

		TSDebug(PLUGIN_NAME,"start_keyframe=%d,%lf end_keyframe=%d,%lf",start_keyframe_len,start_keyframe_positions
				,end_keyframe_len,end_keyframe_positions);
		TSDebug(PLUGIN_NAME,"lastkeyframe location=%lf, timestamp=%lf lasttimestamp=%lf, end_keyframe_len=%d",lastkeyframelocation,lastkeyframetimestamp
				,lasttimestamp,end_keyframe_len);

		this->real_end_keyframe_positions = this->end;
		//计算delete 的关键帧大小
		uint32 k_len;
		k_len = 0;
		if(this->start_keyframe_len > 0)
			k_len = this->start_keyframe_len -1;
		if (this->end_keyframe_len > 0)
			k_len += keyframes_len - this->end_keyframe_len;
		//一个keyframe 为9  删除ft_node 和ff_node
		this->delete_meta_size = k_len * 18;
		TSDebug(PLUGIN_NAME,"[parse_meta_body] delete_meta_size=%lu",this->delete_meta_size);

	}

	if (this->start <=0 && this->end <= 0) {
		this->end = 0;
		this->real_end_keyframe_positions = this->end;
		return -1;
	}

	TSDebug(PLUGIN_NAME,"[parse_meta_body] start=%lu end=%lu",this->start, this->end);

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
		if (avail < (int64_t)flv_tag_length )
			return 0;

		flv_read_flv_tag(tag_reader, &tag);
		body_length = flv_tag_get_body_length(tag);
		sz = flv_tag_length + body_length + sizeof(uint32_be); //tag->(tag header, tag body), tagsize

		if (avail < sz)     // insure the whole tag
			return 0;

		start_dup_size += sz;
        TSDebug(PLUGIN_NAME,"[process_medial_body] start_duration_file_size=%lu,start_dup_size=%lu",start_duration_file_size,start_dup_size);
		timestamp = flv_tag_get_timestamp(tag);

		if (tag.type == FLV_TAG_TYPE_VIDEO) {
			TSDebug(PLUGIN_NAME,"[process_medial_body] FLV_TAG_TYPE_VIDEO");
			TSDebug(PLUGIN_NAME,"[process_medial_body] start_dup_size=%lu, start=%lu",start_dup_size,start);
			if (start_dup_size <= start) {
				start_duration_time = timestamp; //ms
			} else {

				TSDebug(PLUGIN_NAME, "process_medial_body success!!! tag_pos= %ld,start_duration_file_size=%lu",tag_pos, start_duration_file_size);
                //return 1;
				this->current_handler = &FlvTag::update_flv_meta_data;
				return update_flv_meta_data();
			}
		}

		start_duration_file_size += sz;

		TSIOBufferReaderConsume(tag_reader, sz);

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
 * 本次暂时只是更新了duration，lasttimestamp，filesize
 */
int FlvTag::update_flv_meta_data() {
    this->start_duration_time = this->start_duration_time / 1000;
//    ---this->start_duration_file_size += this->delete_meta_size;
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start_duration_time=%lf", this->start_duration_time);
    if(this->start <= 0 && this->end > 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start <= 0 && this->end > 0");
        this->start_duration_time = 0;
        this->start_duration_video_size = 0;
        this->start_duration_audio_size = 0;
        this->duration = this->lastkeyframetimestamp;
        this->filesize = this->lastkeyframelocation;
        //keyframes 在遍历过程中删除
    } else if (this->start > 0 && this->end <= 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start > 0 && this->end <= 0");
        this->lastkeyframelocation = this->lastkeyframelocation - this->start_duration_file_size;
        this->lastkeyframetimestamp = this->lastkeyframetimestamp - this->start_duration_time;
        this->lasttimestamp = this->lastkeyframetimestamp;
		this->duration = this->lastkeyframetimestamp;
        this->filesize = this->filesize - (double)this->start_duration_file_size;
//        this->videosize = this->videosize - this->start_duration_video_size;--
//        this->audiosize = this->audiosize - this->start_duration_audio_size;--
//        this->datasize = this->datasize - this->start_duration_video_size - this->start_duration_audio_size;--
        //keyframes 在遍历过程中删除
    } else if (this->start > 0 && this->end > 0) {
        TSDebug(PLUGIN_NAME, "[update_flv_meta_data] this->start > 0 && this->end > 0");
        this->lastkeyframelocation = this->lastkeyframelocation - this->start_duration_file_size;
        this->lastkeyframetimestamp = this->lastkeyframetimestamp - this->start_keyframe_times;
        this->lasttimestamp = this->lastkeyframetimestamp;
        this->duration = this->lastkeyframetimestamp;
        this->filesize = this->lastkeyframelocation;
        //keyframes 在遍历过程中删除
    } else {
        this->end = 0;
        this->real_end_keyframe_positions = this->end;
        return -1;
    }

    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_file_size=%lu",this->start_duration_file_size);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_time=%lf",this->start_duration_time);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_video_size=%lu",this->start_duration_video_size);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] start_duration_audio_size=%lu",this->start_duration_audio_size);

    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lastkeyframelocation=%lf",this->lastkeyframelocation);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lastkeyframetimestamp=%lf",this->lastkeyframetimestamp);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] lasttimestamp=%lf",this->lasttimestamp);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] duration=%lf",this->duration);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] filesize=%lf",this->filesize);


    int64_t avail;
    avail = TSIOBufferReaderAvail(meta_reader);
    TSDebug(PLUGIN_NAME, "update_flv_meta_data meta data length=%ld",avail);

    clear_copy_meta_buffer();
    create_copy_meta_buffer();
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
    name = NULL;
    data = NULL;
    on_metadata = NULL;
    on_metadata_name = NULL;

    flv_read_flv_tag(copy_meta_reader, &tag);
    TSIOBufferReaderConsume(copy_meta_reader, flv_tag_size);

    body_length = flv_tag_get_body_length(tag);
    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] tag header=%u, tag body=%u",flv_tag_size,body_length);

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
        TSDebug(PLUGIN_NAME,"start on_medata_size: %lu",amf_data_size(data));
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
                amf_number_set_value(data,double2int(this->lastkeyframetimestamp));
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for lastkeyframetimestamp: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

        /* lastkeyframelocation: (number) */
        if (!strcmp((char*) name, "lastkeyframelocation")) {
            if (type == AMF_TYPE_NUMBER) {
                amf_number_set_value(data,double2int(this->lastkeyframelocation));
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for lastkeyframelocation: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

        /* filesize: (number) */
        if (!strcmp((char*) name, "filesize")) {
            if (type == AMF_TYPE_NUMBER) {
                amf_number_set_value(data,double2int(this->filesize));
            } else {
                TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
                        get_amf_type_string(AMF_TYPE_NUMBER),
                        get_amf_type_string(type));
            }
        }

    }// end for

//    TSDebug(PLUGIN_NAME,"end delete_meta_size: %lu",delete_meta_size);
//    size_t delete_size;
//    delete_size = on_medata_size- amf_data_size(on_metadata);
//    if (delete_meta_size != delete_size)
//        this->delete_meta_size = delete_size;

//    TSDebug(PLUGIN_NAME,"end on_medata_size: %u, delete=%lu",amf_data_size(on_metadata), on_medata_size- amf_data_size(on_metadata));
    end:
    uint32 meta_data_length = amf_data_size(on_metadata_name) + amf_data_size(on_metadata);
    prev_tag_size = flv_tag_size + meta_data_length;

    TSDebug(PLUGIN_NAME, "[update_flv_meta_data] prev_tag_size=%lu",prev_tag_size);

    tag.body_length = uint32_to_uint24_be(meta_data_length);

    TSIOBufferWrite(modify_meta_buffer,&tag, (int64_t)flv_tag_size);

    byte metadata_name_b[amf_data_size(on_metadata_name)];
    amf_data_buffer_write(on_metadata_name,metadata_name_b, amf_data_size(on_metadata_name));
    TSIOBufferWrite(modify_meta_buffer, metadata_name_b, (int64_t)amf_data_size(on_metadata_name));

    byte *on_medata_data_b = (byte *)TSmalloc(sizeof(byte) * amf_data_size(on_metadata));
    amf_data_buffer_write(on_metadata,on_medata_data_b, amf_data_size(on_metadata));
    TSIOBufferWrite(modify_meta_buffer, on_medata_data_b, (int64_t)amf_data_size(on_metadata));

    /* first "previous tag size" */
    uint32_be size = swap_uint32(prev_tag_size);
    TSIOBufferWrite(modify_meta_buffer, &size,  (int64_t)sizeof(uint32_be));

    //将非meta的数据copy
    TSIOBufferCopy(modify_meta_buffer, copy_meta_reader,TSIOBufferReaderAvail(copy_meta_reader) , 0);

    amf_data_free(on_metadata);
    amf_data_free(on_metadata_name);


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

void FlvTag::create_copy_meta_buffer() {
    copy_meta_buffer = TSIOBufferCreate();
    copy_meta_reader = TSIOBufferReaderAlloc(copy_meta_buffer);
}

void FlvTag::clear_copy_meta_buffer(){
    if (copy_meta_reader) {
        TSIOBufferReaderFree(copy_meta_reader);
        copy_meta_reader = NULL;
    }

    if (copy_meta_buffer) {
        TSIOBufferDestroy(copy_meta_buffer);
        copy_meta_buffer = NULL;
    }
}
