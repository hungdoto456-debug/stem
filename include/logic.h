#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
namespace stem {
inline bool wifiSsid(const char* s, bool allowEmpty) { if (!s) return false; size_t n=strlen(s); if(n>32||(!allowEmpty&&!n)) return false; for(size_t i=0;i<n;i++) if((unsigned char)s[i]<32||s[i]==127)return false; return true; }
inline bool wifiPassword(const char* s, bool allowEmpty) { if(!s)return false; size_t n=strlen(s); if(!n)return allowEmpty; if(n<8||n>63)return false; for(size_t i=0;i<n;i++)if((unsigned char)s[i]<32||(unsigned char)s[i]>126)return false; return true; }
inline float ds3231Temperature(uint8_t msb,uint8_t lsb){return(msb<128?int(msb):int(msb)-256)+(lsb>>6)*.25f;}
inline bool elapsed(uint32_t now,uint32_t then,uint32_t interval){return uint32_t(now-then)>=interval;}
struct Config { uint32_t version=1,on=100,off=200,delay=15000,waterAlert=20,soilAlert=30; uint32_t waterDry=0,waterWet=4095,soilDry=0,soilWet=4095,waterCal=0,soilCal=0; };
inline bool valid(const Config& c){return c.version==1&&c.on<c.off&&c.off<=100000&&c.delay>=1000&&c.delay<=300000&&c.waterAlert<=100&&c.soilAlert<=100&&c.waterDry<=4095&&c.waterWet<=4095&&c.soilDry<=4095&&c.soilWet<=4095&&c.waterDry!=c.waterWet&&c.soilDry!=c.soilWet&&c.waterCal<=1&&c.soilCal<=1;}
struct Blob {uint32_t magic=0x5354454d;Config config;uint32_t checksum=0;};
inline uint32_t checksum(const Blob& b){uint32_t h=2166136261u;const auto*p=reinterpret_cast<const unsigned char*>(&b);for(size_t i=0;i<offsetof(Blob,checksum);i++)h=(h^p[i])*16777619u;return h;}
inline bool valid(const Blob& b){return b.magic==0x5354454d&&valid(b.config)&&b.checksum==checksum(b);}
inline bool number(const char*s,uint32_t max,uint32_t&out){if(!s||!*s)return false;uint32_t n=0;for(;*s;s++){if(*s<'0'||*s>'9')return false;uint32_t d=*s-'0';if(d>max||n>(max-d)/10)return false;n=n*10+d;}out=n;return true;}
inline float percent(float raw,uint32_t dry,uint32_t wet){if(dry==wet)return NAN;float p=100*(raw-float(dry))/(float(wet)-float(dry));return p<0?0:p>100?100:p;}
template<unsigned N>struct Average{float samples[N]={},sum=0;unsigned count=0,index=0;void clear(){*this=Average{};}float add(float x){if(count<N)count++;else sum-=samples[index];samples[index]=x;index=(index+1)%N;sum+=x;return sum/count;}float value()const{return count?sum/count:NAN;}};
struct Lamp{bool state=false,pending=false,target=false;uint32_t since=0;bool update(uint32_t now,bool fresh,float lux,const Config&c){if(!fresh){pending=false;return state;}bool want=state;if(!state&&lux<c.on)want=true;else if(state&&lux>c.off)want=false;if(want==state)pending=false;else if(!pending||want!=target){pending=true;target=want;since=now;}else if(elapsed(now,since,c.delay)){state=want;pending=false;}return state;}};
struct Pump {
  bool on=false, cooling=false;
  uint32_t started=0, stopped=0;
  bool update(uint32_t now, bool enabled, bool fresh, float soil, float threshold) {
    if (cooling && elapsed(now, stopped, 300000)) cooling=false;
    if (on && (!enabled || !fresh || !isfinite(soil) || soil>=threshold+5 || elapsed(now,started,30000))) {
      on=false; cooling=true; stopped=now;
    } else if (!on && !cooling && enabled && fresh && isfinite(soil) && soil<=threshold) {
      on=true; started=now;
    }
    return on;
  }
};
inline bool dateValid(unsigned y,unsigned m,unsigned d,unsigned h,unsigned min,unsigned s){if(y<2000||y>2099||m<1||m>12||h>23||min>59||s>59)return false;const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};return d>=1&&d<=days[m-1]+(m==2&&y%4==0);}
inline bool datetime(const char*s,unsigned*v){if(strlen(s)!=19)return false;const int st[]={0,5,8,11,14,17},ln[]={4,2,2,2,2,2};if(s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':')return false;for(int i=0;i<6;i++){v[i]=0;for(int j=0;j<ln[i];j++){char c=s[st[i]+j];if(c<'0'||c>'9')return false;v[i]=v[i]*10+c-'0';}}return dateValid(v[0],v[1],v[2],v[3],v[4],v[5]);}
}
