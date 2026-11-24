#include <assert.h>
#include "../include/logic.h"
// Host: c++ -std=c++11 test/self_check.cpp -o self_check && ./self_check
int main(){
  using namespace stem;
  assert(ds3231Temperature(25, 0x40) == 25.25f);
  assert(ds3231Temperature(0xff, 0xc0) == -0.25f);
  Config c;assert(valid(c));c.off=c.on;assert(!valid(c));c=Config{};
  Blob b;b.checksum=checksum(b);assert(valid(b));++b.config.on;assert(!valid(b));
  uint32_t n=99;assert(number("0",4095,n)&&n==0);assert(number("4095",4095,n)&&n==4095);
  for(const char* s:{"","-1","+1","1.0","2x"," 2","4096","999999999999999"})assert(!number(s,4095,n));
  assert(percent(25,0,100)==25&&percent(25,100,0)==75&&percent(200,0,100)==100&&isnan(percent(1,1,1)));
  Average<3> a;assert(isnan(a.value()));assert(a.add(9)==9);assert(a.add(3)==6);assert(a.add(6)==6);assert(a.add(12)==7);a.clear();assert(isnan(a.value()));
  assert(elapsed(0x10,0xfffffff0,32)&&!elapsed(0x10,0xfffffff0,33));
  assert(wifiSsid("", true)&&!wifiSsid("", false)&&wifiSsid("my-network", false));
  assert(!wifiSsid("bad\nssid", false)&&!wifiSsid("123456789012345678901234567890123", false));
  assert(wifiPassword("", true)&&wifiPassword("12345678", false));
  assert(!wifiPassword("short", false)&&!wifiPassword("bad\npass", false));
  c.delay=1000;Lamp l;assert(!l.update(0xfffffff0,true,0,c));assert(!l.update(500,true,0,c));assert(l.update(1000,true,0,c));
  assert(l.update(1100,true,300,c));assert(l.update(2100,false,0,c));assert(!l.pending&&l.state);
  assert(l.update(2200,true,300,c));assert(l.update(3199,true,300,c));assert(!l.update(3200,true,300,c));
  Lamp outage;outage.update(0,true,0,c);outage.update(999,false,0,c);assert(!outage.pending);assert(!outage.update(1000,true,0,c));assert(!outage.update(1999,true,0,c));assert(outage.update(2000,true,0,c));
  unsigned v[6];assert(datetime("2024-02-29T23:59:59",v));assert(!datetime("2025-02-29T23:59:59",v));assert(!datetime("2024-04-31T00:00:00",v));assert(!datetime("2024-01-01T24:00:00",v));assert(!datetime("2024-01-01T00:00",v));
}
