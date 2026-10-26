#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *naki_rendition_config_json(const char *session,size_t bytes,yyjson_val *output,char *error,size_t cap) {
    yyjson_doc *source=yyjson_read(session,bytes,0);
    yyjson_mut_doc *d=source?yyjson_mut_doc_new(NULL):NULL;char *result=NULL;
    if(!d)goto done;
    yyjson_mut_val *root=yyjson_val_mut_copy(d,yyjson_doc_get_root(source));yyjson_mut_doc_set_root(d,root);
    const char *private_keys[]={"outputs","rendition_backend","rendition_auto_restart","rendition_restart_limit","rendition_restart_backoff_ms"};
    for(unsigned i=0;i<sizeof(private_keys)/sizeof(*private_keys);i++)yyjson_mut_obj_remove_key(root,private_keys[i]);
    if(output) {
        if(!yyjson_is_obj(output))goto done;
        size_t i,n;yyjson_val *k,*v;
        yyjson_obj_foreach(output,i,n,k,v) {
            const char *name=yyjson_get_str(k);
            const char *fields[]={"output","output_format","encoder","encoder_preset","encoder_profile","encoder_bit_depth","encoder_input_bit_depth","encoder_native8_reference","width","height","bitrate","encoder_threads","audio_copy","mux_wait_ms","watermark","sei","output_fps","rate_control","qp","maxrate"};
            for(unsigned j=0;j<sizeof(fields)/sizeof(*fields);j++)if(!strcmp(name,fields[j])) {
                yyjson_mut_obj_remove_key(root,name);yyjson_mut_obj_add_val(d,root,name,yyjson_val_mut_copy(d,v));break;
            }
        }
    }
    size_t length=0;char *text=yyjson_mut_write(d,0,&length);Config validated;
    if(text&&length<=60000&&!config_parse(text,length,&validated,error,cap))result=text;else free(text);
done:
    if(!result&&error&&cap&&!*error)snprintf(error,cap,"invalid rendition configuration");
    if(d)yyjson_mut_doc_free(d);
    if(source)yyjson_doc_free(source);
    return result;
}
