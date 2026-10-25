#include "power_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    Nfs4PowerPolicy p={0};
    assert(nfs4_power_cap(&p,60,0,0,1,1000000)==60);
    assert(nfs4_power_cap(&p,60,1,0,1,2000000)==30);
    assert(nfs4_power_cap(&p,60,0,0,1,21999999)==30);
    assert(nfs4_power_cap(&p,60,0,0,1,22000000)==60);
    assert(nfs4_power_cap(&p,60,0,1,1,23000000)==30);
    assert(nfs4_power_cap(&p,30,0,0,1,24000000)==30);
    assert(nfs4_power_cap(&p,60,3,0,1,25000000)==15);
    assert(nfs4_power_cap(&p,60,3,1,0,26000000)==5);
    assert(nfs4_power_cap(&p,60,0,0,1,27000000)==30);
    assert(nfs4_power_cap(&p,60,0,0,1,46000000)==60);
    puts("power_policy: normal/warm/critical/low-power/background and 20s recovery passed");
    return 0;
}
