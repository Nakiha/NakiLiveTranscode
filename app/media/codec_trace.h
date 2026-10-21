#ifndef NAKI_CODEC_TRACE_H
#define NAKI_CODEC_TRACE_H
#include <libavutil/frame.h>
#include <yyjson.h>
#define NAKI_ENCODER_TRACE_FIELDS(X) X(input_us) \
    X(group_last_input_us) \
    X(group_formed_us) \
    X(preprocess_begin_us) \
    X(group_ready_us) \
    X(mctf_begin_us) \
    X(mctf_end_us) \
    X(dispatch_us) \
    X(compress_return_us) \
    X(task_first_us) \
    X(ctu_done_us) \
    X(finalize_begin_us) \
    X(finalize_end_us) \
    X(reconstructed_us) \
    X(au_us) \
    X(preprocess_cpu_us) \
    X(dispatch_cpu_us) \
    X(finalize_cpu_us) \
    X(ctu_cpu_us) \
    X(deblock_cpu_us) \
    X(sao_cpu_us) \
    X(alf_cpu_us) \
    X(finish_cpu_us) \
    X(task_wall_us) \
    X(task_calls) \
    X(gop_enter_us) \
    X(gop_init_begin_us) \
    X(gop_init_end_us) \
    X(gop_init_cpu_us) \
    X(gop_previous_done_us) \
    X(gop_visits) \
    X(gop_order_blocks) \
    X(gop_analysis_blocks) \
    X(gop_ratecap_blocks) \
    X(gop_reference_blocks) \
    X(gop_aps_blocks) \
    X(gop_encoder_blocks) \
    X(gop_rc_blocks) \
    X(byte_me_blocks) \
    X(byte_reference_pack_us) \
    X(preprocess_cpu_wall_us) \
    X(dispatch_cpu_wall_us) \
    X(finalize_cpu_wall_us) \
    X(gop_init_cpu_wall_us)
typedef struct NakiEncoderTrace {
    int64_t version,cts,poc;
#define DECLARE_TRACE(n) int64_t n;
    NAKI_ENCODER_TRACE_FIELDS(DECLARE_TRACE)
#undef DECLARE_TRACE
} NakiEncoderTrace;
static inline void naki_decode_trace_json(yyjson_mut_doc *doc,const AVLiveDecodeTrace *t) {
    yyjson_mut_val *o=yyjson_mut_obj(doc);yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"decoder_internal",o);
    yyjson_mut_obj_add_int(doc,o,"version",t->version);
#define JSON_TRACE(n) yyjson_mut_obj_add_int(doc,o,#n,t->n);
    AV_LIVE_DECODE_TRACE_FIELDS(JSON_TRACE)
#undef JSON_TRACE
}
static inline void naki_encoder_trace_json(yyjson_mut_doc *doc,const NakiEncoderTrace *t) {
    yyjson_mut_val *o=yyjson_mut_obj(doc);yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"encoder_internal",o);
    yyjson_mut_obj_add_int(doc,o,"version",t->version);yyjson_mut_obj_add_int(doc,o,"cts",t->cts);yyjson_mut_obj_add_int(doc,o,"poc",t->poc);
#define JSON_TRACE(n) yyjson_mut_obj_add_int(doc,o,#n,t->n);
    NAKI_ENCODER_TRACE_FIELDS(JSON_TRACE)
#undef JSON_TRACE
}
#endif
