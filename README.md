# ts_flv
    主要实现FLV根据参数start,end 字节大小拖动，并更新脚本数据；如果带end 参数，脚本数据里就必须有关键帧，否则end不生效。
    
# 根据客户需求
    flv meta_data只修改了duration
    其他一些头信息修改，可以根据自身的需求在 flv_tag.cc的 update_flv_meta_data 函数中修改
