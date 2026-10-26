#include "component_update.h"
#include "identity.h"
#include "intent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *error,size_t size,const char *path,const char *reason) {
    snprintf(error,size,"%s: %s",path,reason);return -1;
}
static int keys(yyjson_val *object,const char *allowed,const char *path,char *error,size_t size) {
    if(!yyjson_is_obj(object))return fail(error,size,path,"expected object");
    size_t index,max;yyjson_val *key,*value;
    yyjson_obj_foreach(object,index,max,key,value) {
        char token[160];const char *name=yyjson_get_str(key);
        snprintf(token,sizeof(token),"|%s|",name);
        if(strchr(name,'|')||!strstr(allowed,token)) {char field[256];snprintf(field,sizeof(field),"%s.%s",path,name);return fail(error,size,field,"unsupported field");}
    }
    return 0;
}
static unsigned present(yyjson_val *object,const char *key,unsigned bit) {return yyjson_obj_get(object,key)?bit:0;}
static int topology(yyjson_val *root,const char *job,NakiUpdateRequest *out,char *error,size_t size) {
    if(keys(root,"|operation_id||target||expected_revision||action||spec||expected_generation|","update",error,size))return -1;
    yyjson_val *target=yyjson_obj_get(root,"target"),*action=yyjson_obj_get(root,"action"),*revision=yyjson_obj_get(root,"expected_revision");
    if(keys(target,"|output_id||component||destination_id|","target",error,size))return -1;
    if(!naki_json_id(yyjson_obj_get(root,"operation_id"),128)||!naki_json_id(yyjson_obj_get(target,"output_id"),32)||!yyjson_is_uint(revision)||!yyjson_get_uint(revision)||yyjson_get_uint(revision)>=INT64_MAX||!yyjson_is_str(action))return fail(error,size,"topology","invalid identity, revision or action");
    const char *names[]={"add_rendition","remove_rendition","restart_rendition","add_destination","remove_destination","request_random_access"};
    NakiUpdateRequest u={0};for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(!strcmp(yyjson_get_str(action),names[i]))u.action=(int)i+1;
    if(!u.action)return fail(error,size,"action","unsupported topology action");
    const char *component=yyjson_get_str(yyjson_obj_get(target,"component"));
    int destination=u.action==NAKI_DESTINATION_ADD||u.action==NAKI_DESTINATION_REMOVE;
    if(!component||strcmp(component,destination?"destination":"rendition"))return fail(error,size,"target.component","does not match action");
    yyjson_val *destination_id=yyjson_obj_get(target,"destination_id");
    if(destination){if(!naki_json_id(destination_id,32))return fail(error,size,"target.destination_id","required");strcpy(u.destination_id,yyjson_get_str(destination_id));}
    else if(destination_id)return fail(error,size,"target.destination_id","not allowed for rendition action");
    u.spec=yyjson_obj_get(root,"spec");
    if(u.action==NAKI_RENDITION_ADD||u.action==NAKI_DESTINATION_ADD){if(!yyjson_is_obj(u.spec))return fail(error,size,"spec","required object");}
    else if(u.spec)return fail(error,size,"spec","not allowed for this action");
    yyjson_val *generation=yyjson_obj_get(root,"expected_generation");if(generation&&(!yyjson_is_uint(generation)||!yyjson_get_uint(generation)))return fail(error,size,"expected_generation","must be positive integer");
    u.expected_generation=yyjson_get_uint(generation);u.expected_revision=yyjson_get_uint(revision);
    strcpy(u.operation_id,yyjson_get_str(yyjson_obj_get(root,"operation_id")));strcpy(u.output_id,yyjson_get_str(yyjson_obj_get(target,"output_id")));
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -1;yyjson_mut_val *intent=yyjson_val_mut_copy(d,root);yyjson_mut_doc_set_root(d,intent);yyjson_mut_obj_remove_key(intent,"operation_id");yyjson_mut_obj_add_str(d,intent,"kind","topology");yyjson_mut_obj_add_strcpy(d,intent,"job_id",job);
    size_t bytes;char *text=yyjson_mut_write(d,0,&bytes);yyjson_mut_doc_free(d);yyjson_doc *v=text?yyjson_read(text,bytes,0):NULL;free(text);
    int ret=v?naki_intent_fingerprint(yyjson_doc_get_root(v),u.fingerprint,error,size):-1;if(v)yyjson_doc_free(v);if(!ret)*out=u;return ret;
}
int naki_update_parse(yyjson_val *root,const char *job_id,NakiUpdateRequest *out,char *error,size_t size) {
    NakiUpdateRequest update={0};
    if(!naki_id_valid(job_id,63)||naki_intent_validate(root,error,size))return -1;
    const char *component_name=yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root,"target"),"component"));
    if(component_name&&(!strcmp(component_name,"rendition")||!strcmp(component_name,"destination")))return topology(root,job_id,out,error,size);
    if(keys(root,"|operation_id||target||expected_revision||patch||expected_generation|","update",error,size))return -1;
    yyjson_val *operation=yyjson_obj_get(root,"operation_id"),*target=yyjson_obj_get(root,"target"),*revision=yyjson_obj_get(root,"expected_revision"),*patch=yyjson_obj_get(root,"patch");
    if(!naki_json_id(operation,128))return fail(error,size,"operation_id","invalid control ID");
    if(keys(target,"|output_id||component|","target",error,size)||yyjson_obj_size(target)!=2)return fail(error,size,"target","requires output_id and component");
    yyjson_val *output=yyjson_obj_get(target,"output_id"),*component=yyjson_obj_get(target,"component");
    if(!naki_json_id(output,32))return fail(error,size,"target.output_id","invalid logical output ID");
    if(!yyjson_is_str(component)||strcmp(yyjson_get_str(component),"watermark"))return fail(error,size,"target.component","only watermark is supported");
    if(!yyjson_is_uint(revision)||!yyjson_get_uint(revision)||yyjson_get_uint(revision)>=INT64_MAX)return fail(error,size,"expected_revision","must be 1..INT64_MAX-1");
    if(keys(patch,"|enabled||resource||placement|","patch",error,size))return -1;
    yyjson_val *resource=yyjson_obj_get(patch,"resource"),*placement=yyjson_obj_get(patch,"placement"),*offset=NULL,*dimensions=NULL;
    if(resource&&keys(resource,"|uri||load_timeout_ms|","patch.resource",error,size))return -1;
    if(placement) {
        if(keys(placement,"|anchor||offset||size||opacity|","patch.placement",error,size))return -1;
        offset=yyjson_obj_get(placement,"offset");dimensions=yyjson_obj_get(placement,"size");
        if(offset&&keys(offset,"|x||y|","patch.placement.offset",error,size))return -1;
        if(dimensions&&keys(dimensions,"|width||height||fit|","patch.placement.size",error,size))return -1;
    }
    /* Validate sparse values using the strict full parser with enabled removed;
     * an enable patch may legitimately rely on the existing desired URI. */
    yyjson_mut_doc *validation=yyjson_mut_doc_new(NULL);if(!validation)return fail(error,size,"patch","allocation failure");
    yyjson_mut_val *values=yyjson_val_mut_copy(validation,patch);yyjson_mut_doc_set_root(validation,values);
    yyjson_mut_obj_remove_key(values,"enabled");size_t bytes=0;char *text=yyjson_mut_write(validation,0,&bytes);yyjson_mut_doc_free(validation);
    yyjson_doc *immutable=text?yyjson_read(text,bytes,0):NULL;free(text);
    int parsed=immutable?naki_watermark_config(yyjson_doc_get_root(immutable),&update.patch.values,error,size):-1;
    if(immutable)yyjson_doc_free(immutable);
    if(parsed)return -1;
    yyjson_val *enabled=yyjson_obj_get(patch,"enabled");
    if(enabled&&!yyjson_is_bool(enabled))return fail(error,size,"patch.enabled","expected boolean");
    if(enabled)update.patch.values.enabled=yyjson_get_bool(enabled);
    update.patch.present=present(patch,"enabled",NAKI_PATCH_ENABLED)|present(resource,"uri",NAKI_PATCH_URI)|present(resource,"load_timeout_ms",NAKI_PATCH_LOAD_TIMEOUT)|
        present(placement,"anchor",NAKI_PATCH_ANCHOR)|present(placement,"opacity",NAKI_PATCH_OPACITY)|present(offset,"x",NAKI_PATCH_X)|present(offset,"y",NAKI_PATCH_Y)|
        present(dimensions,"width",NAKI_PATCH_WIDTH)|present(dimensions,"height",NAKI_PATCH_HEIGHT)|present(dimensions,"fit",NAKI_PATCH_FIT);
    yyjson_val *generation=yyjson_obj_get(root,"expected_generation");
    if(generation&&(!yyjson_is_uint(generation)||!yyjson_get_uint(generation)))return fail(error,size,"expected_generation","must be positive integer");
    update.expected_generation=yyjson_get_uint(generation);
    strcpy(update.operation_id,yyjson_get_str(operation));strcpy(update.output_id,yyjson_get_str(output));update.expected_revision=yyjson_get_uint(revision);
    yyjson_mut_doc *intent=yyjson_mut_doc_new(NULL);if(!intent)return fail(error,size,"update","allocation failure");
    yyjson_mut_val *intent_root=yyjson_val_mut_copy(intent,root);yyjson_mut_doc_set_root(intent,intent_root);yyjson_mut_obj_remove_key(intent_root,"operation_id");
    yyjson_mut_obj_add_str(intent,intent_root,"kind","update");yyjson_mut_obj_add_strcpy(intent,intent_root,"job_id",job_id);
    text=yyjson_mut_write(intent,0,&bytes);yyjson_mut_doc_free(intent);immutable=text?yyjson_read(text,bytes,0):NULL;free(text);
    int result=immutable?naki_intent_fingerprint(yyjson_doc_get_root(immutable),update.fingerprint,error,size):-1;
    if(immutable)yyjson_doc_free(immutable);
    if(!result)*out=update;
    return result;
}
int naki_watermark_patch_apply(const NakiWatermarkConfig *desired,const NakiWatermarkPatch *patch,NakiWatermarkConfig *merged,char *error,size_t size) {
    NakiWatermarkConfig value=*desired;const NakiWatermarkConfig *p=&patch->values;unsigned bits=patch->present;
    if(bits&NAKI_PATCH_URI)strcpy(value.uri,p->uri);
#define APPLY(bit,field) if(bits&bit)value.field=p->field
    APPLY(NAKI_PATCH_ENABLED,enabled);APPLY(NAKI_PATCH_LOAD_TIMEOUT,load_timeout_ms);APPLY(NAKI_PATCH_X,x);APPLY(NAKI_PATCH_Y,y);
    APPLY(NAKI_PATCH_WIDTH,width);APPLY(NAKI_PATCH_HEIGHT,height);APPLY(NAKI_PATCH_ANCHOR,anchor);APPLY(NAKI_PATCH_FIT,stretch);APPLY(NAKI_PATCH_OPACITY,opacity);
#undef APPLY
    if(value.required&&!value.enabled)return fail(error,size,"patch.enabled","required watermark cannot be cleared");
    if(value.enabled&&!*value.uri)return fail(error,size,"patch.resource.uri","enabled desired configuration requires a resource");
    *merged=value;return 0;
}
yyjson_mut_val *naki_watermark_config_json(yyjson_mut_doc *d,const NakiWatermarkConfig *c) {
    const char *anchors[]={"top_left","top_center","top_right","center_left","center","center_right","bottom_left","bottom_center","bottom_right"};
    yyjson_mut_val *root=yyjson_mut_obj(d),*resource=yyjson_mut_obj(d),*startup=yyjson_mut_obj(d),*placement=yyjson_mut_obj(d),*offset=yyjson_mut_obj(d),*dimensions=yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d,root,"enabled",c->enabled);if(*c->uri)yyjson_mut_obj_add_strcpy(d,resource,"uri",c->uri);
    yyjson_mut_obj_add_int(d,resource,"load_timeout_ms",c->load_timeout_ms);yyjson_mut_obj_add_val(d,root,"resource",resource);
    yyjson_mut_obj_add_bool(d,startup,"required",c->required);yyjson_mut_obj_add_int(d,startup,"timeout_ms",c->startup_timeout_ms);yyjson_mut_obj_add_val(d,root,"startup",startup);
    yyjson_mut_obj_add_str(d,placement,"anchor",anchors[c->anchor]);yyjson_mut_obj_add_real(d,placement,"opacity",c->opacity);
    yyjson_mut_obj_add_int(d,offset,"x",c->x);yyjson_mut_obj_add_int(d,offset,"y",c->y);yyjson_mut_obj_add_val(d,placement,"offset",offset);
    yyjson_mut_obj_add_int(d,dimensions,"width",c->width);yyjson_mut_obj_add_int(d,dimensions,"height",c->height);yyjson_mut_obj_add_str(d,dimensions,"fit",c->stretch?"stretch":"contain");
    yyjson_mut_obj_add_val(d,placement,"size",dimensions);yyjson_mut_obj_add_val(d,root,"placement",placement);return root;
}
int naki_update_from_legacy(const NakiWatermarkConfig *config,uint64_t revision,const char *job,const char *output,
                            const char *operation,NakiUpdateRequest *out,char *error,size_t size) {
    if(revision<2)return fail(error,size,"revision","must advance initial revision 1");
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -1;
    yyjson_mut_val *root=yyjson_mut_obj(d),*target=yyjson_mut_obj(d),*patch=naki_watermark_config_json(d,config);
    yyjson_mut_doc_set_root(d,root);yyjson_mut_obj_remove_key(patch,"startup");
    yyjson_mut_obj_add_strcpy(d,root,"operation_id",operation);yyjson_mut_obj_add_uint(d,root,"expected_revision",revision-1);
    yyjson_mut_obj_add_strcpy(d,target,"output_id",output);yyjson_mut_obj_add_str(d,target,"component","watermark");
    yyjson_mut_obj_add_val(d,root,"target",target);yyjson_mut_obj_add_val(d,root,"patch",patch);
    size_t bytes=0;char *text=yyjson_mut_write(d,0,&bytes);yyjson_mut_doc_free(d);
    yyjson_doc *immutable=text?yyjson_read(text,bytes,0):NULL;free(text);
    int ret=immutable?naki_update_parse(yyjson_doc_get_root(immutable),job,out,error,size):-1;
    if(immutable)yyjson_doc_free(immutable);
    return ret;
}
