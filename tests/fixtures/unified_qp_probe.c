#include "media/unified_rate.h"
#include <stdio.h>
#include <inttypes.h>
int main(void) {
    double quality,at32,bias;int64_t quota,config,predicted;int qp;
    while(scanf("%lf %lf %"SCNd64" %"SCNd64" %lf",&quality,&at32,&quota,&config,&bias)==5) {
        int ret=naki_unified_request_qp(quality,at32,quota,config,bias,&qp,&predicted);
        if(ret<0)return 1;
        printf("%d %"PRId64"\n",qp,predicted);
    }
    return ferror(stdin)?1:0;
}
