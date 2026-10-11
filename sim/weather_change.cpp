#define SIM_NO_MAIN
#include "scenarios.cpp"
#include <map>
Env calm, rough;
void ramp(float from, float to, double secs){ int n = secs/10; for(int i=0;i<=n;i++){ float f = from + (to-from)*i/(float)n; env.waveA = calm.waveA + f*(rough.waveA-calm.waveA); env.waveP = 5+3*f; env.windA = 1+11*f; env.windVib = 0.02*f; env.tideRate = f; run(10);} }
int main(){
  Serial.quiet = true; verbose = false; setup();
  calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  rough.waveA = 2.5; rough.waveP = 8; rough.windA = 12; rough.windVib = 0.02; rough.tideRate = 1.0;
  for(int seed=1; seed<=6; seed++){
    rng.seed(seed*31); env = calm; events.clear(); strikes.clear(); knockAlerts=0;
    startEnvironmentLearning(); run(15.5); pullTest(simMillis/1000.0+1); for(int i=0;i<30&&rodState==ST_PULLTEST;i++) run(0.5); if(rodState==ST_PULLTEST) handleSkipPull();
    const char* seg[4]={"ramp up 10min","rough 30min","ramp down 10min","calm 30min"}; int s[4], k[4];
    size_t b=strikes.size(); int kb=knockAlerts; run(300);
    ramp(0,1,600); s[0]=strikes.size()-b; k[0]=knockAlerts-kb; b=strikes.size(); kb=knockAlerts;
    ramp(1,1,1800); s[1]=strikes.size()-b; k[1]=knockAlerts-kb; b=strikes.size(); kb=knockAlerts;
    ramp(1,0,600); s[2]=strikes.size()-b; k[2]=knockAlerts-kb; b=strikes.size(); kb=knockAlerts;
    ramp(0,0,1800); s[3]=strikes.size()-b; k[3]=knockAlerts-kb;
    printf("seed %d:", seed); for(int i=0;i<4;i++) printf("  %s: %d strikes/%d beeps |", seg[i], s[i], k[i]); printf("\n"); std::map<std::string,int> m; for(auto&x:strikes) m[x.type]++; for(auto&kv:m) printf("     %s x%d\n", kv.first.c_str(), kv.second);
  }
}
