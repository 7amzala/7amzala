#define SIM_NO_MAIN
#include "scenarios.cpp"
int main(){
  Serial.quiet = true; verbose = false;
  setup();
  Env calm; calm.waveA = 0.15; calm.waveP = 5; calm.windA = 1;
  Env mod; mod.waveA = 1.0; mod.waveP = 6; mod.windA = 5; mod.windVib = 0.01; mod.tideRate = 0.5;
  Env rough; rough.waveA = 2.5; rough.waveP = 8; rough.windA = 12; rough.windVib = 0.02; rough.tideRate = 1.0;
  Env vrough = rough; vrough.waveA = 4; vrough.waveP = 10;
  Env envs[4] = {calm, mod, rough, vrough}; const char* names[4] = {"calm","moderate","rough","very rough"};
  for (int seed = 1; seed <= 3; seed++) for (int k = 0; k < 4; k++) {
    rng.seed(seed * 100 + k); env = envs[k]; events.clear(); strikes.clear();
    startEnvironmentLearning(); run(15.5); pullTest(simMillis/1000.0 + 1);
    for (int i = 0; i < 30 && rodState == ST_PULLTEST; i++) run(0.5);
    bool ok = rodState != ST_PULLTEST; if (!ok) handleSkipPull();
    size_t b = strikes.size(); int kb = knockAlerts;
    run(7200);
    printf("seed %d %-11s pulltest=%s  2h no fish: false strikes=%zu, knock beeps=%d\n", seed, names[k], ok ? "ok  " : "SKIP", strikes.size() - b, knockAlerts - kb);
  }
  // weather change: calm -> rough over 10 minutes, then back
  rng.seed(7); env = calm; events.clear(); startEnvironmentLearning(); run(15.5); pullTest(simMillis/1000.0+1); run(5);
  size_t b = strikes.size(); int kb = knockAlerts; double T0 = simMillis/1000.0;
  run(600);
  for (int i = 0; i <= 60; i++) { float f = i / 60.0; env.waveA = calm.waveA + f * (rough.waveA - calm.waveA); env.waveP = 5 + 3 * f; env.windA = 1 + 11 * f; env.windVib = 0.02 * f; run(10); if(i%6==0) printf("up %3.0f%% t=%5.0f kb=%d thr=%.4f gust=%.4f mean=%.4f swing=%.2f fill=%.2f bucket=%.2f allow=%.2f sea=%d\n", f*100, simMillis/1000.0-T0, knockAlerts-kb, knockThreshold, gustNoise, knockMean, waveSwing, waveFill, bucketSize, pullAllow, seaState); }
  run(1800);
  for (int i = 0; i <= 60; i++) { float f = 1 - i / 60.0; env.waveA = calm.waveA + f * (rough.waveA - calm.waveA); env.waveP = 5 + 3 * f; env.windA = 1 + 11 * f; env.windVib = 0.02 * f; run(10); }
  run(1800);
  printf("weather change calm->rough->calm (~2h): false strikes=%zu knock beeps=%d\n", strikes.size() - b, knockAlerts - kb);
  for (size_t i = b; i < strikes.size(); i++) printf("   %s at +%.0fs\n", strikes[i].type.c_str(), strikes[i].t - T0);
}
