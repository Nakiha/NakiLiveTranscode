#include <string.h>
#include <stddef.h>
int naki_id_valid(const char *value,size_t maximum) {
    if(!value||!maximum)return 0;
    size_t n=strnlen(value,maximum+1);if(!n||n>maximum)return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)value[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
    }
    return 1;
}
